/**
 * @file xr/xr_bridge_metal.mm
 *
 * macOS graphics bridge (docs/design/vr.md, "macOS"): for OpenXR runtimes with XR_KHR_metal_enable, such as OXRSys
 * (formerly OpenXR-OSX), which streams to a Quest. The session is created with a command queue on the runtime's
 * MTLDevice. Each eye is an IOSurface imported into Dawn (SharedTextureMemoryIOSurface) and wrapped as an
 * MTLTexture; MTLSharedEvents order Dawn's rendering, the bridge's blit into the swapchain image and Dawn's next
 * access. The blit goes through an sRGB view of the eye, so the runtime decodes the game's sRGB colours.
 */
#include "xr/xr_openxr_platform.hpp"

#ifdef DEVILUTIONX_XR_BRIDGE_METAL

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <dawn/native/MetalBackend.h>

#include "utils/log.hpp"

namespace devilution::xr {

namespace {

class MetalBridge final : public GraphicsBridge {
public:
	[[nodiscard]] const char *Name() const override
	{
		return "Metal";
	}

	[[nodiscard]] const char *RequiredExtension() const override
	{
		return XR_KHR_METAL_ENABLE_EXTENSION_NAME;
	}

	[[nodiscard]] WGPUTextureFormat ColorFormat() const override
	{
		return WGPUTextureFormat_BGRA8Unorm; // CAMetalLayer has no RGBA8: the presenter keeps BGRA on macOS
	}

	bool CreateDevice(XrInstance instance, XrSystemId system, const std::vector<const char *> &) override
	{
		PFN_xrGetMetalGraphicsRequirementsKHR getRequirements = nullptr;
		if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetMetalGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements)))
		    || getRequirements == nullptr)
			return false;
		XrGraphicsRequirementsMetalKHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR };
		if (XR_FAILED(getRequirements(instance, system, &requirements)) || requirements.metalDevice == nullptr)
			return false;
		device_ = (__bridge id<MTLDevice>)requirements.metalDevice;
		queue_ = [device_ newCommandQueue];
		copyEvent_ = [device_ newSharedEvent];
		if (queue_ == nil || copyEvent_ == nil)
			return false;
		binding_.commandQueue = (__bridge void *)queue_;
		Log("VR bridge: Metal device {}", [[device_ name] UTF8String]);
		return true;
	}

	[[nodiscard]] const void *SessionBinding() const override
	{
		return &binding_;
	}

	[[nodiscard]] std::vector<WGPUFeatureName> DeviceFeatures() const override
	{
		return { WGPUFeatureName_SharedTextureMemoryIOSurface, WGPUFeatureName_SharedFenceMTLSharedEvent };
	}

	[[nodiscard]] std::vector<int64_t> PreferredSwapchainFormats() const override
	{
		return { MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatBGRA8Unorm };
	}

	bool CreateEyes(const std::array<XrSwapchain, 2> &swapchains, int64_t swapchainFormat, uint32_t width, uint32_t height, WGPUDevice device,
	    WGPUTextureFormat colorFormat) override
	{
		if (colorFormat != WGPUTextureFormat_BGRA8Unorm) {
			LogError("VR bridge: the presenter format {} is not BGRA8Unorm", static_cast<int>(colorFormat));
			return false;
		}
		if (dawn::native::metal::GetMTLDevice(device) != device_)
			LogError("VR bridge: Dawn uses another GPU than the headset runtime; the eye textures are shared across GPUs");
		width_ = width;
		height_ = height;
		copyFormat_ = static_cast<MTLPixelFormat>(swapchainFormat);

		WGPUSharedFenceMTLSharedEventDescriptor eventDescriptor = WGPU_SHARED_FENCE_MTL_SHARED_EVENT_DESCRIPTOR_INIT;
		eventDescriptor.sharedEvent = (__bridge void *)copyEvent_;
		WGPUSharedFenceDescriptor fenceDescriptor = WGPU_SHARED_FENCE_DESCRIPTOR_INIT;
		fenceDescriptor.nextInChain = &eventDescriptor.chain;
		dawnCopyFence_ = wgpuDeviceImportSharedFence(device, &fenceDescriptor);
		if (dawnCopyFence_ == nullptr)
			return false;

		for (int index = 0; index < 2; ++index) {
			Eye &eye = eyes_[index];
			uint32_t count = 0;
			if (XR_FAILED(xrEnumerateSwapchainImages(swapchains[index], 0, &count, nullptr)))
				return false;
			std::vector<XrSwapchainImageMetalKHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR });
			if (XR_FAILED(xrEnumerateSwapchainImages(swapchains[index], count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()))))
				return false;
			for (const XrSwapchainImageMetalKHR &image : images)
				eye.swapchainImages.push_back((__bridge id<MTLTexture>)image.texture);

			NSDictionary *properties = @{
				(id)kIOSurfaceWidth : @(width),
				(id)kIOSurfaceHeight : @(height),
				(id)kIOSurfaceBytesPerElement : @4,
				(id)kIOSurfacePixelFormat : @(static_cast<uint32_t>('BGRA')),
			};
			eye.surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
			if (eye.surface == nullptr)
				return false;
			MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                                                                                     width:width
			                                                                                    height:height
			                                                                                 mipmapped:NO];
			descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
			descriptor.storageMode = MTLStorageModeShared;
			id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor iosurface:eye.surface plane:0];
			if (texture == nil)
				return false;
			// The blit source matches the swapchain's format; sRGB <-> UNORM views reinterpret the same bytes.
			eye.copySource = copyFormat_ == MTLPixelFormatBGRA8Unorm ? texture : [texture newTextureViewWithPixelFormat:copyFormat_];

			WGPUSharedTextureMemoryIOSurfaceDescriptor surface = WGPU_SHARED_TEXTURE_MEMORY_IO_SURFACE_DESCRIPTOR_INIT;
			surface.ioSurface = eye.surface;
			WGPUSharedTextureMemoryDescriptor memoryDescriptor = WGPU_SHARED_TEXTURE_MEMORY_DESCRIPTOR_INIT;
			memoryDescriptor.nextInChain = &surface.chain;
			memoryDescriptor.label = { "DamnationX VR eye", WGPU_STRLEN };
			eye.memory = wgpuDeviceImportSharedTextureMemory(device, &memoryDescriptor);
			if (eye.memory == nullptr)
				return false;
			WGPUTextureDescriptor textureDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
			textureDescriptor.usage = WGPUTextureUsage_RenderAttachment;
			textureDescriptor.dimension = WGPUTextureDimension_2D;
			textureDescriptor.size = { width, height, 1 };
			textureDescriptor.format = WGPUTextureFormat_BGRA8Unorm;
			textureDescriptor.mipLevelCount = 1;
			eye.dawnTexture = wgpuSharedTextureMemoryCreateTexture(eye.memory, &textureDescriptor);
			if (eye.dawnTexture == nullptr)
				return false;
			eye.view = wgpuTextureCreateView(eye.dawnTexture, nullptr);
			if (eye.view == nullptr)
				return false;
		}
		return true;
	}

	WGPUTextureView BeginEye(int index) override
	{
		Eye &eye = eyes_[index];
		WGPUSharedTextureMemoryBeginAccessDescriptor begin = WGPU_SHARED_TEXTURE_MEMORY_BEGIN_ACCESS_DESCRIPTOR_INIT;
		begin.initialized = false;
		const uint64_t value = copyValue_;
		if (value > 0) { // the bridge's last blit out of this surface comes first
			begin.fenceCount = 1;
			begin.fences = &dawnCopyFence_;
			begin.signaledValues = &value;
		}
		if (wgpuSharedTextureMemoryBeginAccess(eye.memory, eye.dawnTexture, &begin) != WGPUStatus_Success) {
			LogError("VR bridge: BeginAccess failed");
			return nullptr;
		}
		return eye.view;
	}

	bool EndEye(int index, bool copy, uint32_t imageIndex) override
	{
		Eye &eye = eyes_[index];
		WGPUSharedTextureMemoryEndAccessState end = WGPU_SHARED_TEXTURE_MEMORY_END_ACCESS_STATE_INIT;
		if (wgpuSharedTextureMemoryEndAccess(eye.memory, eye.dawnTexture, &end) != WGPUStatus_Success) {
			LogError("VR bridge: EndAccess failed");
			return false;
		}
		id<MTLCommandBuffer> commands = [queue_ commandBuffer];
		for (size_t i = 0; i < end.fenceCount; ++i) {
			WGPUSharedFenceMTLSharedEventExportInfo event = WGPU_SHARED_FENCE_MTL_SHARED_EVENT_EXPORT_INFO_INIT;
			WGPUSharedFenceExportInfo info = WGPU_SHARED_FENCE_EXPORT_INFO_INIT;
			info.nextInChain = &event.chain;
			wgpuSharedFenceExportInfo(end.fences[i], &info);
			if (event.sharedEvent != nullptr)
				[commands encodeWaitForEvent:(__bridge id<MTLSharedEvent>)event.sharedEvent value:end.signaledValues[i]];
		}
		wgpuSharedTextureMemoryEndAccessStateFreeMembers(end);
		if (copy && imageIndex < eye.swapchainImages.size()) {
			id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
			[blit copyFromTexture:eye.copySource toTexture:eye.swapchainImages[imageIndex]];
			[blit endEncoding];
		}
		[commands encodeSignalEvent:copyEvent_ value:++copyValue_];
		[commands commit];
		[commands waitUntilScheduled]; // xrReleaseSwapchainImage follows: the blit must be on the runtime's queue first
		return true;
	}

	void Destroy() override
	{
		for (Eye &eye : eyes_) {
			if (eye.view != nullptr)
				wgpuTextureViewRelease(eye.view);
			if (eye.dawnTexture != nullptr)
				wgpuTextureRelease(eye.dawnTexture);
			if (eye.memory != nullptr)
				wgpuSharedTextureMemoryRelease(eye.memory);
			if (eye.surface != nullptr)
				CFRelease(eye.surface);
			eye = Eye {};
		}
		if (dawnCopyFence_ != nullptr)
			wgpuSharedFenceRelease(dawnCopyFence_);
		dawnCopyFence_ = nullptr;
		copyEvent_ = nil;
		queue_ = nil;
		device_ = nil;
	}

private:
	struct Eye {
		IOSurfaceRef surface = nullptr;
		id<MTLTexture> copySource = nil;
		std::vector<id<MTLTexture>> swapchainImages;
		WGPUSharedTextureMemory memory = nullptr;
		WGPUTexture dawnTexture = nullptr;
		WGPUTextureView view = nullptr;
	};

	id<MTLDevice> device_ = nil;
	id<MTLCommandQueue> queue_ = nil;
	id<MTLSharedEvent> copyEvent_ = nil;
	WGPUSharedFence dawnCopyFence_ = nullptr;
	uint64_t copyValue_ = 0;
	XrGraphicsBindingMetalKHR binding_ { XR_TYPE_GRAPHICS_BINDING_METAL_KHR };
	std::array<Eye, 2> eyes_ {};
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	MTLPixelFormat copyFormat_ = MTLPixelFormatBGRA8Unorm;
};

} // namespace

GraphicsBridge *CreateGraphicsBridge()
{
	return new MetalBridge();
}

} // namespace devilution::xr

#endif // DEVILUTIONX_XR_BRIDGE_METAL
