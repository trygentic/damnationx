/**
 * @file xr/xr_bridge_d3d11.cpp
 *
 * PC VR graphics bridge (docs/design/vr.md, "Graphics bridge"): a D3D11 device on the headset's adapter owns the
 * OpenXR session (XR_KHR_D3D11_enable, supported by the Meta/Oculus, SteamVR, Virtual Desktop and WMR runtimes).
 * Each eye is a D3D11 texture shared through a DXGI NT handle and imported into Dawn (D3D12 or D3D11 backend,
 * SharedTextureMemoryDXGISharedHandle); a shared D3D11 fence and Dawn's exported fences order the two devices.
 */
#include "xr/xr_openxr_platform.hpp"

#ifdef DEVILUTIONX_XR_BRIDGE_D3D11

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <unordered_map>

#include "utils/log.hpp"

using Microsoft::WRL::ComPtr;

namespace devilution::xr {

namespace {

class D3D11Bridge final : public GraphicsBridge {
public:
	[[nodiscard]] const char *Name() const override
	{
		return "D3D11";
	}

	[[nodiscard]] const char *RequiredExtension() const override
	{
		return XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
	}

	bool CreateDevice(XrInstance instance, XrSystemId system, const std::vector<const char *> &) override
	{
		PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
		if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements)))
		    || getRequirements == nullptr)
			return false;
		XrGraphicsRequirementsD3D11KHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
		if (XR_FAILED(getRequirements(instance, system, &requirements)))
			return false;
		adapterLuid_ = requirements.adapterLuid;

		ComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
			return false;
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 description {};
			adapter->GetDesc1(&description);
			if (description.AdapterLuid.LowPart == adapterLuid_.LowPart && description.AdapterLuid.HighPart == adapterLuid_.HighPart)
				break;
			adapter.Reset();
		}
		if (adapter == nullptr) {
			LogError("VR bridge: the headset's GPU adapter was not found");
			return false;
		}
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
		        D3D11_SDK_VERSION, &device, nullptr, &context))
		    || FAILED(device.As(&device_)) || FAILED(context.As(&context_))) {
			LogError("VR bridge: D3D11.4 device creation failed (Windows 10 1703 or newer is needed)");
			return false;
		}
		if (FAILED(device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&copyFence_)))
		    || FAILED(copyFence_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &copyFenceHandle_)))
			return false;
		binding_.device = device_.Get();
		Log("VR bridge: D3D11 device ready");
		return true;
	}

	[[nodiscard]] const void *SessionBinding() const override
	{
		return &binding_;
	}

	[[nodiscard]] std::vector<WGPUFeatureName> DeviceFeatures() const override
	{
		return { WGPUFeatureName_SharedTextureMemoryDXGISharedHandle, WGPUFeatureName_SharedFenceDXGISharedHandle };
	}

	[[nodiscard]] std::vector<int64_t> PreferredSwapchainFormats() const override
	{
		// The same typeless family as the RGBA8 eye texture, so CopyResource works.
		return { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
	}

	bool CreateEyes(const std::array<XrSwapchain, 2> &swapchains, int64_t, uint32_t width, uint32_t height, WGPUDevice device,
	    WGPUTextureFormat colorFormat) override
	{
		if (colorFormat != WGPUTextureFormat_RGBA8Unorm) {
			LogError("VR bridge: the presenter format {} is not RGBA8Unorm", static_cast<int>(colorFormat));
			return false;
		}
		WGPUSharedFenceDXGISharedHandleDescriptor fenceHandle = WGPU_SHARED_FENCE_DXGI_SHARED_HANDLE_DESCRIPTOR_INIT;
		fenceHandle.handle = copyFenceHandle_;
		WGPUSharedFenceDescriptor fenceDescriptor = WGPU_SHARED_FENCE_DESCRIPTOR_INIT;
		fenceDescriptor.nextInChain = &fenceHandle.chain;
		dawnCopyFence_ = wgpuDeviceImportSharedFence(device, &fenceDescriptor);
		if (dawnCopyFence_ == nullptr)
			return false;

		for (int index = 0; index < 2; ++index) {
			Eye &eye = eyes_[index];
			uint32_t count = 0;
			if (XR_FAILED(xrEnumerateSwapchainImages(swapchains[index], 0, &count, nullptr)))
				return false;
			std::vector<XrSwapchainImageD3D11KHR> images(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
			if (XR_FAILED(xrEnumerateSwapchainImages(swapchains[index], count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()))))
				return false;
			for (const XrSwapchainImageD3D11KHR &image : images)
				eye.swapchainImages.push_back(image.texture);

			D3D11_TEXTURE2D_DESC description {};
			description.Width = width;
			description.Height = height;
			description.MipLevels = 1;
			description.ArraySize = 1;
			description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			description.SampleDesc.Count = 1;
			description.Usage = D3D11_USAGE_DEFAULT;
			description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			description.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
			ComPtr<IDXGIResource1> resource;
			if (FAILED(device_->CreateTexture2D(&description, nullptr, &eye.texture)) || FAILED(eye.texture.As(&resource))
			    || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &eye.sharedHandle)))
				return false;

			WGPUSharedTextureMemoryDXGISharedHandleDescriptor handle = WGPU_SHARED_TEXTURE_MEMORY_DXGI_SHARED_HANDLE_DESCRIPTOR_INIT;
			handle.handle = eye.sharedHandle;
			handle.useKeyedMutex = false;
			WGPUSharedTextureMemoryDescriptor memoryDescriptor = WGPU_SHARED_TEXTURE_MEMORY_DESCRIPTOR_INIT;
			memoryDescriptor.nextInChain = &handle.chain;
			memoryDescriptor.label = { "DamnationX VR eye", WGPU_STRLEN };
			eye.memory = wgpuDeviceImportSharedTextureMemory(device, &memoryDescriptor);
			if (eye.memory == nullptr)
				return false;
			WGPUTextureDescriptor textureDescriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
			textureDescriptor.usage = WGPUTextureUsage_RenderAttachment;
			textureDescriptor.dimension = WGPUTextureDimension_2D;
			textureDescriptor.size = { width, height, 1 };
			textureDescriptor.format = WGPUTextureFormat_RGBA8Unorm;
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
		if (value > 0) { // the previous copy out of this texture must be done before Dawn overwrites it
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
		for (size_t i = 0; i < end.fenceCount; ++i) {
			WGPUSharedFenceDXGISharedHandleExportInfo handle = WGPU_SHARED_FENCE_DXGI_SHARED_HANDLE_EXPORT_INFO_INIT;
			WGPUSharedFenceExportInfo info = WGPU_SHARED_FENCE_EXPORT_INFO_INIT;
			info.nextInChain = &handle.chain;
			wgpuSharedFenceExportInfo(end.fences[i], &info);
			ID3D11Fence *fence = OpenDawnFence(handle.handle);
			if (fence != nullptr)
				context_->Wait(fence, end.signaledValues[i]);
		}
		wgpuSharedTextureMemoryEndAccessStateFreeMembers(end);
		if (copy && imageIndex < eye.swapchainImages.size())
			context_->CopyResource(eye.swapchainImages[imageIndex], eye.texture.Get());
		context_->Signal(copyFence_.Get(), ++copyValue_);
		context_->Flush();
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
			if (eye.sharedHandle != nullptr)
				CloseHandle(eye.sharedHandle);
			eye = Eye {};
		}
		if (dawnCopyFence_ != nullptr)
			wgpuSharedFenceRelease(dawnCopyFence_);
		dawnCopyFence_ = nullptr;
		dawnFences_.clear();
		if (copyFenceHandle_ != nullptr)
			CloseHandle(copyFenceHandle_);
		copyFenceHandle_ = nullptr;
		copyFence_.Reset();
		context_.Reset();
		device_.Reset();
	}

private:
	struct Eye {
		ComPtr<ID3D11Texture2D> texture;
		HANDLE sharedHandle = nullptr;
		WGPUSharedTextureMemory memory = nullptr;
		WGPUTexture dawnTexture = nullptr;
		WGPUTextureView view = nullptr;
		std::vector<ID3D11Texture2D *> swapchainImages;
	};

	ID3D11Fence *OpenDawnFence(void *handle)
	{
		if (handle == nullptr)
			return nullptr;
		auto it = dawnFences_.find(handle);
		if (it != dawnFences_.end())
			return it->second.Get();
		ComPtr<ID3D11Fence> fence;
		if (FAILED(device_->OpenSharedFence(static_cast<HANDLE>(handle), IID_PPV_ARGS(&fence))))
			return nullptr;
		return dawnFences_.emplace(handle, fence).first->second.Get();
	}

	LUID adapterLuid_ {};
	ComPtr<ID3D11Device5> device_;
	ComPtr<ID3D11DeviceContext4> context_;
	ComPtr<ID3D11Fence> copyFence_;
	HANDLE copyFenceHandle_ = nullptr;
	WGPUSharedFence dawnCopyFence_ = nullptr;
	uint64_t copyValue_ = 0;
	std::unordered_map<void *, ComPtr<ID3D11Fence>> dawnFences_;
	XrGraphicsBindingD3D11KHR binding_ { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	std::array<Eye, 2> eyes_ {};
};

} // namespace

GraphicsBridge *CreateGraphicsBridge()
{
	return new D3D11Bridge();
}

} // namespace devilution::xr

#endif // DEVILUTIONX_XR_BRIDGE_D3D11
