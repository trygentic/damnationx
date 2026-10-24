/**
 * @file xr/xr_bridge_vulkan_android.cpp
 *
 * Quest graphics bridge (docs/design/vr.md, "Graphics bridge"): a small Vulkan device created through
 * XR_KHR_vulkan_enable2 owns the OpenXR session. Each eye is an AHardwareBuffer imported both into that device (as
 * a VkImage) and into Dawn (SharedTextureMemoryAHardwareBuffer). Per frame: Dawn renders the eye, EndAccess hands
 * back a sync fd, the bridge imports it as a semaphore, copies the eye into the swapchain image and releases the
 * buffer back to Dawn (its fence is waited before the next BeginAccess).
 */
#include "xr/xr_openxr_platform.hpp"

#ifdef DEVILUTIONX_XR_BRIDGE_VULKAN

#include <algorithm>
#include <cstring>
#include <string>

#include <android/hardware_buffer.h>
#include <poll.h>
#include <unistd.h>

#include "utils/log.hpp"

namespace devilution::xr {

namespace {

constexpr VkFormat kEyeFormat = VK_FORMAT_R8G8B8A8_UNORM;

class VulkanAndroidBridge final : public GraphicsBridge {
public:
	[[nodiscard]] const char *Name() const override
	{
		return "Vulkan (Android)";
	}

	[[nodiscard]] const char *RequiredExtension() const override
	{
		return XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
	}

	[[nodiscard]] std::vector<const char *> OptionalExtensions() const override
	{
		// The original vulkan_enable adds the runtime's required device extensions (see CreateDevice).
		return { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
	}

	bool CreateDevice(XrInstance instance, XrSystemId system, const std::vector<const char *> &enabledExtensions) override
	{
		xrInstance_ = instance;
		PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
		PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
		PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
		PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
		if (!Load(instance, "xrGetVulkanGraphicsRequirements2KHR", getRequirements) || !Load(instance, "xrCreateVulkanInstanceKHR", createInstance)
		    || !Load(instance, "xrGetVulkanGraphicsDevice2KHR", getDevice) || !Load(instance, "xrCreateVulkanDeviceKHR", createDevice))
			return false;

		XrGraphicsRequirementsVulkan2KHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
		if (XR_FAILED(getRequirements(instance, system, &requirements)))
			return false;

		VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.pApplicationName = "DamnationX VR bridge";
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo instanceInfo { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		instanceInfo.pApplicationInfo = &app;
		XrVulkanInstanceCreateInfoKHR xrInstanceInfo { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
		xrInstanceInfo.systemId = system;
		xrInstanceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
		xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
		VkResult vkResult = VK_SUCCESS;
		if (XR_FAILED(createInstance(instance, &xrInstanceInfo, &vkInstance_, &vkResult)) || vkResult != VK_SUCCESS) {
			LogError("VR bridge: Vulkan instance creation failed ({})", static_cast<int>(vkResult));
			return false;
		}

		XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
		deviceGetInfo.systemId = system;
		deviceGetInfo.vulkanInstance = vkInstance_;
		if (XR_FAILED(getDevice(instance, &deviceGetInfo, &physicalDevice_)))
			return false;

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
		queueFamily_ = UINT32_MAX;
		for (uint32_t i = 0; i < familyCount; ++i) {
			if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
				queueFamily_ = i;
				break;
			}
		}
		if (queueFamily_ == UINT32_MAX)
			return false;

		// Device extensions: AHB import, foreign queue family, sync fd semaphores, plus what the runtime asks for.
		uint32_t extensionCount = 0;
		vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> available(extensionCount);
		vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, available.data());
		const auto has = [&](const char *name) {
			return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties &p) { return std::strcmp(p.extensionName, name) == 0; });
		};
		std::vector<std::string> wanted = {
			VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
			VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
			VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
			VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
			VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
		};
		const bool legacyEnable = std::any_of(enabledExtensions.begin(), enabledExtensions.end(),
		    [](const char *name) { return std::strcmp(name, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0; });
		PFN_xrGetVulkanDeviceExtensionsKHR getDeviceExtensions = nullptr;
		if (legacyEnable && Load(instance, "xrGetVulkanDeviceExtensionsKHR", getDeviceExtensions)) {
			uint32_t size = 0;
			if (XR_SUCCEEDED(getDeviceExtensions(instance, system, 0, &size, nullptr)) && size > 0) {
				std::string list(size, '\0');
				if (XR_SUCCEEDED(getDeviceExtensions(instance, system, size, &size, list.data()))) {
					size_t start = 0;
					while (start < list.size()) {
						size_t end = list.find(' ', start);
						if (end == std::string::npos)
							end = list.size();
						std::string name = list.substr(start, end - start);
						name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
						if (!name.empty())
							wanted.push_back(name);
						start = end + 1;
					}
				}
			}
		}
		std::vector<std::string> unique;
		for (const std::string &name : wanted) {
			if (has(name.c_str()) && std::find(unique.begin(), unique.end(), name) == unique.end())
				unique.push_back(name);
		}
		if (std::find(unique.begin(), unique.end(), std::string(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) == unique.end()) {
			LogError("VR bridge: the GPU lacks {}", VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
			return false;
		}
		std::vector<const char *> names;
		for (const std::string &name : unique)
			names.push_back(name.c_str());

		const float priority = 1.0F;
		VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueInfo.queueFamilyIndex = queueFamily_;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;
		VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = static_cast<uint32_t>(names.size());
		deviceInfo.ppEnabledExtensionNames = names.data();
		XrVulkanDeviceCreateInfoKHR xrDeviceInfo { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
		xrDeviceInfo.systemId = system;
		xrDeviceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
		xrDeviceInfo.vulkanPhysicalDevice = physicalDevice_;
		xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
		if (XR_FAILED(createDevice(instance, &xrDeviceInfo, &device_, &vkResult)) || vkResult != VK_SUCCESS) {
			LogError("VR bridge: Vulkan device creation failed ({})", static_cast<int>(vkResult));
			return false;
		}
		vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
		getAhbProperties_ = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
		    vkGetDeviceProcAddr(device_, "vkGetAndroidHardwareBufferPropertiesANDROID"));
		importSemaphoreFd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
		if (getAhbProperties_ == nullptr)
			return false;

		VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = queueFamily_;
		if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS)
			return false;

		binding_.instance = vkInstance_;
		binding_.physicalDevice = physicalDevice_;
		binding_.device = device_;
		binding_.queueFamilyIndex = queueFamily_;
		binding_.queueIndex = 0;
		Log("VR bridge: Vulkan device ready ({} extensions)", names.size());
		return true;
	}

	[[nodiscard]] const void *SessionBinding() const override
	{
		return &binding_;
	}

	[[nodiscard]] std::vector<WGPUFeatureName> DeviceFeatures() const override
	{
		return { WGPUFeatureName_SharedTextureMemoryAHardwareBuffer, WGPUFeatureName_SharedFenceSyncFD };
	}

	[[nodiscard]] std::vector<int64_t> PreferredSwapchainFormats() const override
	{
		return { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM };
	}

	bool CreateEyes(const std::array<XrSwapchain, 2> &swapchains, int64_t swapchainFormat, uint32_t width, uint32_t height,
	    WGPUDevice device, WGPUTextureFormat colorFormat) override
	{
		if (colorFormat != WGPUTextureFormat_RGBA8Unorm) {
			LogError("VR bridge: the presenter format {} is not RGBA8Unorm", static_cast<int>(colorFormat));
			return false;
		}
		width_ = width;
		height_ = height;
		swapchainFormat_ = static_cast<VkFormat>(swapchainFormat);
		// A raw copy needs the same byte order; BGRA swapchains get a blit (which converts).
		useBlit_ = swapchainFormat_ == VK_FORMAT_B8G8R8A8_SRGB || swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM;
		for (int index = 0; index < 2; ++index) {
			Eye &eye = eyes_[index];
			uint32_t count = 0;
			if (XR_FAILED(xrEnumerateSwapchainImages(swapchains[index], 0, &count, nullptr)))
				return false;
			std::vector<XrSwapchainImageVulkan2KHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
			if (XR_FAILED(xrEnumerateSwapchainImages(swapchains[index], count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()))))
				return false;
			for (const XrSwapchainImageVulkan2KHR &image : images)
				eye.swapchainImages.push_back(image.image);
			if (!CreateSharedImage(eye, device))
				return false;

			VkCommandBufferAllocateInfo allocateInfo { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			allocateInfo.commandPool = commandPool_;
			allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocateInfo.commandBufferCount = 1;
			VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			VkSemaphoreCreateInfo semaphoreInfo { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			if (vkAllocateCommandBuffers(device_, &allocateInfo, &eye.commands) != VK_SUCCESS
			    || vkCreateFence(device_, &fenceInfo, nullptr, &eye.fence) != VK_SUCCESS
			    || vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &eye.dawnDone) != VK_SUCCESS)
				return false;
		}
		return true;
	}

	WGPUTextureView BeginEye(int index) override
	{
		Eye &eye = eyes_[index];
		if (eye.fencePending) {
			vkWaitForFences(device_, 1, &eye.fence, VK_TRUE, UINT64_MAX);
			vkResetFences(device_, 1, &eye.fence);
			eye.fencePending = false;
		}
		WGPUSharedTextureMemoryVkImageLayoutBeginState layouts = WGPU_SHARED_TEXTURE_MEMORY_VK_IMAGE_LAYOUT_BEGIN_STATE_INIT;
		layouts.oldLayout = eye.releasedOld;
		layouts.newLayout = eye.releasedNew;
		WGPUSharedTextureMemoryBeginAccessDescriptor begin = WGPU_SHARED_TEXTURE_MEMORY_BEGIN_ACCESS_DESCRIPTOR_INIT;
		begin.nextInChain = &layouts.chain;
		begin.initialized = false; // every frame clears the whole eye
		if (wgpuSharedTextureMemoryBeginAccess(eye.memory, eye.texture, &begin) != WGPUStatus_Success) {
			LogError("VR bridge: BeginAccess failed");
			return nullptr;
		}
		return eye.view;
	}

	bool EndEye(int index, bool copy, uint32_t imageIndex) override
	{
		Eye &eye = eyes_[index];
		WGPUSharedTextureMemoryVkImageLayoutEndState layouts = WGPU_SHARED_TEXTURE_MEMORY_VK_IMAGE_LAYOUT_END_STATE_INIT;
		WGPUSharedTextureMemoryEndAccessState end = WGPU_SHARED_TEXTURE_MEMORY_END_ACCESS_STATE_INIT;
		end.nextInChain = &layouts.chain;
		if (wgpuSharedTextureMemoryEndAccess(eye.memory, eye.texture, &end) != WGPUStatus_Success) {
			LogError("VR bridge: EndAccess failed");
			return false;
		}
		// Dawn's release: the fences to wait for and the layouts of its queue family release barrier.
		bool waitSemaphore = false;
		for (size_t i = 0; i < end.fenceCount; ++i) {
			WGPUSharedFenceSyncFDExportInfo fd = WGPU_SHARED_FENCE_SYNC_FD_EXPORT_INFO_INIT;
			WGPUSharedFenceExportInfo info = WGPU_SHARED_FENCE_EXPORT_INFO_INIT;
			info.nextInChain = &fd.chain;
			wgpuSharedFenceExportInfo(end.fences[i], &info);
			if (fd.handle < 0)
				continue; // already signalled
			if (!waitSemaphore && importSemaphoreFd_ != nullptr) {
				VkImportSemaphoreFdInfoKHR import { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR };
				import.semaphore = eye.dawnDone;
				import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
				import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
				import.fd = dup(fd.handle); // the import takes ownership; Dawn keeps its own
				if (import.fd >= 0 && importSemaphoreFd_(device_, &import) == VK_SUCCESS) {
					waitSemaphore = true;
					continue;
				}
				if (import.fd >= 0)
					close(import.fd);
			}
			pollfd poller { fd.handle, POLLIN, 0 }; // fallback: wait on the CPU
			poll(&poller, 1, 1000);
		}
		const VkImageLayout dawnOld = static_cast<VkImageLayout>(layouts.oldLayout);
		VkImageLayout dawnNew = static_cast<VkImageLayout>(layouts.newLayout);
		if (dawnNew == VK_IMAGE_LAYOUT_UNDEFINED) // no transition requested: the image stays in its layout
			dawnNew = dawnOld != VK_IMAGE_LAYOUT_UNDEFINED ? dawnOld : VK_IMAGE_LAYOUT_GENERAL;
		wgpuSharedTextureMemoryEndAccessStateFreeMembers(end);

		if (!copy) {
			// Nothing to copy: hand the image straight back (the same layouts Dawn released it with).
			eye.releasedOld = dawnOld;
			eye.releasedNew = dawnNew;
			if (waitSemaphore)
				SubmitEmptyWait(eye);
			return true;
		}

		vkResetCommandBuffer(eye.commands, 0);
		VkCommandBufferBeginInfo beginInfo { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(eye.commands, &beginInfo);

		const VkImage swapImage = eye.swapchainImages[std::min<size_t>(imageIndex, eye.swapchainImages.size() - 1)];
		// 1. Acquire the eye from Dawn (foreign queue family), 2. to transfer source; swapchain image to transfer destination.
		std::array<VkImageMemoryBarrier, 2> barriers {};
		barriers[0] = Barrier(eye.image, dawnOld, dawnNew, VK_QUEUE_FAMILY_FOREIGN_EXT, queueFamily_, 0, VK_ACCESS_TRANSFER_READ_BIT);
		barriers[1] = Barrier(swapImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, 0,
		    VK_ACCESS_TRANSFER_WRITE_BIT);
		vkCmdPipelineBarrier(eye.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
		    static_cast<uint32_t>(barriers.size()), barriers.data());
		VkImageMemoryBarrier toSource = Barrier(eye.image, dawnNew, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
		    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		vkCmdPipelineBarrier(eye.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSource);

		if (useBlit_) {
			VkImageBlit blit {};
			blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blit.srcOffsets[1] = { static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1 };
			blit.dstOffsets[1] = { static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1 };
			vkCmdBlitImage(eye.commands, eye.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
			    VK_FILTER_NEAREST);
		} else {
			// UNORM -> SRGB of the same layout is a bit copy: the runtime then decodes the game's sRGB colours.
			VkImageCopy region {};
			region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.extent = { width_, height_, 1 };
			vkCmdCopyImage(eye.commands, eye.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}

		// Swapchain image back to colour attachment (what the runtime expects on release); the eye back to Dawn.
		barriers[0] = Barrier(swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
		    VK_QUEUE_FAMILY_IGNORED, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
		barriers[1] = Barrier(eye.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, queueFamily_, VK_QUEUE_FAMILY_FOREIGN_EXT,
		    VK_ACCESS_TRANSFER_READ_BIT, 0);
		vkCmdPipelineBarrier(eye.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
		    static_cast<uint32_t>(barriers.size()), barriers.data());
		vkEndCommandBuffer(eye.commands);
		eye.releasedOld = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		eye.releasedNew = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.waitSemaphoreCount = waitSemaphore ? 1 : 0;
		submit.pWaitSemaphores = &eye.dawnDone;
		submit.pWaitDstStageMask = &waitStage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &eye.commands;
		if (vkQueueSubmit(queue_, 1, &submit, eye.fence) != VK_SUCCESS) {
			LogError("VR bridge: copy submit failed");
			return false;
		}
		eye.fencePending = true;
		return true;
	}

	void Destroy() override
	{
		if (device_ != VK_NULL_HANDLE)
			vkDeviceWaitIdle(device_);
		for (Eye &eye : eyes_) {
			if (eye.view != nullptr)
				wgpuTextureViewRelease(eye.view);
			if (eye.texture != nullptr)
				wgpuTextureRelease(eye.texture);
			if (eye.memory != nullptr)
				wgpuSharedTextureMemoryRelease(eye.memory);
			if (device_ != VK_NULL_HANDLE) {
				if (eye.fence != VK_NULL_HANDLE)
					vkDestroyFence(device_, eye.fence, nullptr);
				if (eye.dawnDone != VK_NULL_HANDLE)
					vkDestroySemaphore(device_, eye.dawnDone, nullptr);
				if (eye.image != VK_NULL_HANDLE)
					vkDestroyImage(device_, eye.image, nullptr);
				if (eye.imageMemory != VK_NULL_HANDLE)
					vkFreeMemory(device_, eye.imageMemory, nullptr);
			}
			if (eye.buffer != nullptr)
				AHardwareBuffer_release(eye.buffer);
			eye = Eye {};
		}
		if (commandPool_ != VK_NULL_HANDLE)
			vkDestroyCommandPool(device_, commandPool_, nullptr);
		commandPool_ = VK_NULL_HANDLE;
		if (device_ != VK_NULL_HANDLE)
			vkDestroyDevice(device_, nullptr);
		device_ = VK_NULL_HANDLE;
		if (vkInstance_ != VK_NULL_HANDLE)
			vkDestroyInstance(vkInstance_, nullptr);
		vkInstance_ = VK_NULL_HANDLE;
	}

private:
	struct Eye {
		AHardwareBuffer *buffer = nullptr;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory imageMemory = VK_NULL_HANDLE;
		WGPUSharedTextureMemory memory = nullptr;
		WGPUTexture texture = nullptr;
		WGPUTextureView view = nullptr;
		std::vector<VkImage> swapchainImages;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool fencePending = false;
		VkSemaphore dawnDone = VK_NULL_HANDLE;
		// The layouts of the last release to Dawn (its BeginAccess acquire must use the same).
		VkImageLayout releasedOld = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImageLayout releasedNew = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	template <typename T>
	static bool Load(XrInstance instance, const char *name, T &function)
	{
		return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(&function))) && function != nullptr;
	}

	static VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, uint32_t srcFamily, uint32_t dstFamily,
	    VkAccessFlags srcAccess, VkAccessFlags dstAccess)
	{
		VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		barrier.oldLayout = from;
		barrier.newLayout = to;
		barrier.srcQueueFamilyIndex = srcFamily;
		barrier.dstQueueFamilyIndex = dstFamily;
		barrier.image = image;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		return barrier;
	}

	void SubmitEmptyWait(Eye &eye)
	{
		// Consume the imported semaphore payload so the next import starts clean.
		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &eye.dawnDone;
		submit.pWaitDstStageMask = &waitStage;
		if (vkQueueSubmit(queue_, 1, &submit, eye.fence) == VK_SUCCESS)
			eye.fencePending = true;
	}

	bool CreateSharedImage(Eye &eye, WGPUDevice device)
	{
		AHardwareBuffer_Desc description {};
		description.width = width_;
		description.height = height_;
		description.layers = 1;
		description.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
		description.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
		if (AHardwareBuffer_allocate(&description, &eye.buffer) != 0) {
			LogError("VR bridge: AHardwareBuffer_allocate failed");
			return false;
		}

		// Vulkan side.
		VkAndroidHardwareBufferPropertiesANDROID properties { VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
		if (getAhbProperties_(device_, eye.buffer, &properties) != VK_SUCCESS)
			return false;
		VkExternalMemoryImageCreateInfo external { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
		VkImageCreateInfo imageInfo { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		imageInfo.pNext = &external;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = kEyeFormat;
		imageInfo.extent = { width_, height_, 1 };
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(device_, &imageInfo, nullptr, &eye.image) != VK_SUCCESS)
			return false;
		VkImportAndroidHardwareBufferInfoANDROID import { VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID };
		import.buffer = eye.buffer;
		VkMemoryDedicatedAllocateInfo dedicated { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		dedicated.pNext = &import;
		dedicated.image = eye.image;
		VkMemoryAllocateInfo allocateInfo { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		allocateInfo.pNext = &dedicated;
		allocateInfo.allocationSize = properties.allocationSize;
		allocateInfo.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(properties.memoryTypeBits));
		if (vkAllocateMemory(device_, &allocateInfo, nullptr, &eye.imageMemory) != VK_SUCCESS
		    || vkBindImageMemory(device_, eye.image, eye.imageMemory, 0) != VK_SUCCESS) {
			LogError("VR bridge: AHardwareBuffer import into Vulkan failed");
			return false;
		}

		// Dawn side.
		WGPUSharedTextureMemoryAHardwareBufferDescriptor ahb = WGPU_SHARED_TEXTURE_MEMORY_A_HARDWARE_BUFFER_DESCRIPTOR_INIT;
		ahb.handle = eye.buffer;
		WGPUSharedTextureMemoryDescriptor memoryDescriptor = WGPU_SHARED_TEXTURE_MEMORY_DESCRIPTOR_INIT;
		memoryDescriptor.nextInChain = &ahb.chain;
		memoryDescriptor.label = { "DamnationX VR eye", WGPU_STRLEN };
		eye.memory = wgpuDeviceImportSharedTextureMemory(device, &memoryDescriptor);
		if (eye.memory == nullptr)
			return false;
		WGPUTextureDescriptor textureDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
		textureDescriptor.label = { "DamnationX VR eye", WGPU_STRLEN };
		textureDescriptor.usage = WGPUTextureUsage_RenderAttachment;
		textureDescriptor.dimension = WGPUTextureDimension_2D;
		textureDescriptor.size = { width_, height_, 1 };
		textureDescriptor.format = WGPUTextureFormat_RGBA8Unorm;
		textureDescriptor.mipLevelCount = 1;
		eye.texture = wgpuSharedTextureMemoryCreateTexture(eye.memory, &textureDescriptor);
		if (eye.texture == nullptr)
			return false;
		eye.view = wgpuTextureCreateView(eye.texture, nullptr);
		return eye.view != nullptr;
	}

	XrInstance xrInstance_ = XR_NULL_HANDLE;
	VkInstance vkInstance_ = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t queueFamily_ = 0;
	VkCommandPool commandPool_ = VK_NULL_HANDLE;
	PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties_ = nullptr;
	PFN_vkImportSemaphoreFdKHR importSemaphoreFd_ = nullptr;
	XrGraphicsBindingVulkan2KHR binding_ { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
	std::array<Eye, 2> eyes_ {};
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
	bool useBlit_ = false;
};

} // namespace

GraphicsBridge *CreateGraphicsBridge()
{
	return new VulkanAndroidBridge();
}

} // namespace devilution::xr

#endif // DEVILUTIONX_XR_BRIDGE_VULKAN
