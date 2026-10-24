/**
 * @file xr/xr_openxr.cpp
 *
 * OpenXR backend (Quest standalone on Android, PC VR runtimes on Windows): instance, system, session, reference
 * spaces, actions for Touch / Index / simple controllers, the frame loop and the projection layer. Graphics go
 * through the platform's GraphicsBridge (xr_openxr_platform.hpp). See docs/design/vr.md.
 */
#include "xr/xr_types.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#endif

#include "utils/log.hpp"
#include "xr/xr_openxr_platform.hpp"

namespace devilution::xr {

namespace {

std::string XrResultName(XrInstance instance, XrResult result)
{
	char buffer[XR_MAX_RESULT_STRING_SIZE] {};
	if (instance != XR_NULL_HANDLE && xrResultToString(instance, result, buffer) == XR_SUCCESS)
		return buffer;
	return std::to_string(static_cast<int>(result));
}

#define XR_CHECK(call)                                                                       \
	do {                                                                                     \
		const XrResult xrCheckResult = (call);                                               \
		if (XR_FAILED(xrCheckResult)) {                                                      \
			LogError("OpenXR: {} failed: {}", #call, XrResultName(Instance, xrCheckResult)); \
			return false;                                                                    \
		}                                                                                    \
	} while (false)

Pose ToPose(const XrPosef &pose, bool valid)
{
	Pose result;
	result.orientation = { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w };
	result.position = { pose.position.x, pose.position.y, pose.position.z };
	result.valid = valid;
	return result;
}

constexpr XrPosef IdentityPose()
{
	XrPosef pose {};
	pose.orientation.w = 1.0F;
	return pose;
}

class OpenXrBackend final : public Backend {
public:
	OpenXrBackend()
	    : bridge_(CreateGraphicsBridge())
	{
	}

	[[nodiscard]] const char *Name() const override
	{
		return "OpenXR";
	}

	bool PreDeviceInit() override
	{
		if (bridge_ == nullptr) {
			LogError("OpenXR: no graphics bridge for this platform");
			return false;
		}
		if (!PlatformInitializeLoader())
			return false;
		return CreateInstance() && FindSystem() && bridge_->CreateDevice(Instance, system_, enabledExtensions_);
	}

	[[nodiscard]] WGPUTextureFormat PreferredColorFormat() const override
	{
		return bridge_ != nullptr ? bridge_->ColorFormat() : WGPUTextureFormat_Undefined;
	}

	void AppendDeviceFeatures(WGPUAdapter adapter, std::vector<WGPUFeatureName> &features) override
	{
		for (const WGPUFeatureName feature : bridge_->DeviceFeatures()) {
			if (wgpuAdapterHasFeature(adapter, feature))
				features.push_back(feature);
			else
				LogError("OpenXR: the WebGPU adapter lacks feature 0x{:x}; VR will not start", static_cast<uint32_t>(feature));
		}
	}

	bool Initialize(WGPUInstance, WGPUAdapter, WGPUDevice device, WGPUQueue, WGPUTextureFormat colorFormat, float renderScale) override
	{
		return CreateSession() && CreateSpaces() && CreateActions() && CreateSwapchains(device, colorFormat, renderScale);
	}

	bool Update() override
	{
		XrEventDataBuffer event { XR_TYPE_EVENT_DATA_BUFFER };
		while (xrPollEvent(Instance, &event) == XR_SUCCESS) {
			switch (event.type) {
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				exitRequested_ = true;
				break;
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const auto &changed = *reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
				OnStateChanged(changed.state);
				break;
			}
			default:
				break;
			}
			event = { XR_TYPE_EVENT_DATA_BUFFER };
		}
		return sessionRunning_;
	}

	[[nodiscard]] bool OwnsDisplay() const override
	{
#ifdef __ANDROID__
		return true;
#else
		return false;
#endif
	}

	bool BeginFrame(FrameInput &input) override
	{
		if (!sessionRunning_)
			return false;
		XrFrameWaitInfo waitInfo { XR_TYPE_FRAME_WAIT_INFO };
		frameState_ = { XR_TYPE_FRAME_STATE };
		if (XR_FAILED(xrWaitFrame(session_, &waitInfo, &frameState_)))
			return false;
		XrFrameBeginInfo beginInfo { XR_TYPE_FRAME_BEGIN_INFO };
		if (XR_FAILED(xrBeginFrame(session_, &beginInfo)))
			return false;
		frameBegun_ = true;
		eyesBegun_ = false;

		const auto now = std::chrono::steady_clock::now();
		input.deltaSeconds = lastFrame_.time_since_epoch().count() == 0 ? 1.0F / 72.0F
		                                                              : std::clamp(std::chrono::duration<float>(now - lastFrame_).count(), 0.0F, 0.1F);
		lastFrame_ = now;
		input.trackingHasNoFloor = baseSpaceIsLocal_;
		input.shouldRender = frameState_.shouldRender == XR_TRUE;

		// Eyes.
		XrViewLocateInfo locateInfo { XR_TYPE_VIEW_LOCATE_INFO };
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = frameState_.predictedDisplayTime;
		locateInfo.space = baseSpace_;
		XrViewState viewState { XR_TYPE_VIEW_STATE };
		uint32_t viewCount = 0;
		views_[0] = views_[1] = { XR_TYPE_VIEW };
		const bool located = XR_SUCCEEDED(xrLocateViews(session_, &locateInfo, &viewState, 2, &viewCount, views_.data())) && viewCount == 2
		    && (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
		if (!located)
			input.shouldRender = false;
		for (int eye = 0; eye < 2; ++eye) {
			input.eyes[eye].pose = ToPose(views_[eye].pose, located);
			input.eyes[eye].fov = { views_[eye].fov.angleLeft, views_[eye].fov.angleRight, views_[eye].fov.angleUp, views_[eye].fov.angleDown };
		}

		// Head.
		XrSpaceLocation head { XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(xrLocateSpace(viewSpace_, baseSpace_, frameState_.predictedDisplayTime, &head)))
			input.head = ToPose(head.pose, (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0);

		ReadControllers(input);
		return true;
	}

	bool AcquireEyeTargets(std::array<WGPUTextureView, 2> &targets, uint32_t &width, uint32_t &height) override
	{
		if (!frameBegun_)
			return false;
		for (int eye = 0; eye < 2; ++eye) {
			targets[eye] = bridge_->BeginEye(eye);
			if (targets[eye] == nullptr) {
				if (eye == 1)
					bridge_->EndEye(0, false, 0);
				return false;
			}
		}
		eyesBegun_ = true;
		width = eyeWidth_;
		height = eyeHeight_;
		return true;
	}

	void EndFrame(bool rendered) override
	{
		if (!frameBegun_)
			return;
		frameBegun_ = false;
		bool submitted = false;
		if (eyesBegun_) {
			eyesBegun_ = false;
			submitted = rendered;
			for (int eye = 0; eye < 2; ++eye) {
				uint32_t imageIndex = 0;
				bool haveImage = false;
				if (rendered) {
					XrSwapchainImageAcquireInfo acquireInfo { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
					XrSwapchainImageWaitInfo waitInfo { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
					waitInfo.timeout = XR_INFINITE_DURATION;
					haveImage = XR_SUCCEEDED(xrAcquireSwapchainImage(swapchains_[eye], &acquireInfo, &imageIndex))
					    && XR_SUCCEEDED(xrWaitSwapchainImage(swapchains_[eye], &waitInfo));
				}
				const bool copied = bridge_->EndEye(eye, haveImage, imageIndex);
				if (haveImage) {
					XrSwapchainImageReleaseInfo releaseInfo { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
					xrReleaseSwapchainImage(swapchains_[eye], &releaseInfo);
				}
				submitted = submitted && haveImage && copied;
			}
		}

		std::array<XrCompositionLayerProjectionView, 2> projectionViews {};
		XrCompositionLayerProjection layer { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		const XrCompositionLayerBaseHeader *layers[] = { reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer) };
		XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = frameState_.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if (submitted && frameState_.shouldRender) {
			for (int eye = 0; eye < 2; ++eye) {
				projectionViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
				projectionViews[eye].pose = views_[eye].pose;
				projectionViews[eye].fov = views_[eye].fov;
				projectionViews[eye].subImage.swapchain = swapchains_[eye];
				projectionViews[eye].subImage.imageRect.offset = { 0, 0 };
				projectionViews[eye].subImage.imageRect.extent = { static_cast<int32_t>(eyeWidth_), static_cast<int32_t>(eyeHeight_) };
				projectionViews[eye].subImage.imageArrayIndex = 0;
			}
			layer.space = baseSpace_;
			layer.viewCount = 2;
			layer.views = projectionViews.data();
			endInfo.layerCount = 1;
			endInfo.layers = layers;
		}
		const XrResult result = xrEndFrame(session_, &endInfo);
		if (XR_FAILED(result))
			LogError("OpenXR: xrEndFrame failed: {}", XrResultName(Instance, result));
	}

	[[nodiscard]] bool ExitRequested() const override
	{
		return exitRequested_;
	}

	void Shutdown() override
	{
		if (bridge_ != nullptr)
			bridge_->Destroy();
		for (XrSwapchain &swapchain : swapchains_) {
			if (swapchain != XR_NULL_HANDLE)
				xrDestroySwapchain(swapchain);
			swapchain = XR_NULL_HANDLE;
		}
		for (XrSpace *space : { &baseSpace_, &viewSpace_, &aimSpaces_[0], &aimSpaces_[1] }) {
			if (*space != XR_NULL_HANDLE)
				xrDestroySpace(*space);
			*space = XR_NULL_HANDLE;
		}
		if (actionSet_ != XR_NULL_HANDLE)
			xrDestroyActionSet(actionSet_);
		actionSet_ = XR_NULL_HANDLE;
		if (session_ != XR_NULL_HANDLE)
			xrDestroySession(session_);
		session_ = XR_NULL_HANDLE;
		if (Instance != XR_NULL_HANDLE)
			xrDestroyInstance(Instance);
		Instance = XR_NULL_HANDLE;
		sessionRunning_ = false;
	}

private:
	bool HasExtension(const std::vector<XrExtensionProperties> &available, const char *name)
	{
		return std::any_of(available.begin(), available.end(), [&](const XrExtensionProperties &p) { return std::strcmp(p.extensionName, name) == 0; });
	}

	bool CreateInstance()
	{
		uint32_t count = 0;
		XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
		std::vector<XrExtensionProperties> available(count, { XR_TYPE_EXTENSION_PROPERTIES });
		XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()));

		enabledExtensions_.clear();
		if (!HasExtension(available, bridge_->RequiredExtension())) {
			LogError("OpenXR: the runtime lacks {}", bridge_->RequiredExtension());
			return false;
		}
		enabledExtensions_.push_back(bridge_->RequiredExtension());
		for (const char *optional : bridge_->OptionalExtensions()) {
			if (HasExtension(available, optional))
				enabledExtensions_.push_back(optional);
		}
#ifdef __ANDROID__
		if (HasExtension(available, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
			enabledExtensions_.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
		hasLocalFloor_ = HasExtension(available, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
		if (hasLocalFloor_)
			enabledExtensions_.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);

		XrInstanceCreateInfo createInfo { XR_TYPE_INSTANCE_CREATE_INFO };
		createInfo.next = PlatformInstanceCreateNext();
		std::strncpy(createInfo.applicationInfo.applicationName, "DamnationX", XR_MAX_APPLICATION_NAME_SIZE - 1);
		createInfo.applicationInfo.applicationVersion = 1;
		std::strncpy(createInfo.applicationInfo.engineName, "DevilutionX", XR_MAX_ENGINE_NAME_SIZE - 1);
		createInfo.applicationInfo.engineVersion = 1;
		createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0; // 1.0 + extensions: the widest runtime support
		createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions_.size());
		createInfo.enabledExtensionNames = enabledExtensions_.data();
		XR_CHECK(xrCreateInstance(&createInfo, &Instance));

		XrInstanceProperties properties { XR_TYPE_INSTANCE_PROPERTIES };
		if (XR_SUCCEEDED(xrGetInstanceProperties(Instance, &properties))) {
			Log("OpenXR: runtime {} {}.{}.{}", properties.runtimeName, XR_VERSION_MAJOR(properties.runtimeVersion),
			    XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
		}
		return true;
	}

	bool FindSystem()
	{
		XrSystemGetInfo getInfo { XR_TYPE_SYSTEM_GET_INFO };
		getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		const XrResult result = xrGetSystem(Instance, &getInfo, &system_);
		if (XR_FAILED(result)) {
			LogError("OpenXR: no headset ({}). Is the headset connected (Quest Link / Air Link / Virtual Desktop) and the runtime active?",
			    XrResultName(Instance, result));
			return false;
		}
		XrSystemProperties properties { XR_TYPE_SYSTEM_PROPERTIES };
		if (XR_SUCCEEDED(xrGetSystemProperties(Instance, system_, &properties)))
			Log("OpenXR: headset {}", properties.systemName);

		uint32_t count = 0;
		XR_CHECK(xrEnumerateViewConfigurationViews(Instance, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr));
		std::vector<XrViewConfigurationView> configViews(count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
		XR_CHECK(xrEnumerateViewConfigurationViews(Instance, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, configViews.data()));
		if (count < 2) {
			LogError("OpenXR: the headset has no stereo view configuration");
			return false;
		}
		recommended_ = configViews[0];
		return true;
	}

	bool CreateSession()
	{
		XrSessionCreateInfo createInfo { XR_TYPE_SESSION_CREATE_INFO };
		createInfo.next = bridge_->SessionBinding();
		createInfo.systemId = system_;
		XR_CHECK(xrCreateSession(Instance, &createInfo, &session_));
		return true;
	}

	bool CreateReferenceSpace(XrReferenceSpaceType type, XrSpace &space)
	{
		XrReferenceSpaceCreateInfo createInfo { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		createInfo.referenceSpaceType = type;
		createInfo.poseInReferenceSpace = IdentityPose();
		return XR_SUCCEEDED(xrCreateReferenceSpace(session_, &createInfo, &space));
	}

	bool CreateSpaces()
	{
		// Floor level first: the eye height then comes from the player's real height.
		baseSpaceIsLocal_ = false;
		if (!(hasLocalFloor_ && CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT, baseSpace_))
		    && !CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, baseSpace_)) {
			baseSpaceIsLocal_ = true;
			if (!CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, baseSpace_)) {
				LogError("OpenXR: no reference space");
				return false;
			}
		}
		if (!CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, viewSpace_)) {
			LogError("OpenXR: no view space");
			return false;
		}
		return true;
	}

	XrPath Path(const char *text)
	{
		XrPath path = XR_NULL_PATH;
		xrStringToPath(Instance, text, &path);
		return path;
	}

	bool CreateAction(const char *name, const char *localized, XrActionType type, XrAction &action)
	{
		XrActionCreateInfo createInfo { XR_TYPE_ACTION_CREATE_INFO };
		std::strncpy(createInfo.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
		std::strncpy(createInfo.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
		createInfo.actionType = type;
		createInfo.countSubactionPaths = 2;
		createInfo.subactionPaths = handPaths_.data();
		XR_CHECK(xrCreateAction(actionSet_, &createInfo, &action));
		return true;
	}

	void SuggestBindings(const char *profile, const std::vector<std::pair<XrAction, const char *>> &bindings)
	{
		std::vector<XrActionSuggestedBinding> suggested;
		for (const auto &[action, path] : bindings)
			suggested.push_back({ action, Path(path) });
		XrInteractionProfileSuggestedBinding suggestion { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggestion.interactionProfile = Path(profile);
		suggestion.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
		suggestion.suggestedBindings = suggested.data();
		const XrResult result = xrSuggestInteractionProfileBindings(Instance, &suggestion);
		if (XR_FAILED(result))
			LogVerbose("OpenXR: bindings for {} rejected: {}", profile, XrResultName(Instance, result));
	}

	bool CreateActions()
	{
		handPaths_ = { Path("/user/hand/left"), Path("/user/hand/right") };
		XrActionSetCreateInfo setInfo { XR_TYPE_ACTION_SET_CREATE_INFO };
		std::strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
		std::strncpy(setInfo.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
		XR_CHECK(xrCreateActionSet(Instance, &setInfo, &actionSet_));
		if (!CreateAction("aim_pose", "Aim", XR_ACTION_TYPE_POSE_INPUT, aimAction_)
		    || !CreateAction("stick", "Move and turn", XR_ACTION_TYPE_VECTOR2F_INPUT, stickAction_)
		    || !CreateAction("trigger", "Attack / click", XR_ACTION_TYPE_FLOAT_INPUT, triggerAction_)
		    || !CreateAction("grip", "Spell / right click", XR_ACTION_TYPE_FLOAT_INPUT, gripAction_)
		    || !CreateAction("primary", "Interact / character", XR_ACTION_TYPE_BOOLEAN_INPUT, primaryAction_)
		    || !CreateAction("secondary", "Inventory / spellbook", XR_ACTION_TYPE_BOOLEAN_INPUT, secondaryAction_)
		    || !CreateAction("stick_click", "Recentre / potion", XR_ACTION_TYPE_BOOLEAN_INPUT, stickClickAction_)
		    || !CreateAction("menu", "Game menu", XR_ACTION_TYPE_BOOLEAN_INPUT, menuAction_))
			return false;

		SuggestBindings("/interaction_profiles/oculus/touch_controller", {
		                                                                    { aimAction_, "/user/hand/left/input/aim/pose" },
		                                                                    { aimAction_, "/user/hand/right/input/aim/pose" },
		                                                                    { stickAction_, "/user/hand/left/input/thumbstick" },
		                                                                    { stickAction_, "/user/hand/right/input/thumbstick" },
		                                                                    { triggerAction_, "/user/hand/left/input/trigger/value" },
		                                                                    { triggerAction_, "/user/hand/right/input/trigger/value" },
		                                                                    { gripAction_, "/user/hand/left/input/squeeze/value" },
		                                                                    { gripAction_, "/user/hand/right/input/squeeze/value" },
		                                                                    { primaryAction_, "/user/hand/left/input/x/click" },
		                                                                    { primaryAction_, "/user/hand/right/input/a/click" },
		                                                                    { secondaryAction_, "/user/hand/left/input/y/click" },
		                                                                    { secondaryAction_, "/user/hand/right/input/b/click" },
		                                                                    { stickClickAction_, "/user/hand/left/input/thumbstick/click" },
		                                                                    { stickClickAction_, "/user/hand/right/input/thumbstick/click" },
		                                                                    { menuAction_, "/user/hand/left/input/menu/click" },
		                                                                });
		SuggestBindings("/interaction_profiles/valve/index_controller", {
		                                                                   { aimAction_, "/user/hand/left/input/aim/pose" },
		                                                                   { aimAction_, "/user/hand/right/input/aim/pose" },
		                                                                   { stickAction_, "/user/hand/left/input/thumbstick" },
		                                                                   { stickAction_, "/user/hand/right/input/thumbstick" },
		                                                                   { triggerAction_, "/user/hand/left/input/trigger/value" },
		                                                                   { triggerAction_, "/user/hand/right/input/trigger/value" },
		                                                                   { gripAction_, "/user/hand/left/input/squeeze/value" },
		                                                                   { gripAction_, "/user/hand/right/input/squeeze/value" },
		                                                                   { primaryAction_, "/user/hand/left/input/a/click" },
		                                                                   { primaryAction_, "/user/hand/right/input/a/click" },
		                                                                   { secondaryAction_, "/user/hand/left/input/b/click" },
		                                                                   { secondaryAction_, "/user/hand/right/input/b/click" },
		                                                                   { stickClickAction_, "/user/hand/left/input/thumbstick/click" },
		                                                                   { stickClickAction_, "/user/hand/right/input/thumbstick/click" },
		                                                               });
		SuggestBindings("/interaction_profiles/khr/simple_controller", {
		                                                                  { aimAction_, "/user/hand/left/input/aim/pose" },
		                                                                  { aimAction_, "/user/hand/right/input/aim/pose" },
		                                                                  { triggerAction_, "/user/hand/left/input/select/click" },
		                                                                  { triggerAction_, "/user/hand/right/input/select/click" },
		                                                                  { menuAction_, "/user/hand/left/input/menu/click" },
		                                                                  { menuAction_, "/user/hand/right/input/menu/click" },
		                                                              });

		for (int hand = 0; hand < 2; ++hand) {
			XrActionSpaceCreateInfo spaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
			spaceInfo.action = aimAction_;
			spaceInfo.subactionPath = handPaths_[hand];
			spaceInfo.poseInActionSpace = IdentityPose();
			XR_CHECK(xrCreateActionSpace(session_, &spaceInfo, &aimSpaces_[hand]));
		}
		XrSessionActionSetsAttachInfo attachInfo { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		attachInfo.countActionSets = 1;
		attachInfo.actionSets = &actionSet_;
		XR_CHECK(xrAttachSessionActionSets(session_, &attachInfo));
		return true;
	}

	bool CreateSwapchains(WGPUDevice device, WGPUTextureFormat colorFormat, float renderScale)
	{
		uint32_t count = 0;
		XR_CHECK(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr));
		std::vector<int64_t> formats(count);
		XR_CHECK(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()));
		int64_t chosen = 0;
		for (const int64_t preferred : bridge_->PreferredSwapchainFormats()) {
			if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) {
				chosen = preferred;
				break;
			}
		}
		if (chosen == 0) {
			LogError("OpenXR: none of the bridge's swapchain formats is supported");
			return false;
		}
		const float scale = std::clamp(renderScale, 0.3F, 1.0F);
		eyeWidth_ = std::max<uint32_t>(64, static_cast<uint32_t>(static_cast<float>(recommended_.recommendedImageRectWidth) * scale));
		eyeHeight_ = std::max<uint32_t>(64, static_cast<uint32_t>(static_cast<float>(recommended_.recommendedImageRectHeight) * scale));
		eyeWidth_ = std::min(eyeWidth_, recommended_.maxImageRectWidth);
		eyeHeight_ = std::min(eyeHeight_, recommended_.maxImageRectHeight);
		for (int eye = 0; eye < 2; ++eye) {
			XrSwapchainCreateInfo createInfo { XR_TYPE_SWAPCHAIN_CREATE_INFO };
			createInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			createInfo.format = chosen;
			createInfo.sampleCount = 1;
			createInfo.width = eyeWidth_;
			createInfo.height = eyeHeight_;
			createInfo.faceCount = 1;
			createInfo.arraySize = 1;
			createInfo.mipCount = 1;
			XR_CHECK(xrCreateSwapchain(session_, &createInfo, &swapchains_[eye]));
		}
		Log("OpenXR: eyes {}x{} (recommended {}x{}), swapchain format {}", eyeWidth_, eyeHeight_, recommended_.recommendedImageRectWidth,
		    recommended_.recommendedImageRectHeight, chosen);
		return bridge_->CreateEyes(swapchains_, chosen, eyeWidth_, eyeHeight_, device, colorFormat);
	}

	void OnStateChanged(XrSessionState state)
	{
		LogVerbose("OpenXR: session state {}", static_cast<int>(state));
		switch (state) {
		case XR_SESSION_STATE_READY: {
			XrSessionBeginInfo beginInfo { XR_TYPE_SESSION_BEGIN_INFO };
			beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			const XrResult result = xrBeginSession(session_, &beginInfo);
			sessionRunning_ = XR_SUCCEEDED(result);
			if (!sessionRunning_)
				LogError("OpenXR: xrBeginSession failed: {}", XrResultName(Instance, result));
			break;
		}
		case XR_SESSION_STATE_STOPPING:
			xrEndSession(session_);
			sessionRunning_ = false;
			break;
		case XR_SESSION_STATE_FOCUSED:
			focused_ = true;
			break;
		case XR_SESSION_STATE_VISIBLE:
			focused_ = false;
			break;
		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING:
			sessionRunning_ = false;
			exitRequested_ = true;
			break;
		default:
			break;
		}
	}

	void ReadControllers(FrameInput &input)
	{
		if (!focused_) {
			input.hands = {};
			return;
		}
		XrActiveActionSet active { actionSet_, XR_NULL_PATH };
		XrActionsSyncInfo syncInfo { XR_TYPE_ACTIONS_SYNC_INFO };
		syncInfo.countActiveActionSets = 1;
		syncInfo.activeActionSets = &active;
		if (XR_FAILED(xrSyncActions(session_, &syncInfo)))
			return;
		for (int hand = 0; hand < 2; ++hand) {
			HandInput &out = input.hands[hand];
			XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
			getInfo.subactionPath = handPaths_[hand];

			getInfo.action = aimAction_;
			XrActionStatePose poseState { XR_TYPE_ACTION_STATE_POSE };
			xrGetActionStatePose(session_, &getInfo, &poseState);
			out.active = poseState.isActive == XR_TRUE;
			if (out.active) {
				XrSpaceLocation location { XR_TYPE_SPACE_LOCATION };
				if (XR_SUCCEEDED(xrLocateSpace(aimSpaces_[hand], baseSpace_, frameState_.predictedDisplayTime, &location))) {
					constexpr XrSpaceLocationFlags Valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
					out.aim = ToPose(location.pose, (location.locationFlags & Valid) == Valid);
				}
			}

			getInfo.action = stickAction_;
			XrActionStateVector2f stick { XR_TYPE_ACTION_STATE_VECTOR2F };
			if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &getInfo, &stick)) && stick.isActive) {
				out.stickX = stick.currentState.x;
				out.stickY = stick.currentState.y;
			}
			out.trigger = ReadFloat(triggerAction_, getInfo);
			out.grip = ReadFloat(gripAction_, getInfo);
			out.primary = ReadBool(primaryAction_, getInfo);
			out.secondary = ReadBool(secondaryAction_, getInfo);
			out.stickClick = ReadBool(stickClickAction_, getInfo);
			out.menu = ReadBool(menuAction_, getInfo);
		}
	}

	float ReadFloat(XrAction action, XrActionStateGetInfo getInfo)
	{
		getInfo.action = action;
		XrActionStateFloat state { XR_TYPE_ACTION_STATE_FLOAT };
		if (XR_SUCCEEDED(xrGetActionStateFloat(session_, &getInfo, &state)) && state.isActive)
			return state.currentState;
		return 0.0F;
	}

	bool ReadBool(XrAction action, XrActionStateGetInfo getInfo)
	{
		getInfo.action = action;
		XrActionStateBoolean state { XR_TYPE_ACTION_STATE_BOOLEAN };
		return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &state)) && state.isActive && state.currentState;
	}

	XrInstance Instance = XR_NULL_HANDLE; // capitalised for XR_CHECK
	std::unique_ptr<GraphicsBridge> bridge_;
	std::vector<const char *> enabledExtensions_;
	bool hasLocalFloor_ = false;
	XrSystemId system_ = XR_NULL_SYSTEM_ID;
	XrViewConfigurationView recommended_ { XR_TYPE_VIEW_CONFIGURATION_VIEW };
	XrSession session_ = XR_NULL_HANDLE;
	XrSpace baseSpace_ = XR_NULL_HANDLE;
	XrSpace viewSpace_ = XR_NULL_HANDLE;
	bool baseSpaceIsLocal_ = false;
	std::array<XrPath, 2> handPaths_ {};
	XrActionSet actionSet_ = XR_NULL_HANDLE;
	XrAction aimAction_ = XR_NULL_HANDLE;
	XrAction stickAction_ = XR_NULL_HANDLE;
	XrAction triggerAction_ = XR_NULL_HANDLE;
	XrAction gripAction_ = XR_NULL_HANDLE;
	XrAction primaryAction_ = XR_NULL_HANDLE;
	XrAction secondaryAction_ = XR_NULL_HANDLE;
	XrAction stickClickAction_ = XR_NULL_HANDLE;
	XrAction menuAction_ = XR_NULL_HANDLE;
	std::array<XrSpace, 2> aimSpaces_ { XR_NULL_HANDLE, XR_NULL_HANDLE };
	std::array<XrSwapchain, 2> swapchains_ { XR_NULL_HANDLE, XR_NULL_HANDLE };
	uint32_t eyeWidth_ = 0;
	uint32_t eyeHeight_ = 0;
	bool sessionRunning_ = false;
	bool focused_ = false;
	bool exitRequested_ = false;
	bool frameBegun_ = false;
	bool eyesBegun_ = false;
	XrFrameState frameState_ { XR_TYPE_FRAME_STATE };
	std::array<XrView, 2> views_ { XrView { XR_TYPE_VIEW }, XrView { XR_TYPE_VIEW } };
	std::chrono::steady_clock::time_point lastFrame_ {};
};

#ifdef __ANDROID__
JavaVM *AndroidVm = nullptr;
jobject AndroidActivity = nullptr;
XrInstanceCreateInfoAndroidKHR AndroidCreateInfo { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
#endif

} // namespace

bool PlatformInitializeLoader()
{
#ifdef __ANDROID__
	auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
	auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
	if (env == nullptr || activity == nullptr || env->GetJavaVM(&AndroidVm) != JNI_OK) {
		LogError("OpenXR: no Android activity");
		return false;
	}
	if (AndroidActivity == nullptr)
		AndroidActivity = env->NewGlobalRef(activity);
	env->DeleteLocalRef(activity);
	PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction *>(&initializeLoader)))
	    || initializeLoader == nullptr) {
		LogError("OpenXR: xrInitializeLoaderKHR missing");
		return false;
	}
	XrLoaderInitInfoAndroidKHR loaderInfo { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInfo.applicationVM = AndroidVm;
	loaderInfo.applicationContext = AndroidActivity;
	if (XR_FAILED(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loaderInfo)))) {
		LogError("OpenXR: loader initialisation failed (no OpenXR runtime installed?)");
		return false;
	}
	AndroidCreateInfo.applicationVM = AndroidVm;
	AndroidCreateInfo.applicationActivity = AndroidActivity;
#endif
	return true;
}

const void *PlatformInstanceCreateNext()
{
#ifdef __ANDROID__
	return &AndroidCreateInfo;
#else
	return nullptr;
#endif
}

#ifndef __EMSCRIPTEN__
Backend *CreateBackend()
{
	return new OpenXrBackend();
}
#endif

} // namespace devilution::xr
