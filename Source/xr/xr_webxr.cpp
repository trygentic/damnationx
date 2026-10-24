/**
 * @file xr/xr_webxr.cpp
 *
 * WebXR backend for the browser build (Quest Browser, desktop Chrome with a PC VR runtime). See docs/design/vr.md,
 * "WebXR".
 *
 * WebXR has no stable WebGPU binding yet (XRGPUBinding is behind flags in Chrome and Quest Browser), so the eyes go
 * through WebGL: the game renders both eyes side by side into a hidden WebGPU canvas (#dvl-xr-canvas); each XR
 * animation frame uploads that canvas into a WebGL texture (texImage2D accepts a WebGPU canvas) and draws one half
 * per XR view into the XRWebGLLayer. The page stays in charge of the session: an "Enter VR" button (a user gesture is
 * required) starts it. Poses reach the game through a small JS state object; the XR animation frame also wakes the
 * game's frame yield (window.requestAnimationFrame stops while immersive).
 *
 * Latency: the image shown in XR frame N was rendered with the poses of frame N-1 (one frame behind, no reprojection
 * of our own beyond what the browser does). Good enough to play; the native Quest app is the low-latency path.
 */
#include "xr/xr_types.hpp"

#ifdef __EMSCRIPTEN__

#include <algorithm>
#include <array>

#include <emscripten.h>

#include "utils/log.hpp"

namespace devilution::xr {

namespace {

// Layout of the float block dvl_xr_read_state fills (kept in sync with the JS below).
// [0] shouldRender, [1] dt, [2] no floor, [3..9] head, [10..31] eyes (pose + 4 fov angles), [32..63] hands
// (active, pose, pose valid, stick x/y, trigger, grip, primary, secondary, stick click), [64..65] eye size.
constexpr int kStateFloats = 72;

// clang-format off
EM_ASYNC_JS(int, dvl_xr_supported, (), {
	try {
		if (!navigator.xr) return 0;
		return (await navigator.xr.isSessionSupported('immersive-vr')) ? 1 : 0;
	} catch (e) {
		return 0;
	}
});

EM_JS(void, dvl_xr_setup, (float renderScale), {
	if (Module.dvlXr) return;
	const xr = Module.dvlXr = {
		running: false, exitRequested: false, session: null, refSpace: null, noFloor: false, gl: null, layer: null,
		texture: null, program: null, gpuCanvas: null, eyeWidth: 0, eyeHeight: 0, lastTime: 0, dt: 1 / 72, state: new Float32Array(72),
		renderScale: renderScale
	};
	const gpuCanvas = document.createElement('canvas');
	gpuCanvas.id = 'dvl-xr-canvas';
	gpuCanvas.width = 2; gpuCanvas.height = 1;
	gpuCanvas.style.cssText = 'position:fixed;left:-10000px;top:0;width:1px;height:1px;pointer-events:none';
	document.body.appendChild(gpuCanvas);
	xr.gpuCanvas = gpuCanvas;

	const button = document.createElement('button');
	button.id = 'dvl-enter-vr';
	button.textContent = 'Enter VR';
	button.style.cssText = 'position:fixed;right:16px;bottom:16px;z-index:1000;padding:12px 20px;font:600 16px sans-serif;'
		+ 'background:#7a1010;color:#fff;border:1px solid #c33;border-radius:6px;cursor:pointer';
	document.body.appendChild(button);

	const start = async () => {
		if (xr.session) { xr.session.end(); return; }
		let session;
		try {
			session = await navigator.xr.requestSession('immersive-vr', { optionalFeatures: ['local-floor'] });
		} catch (e) {
			console.error('DamnationX VR: requestSession failed', e);
			return;
		}
		const glCanvas = document.createElement('canvas');
		const gl = glCanvas.getContext('webgl2', { xrCompatible: true, antialias: false, alpha: false });
		await gl.makeXRCompatible();
		const layer = new XRWebGLLayer(session, gl, { antialias: false, depth: false, framebufferScaleFactor: xr.renderScale });
		session.updateRenderState({ baseLayer: layer, depthNear: 0.05, depthFar: 500 });
		try {
			xr.refSpace = await session.requestReferenceSpace('local-floor');
			xr.noFloor = false;
		} catch (e) {
			xr.refSpace = await session.requestReferenceSpace('local');
			xr.noFloor = true;
		}
		const vs = `#version 300 es
			in vec2 p; out vec2 uv; uniform vec4 rect;
			void main() { uv = vec2(rect.x + (p.x * 0.5 + 0.5) * rect.z, 1.0 - (p.y * 0.5 + 0.5)); gl_Position = vec4(p, 0.0, 1.0); }`;
		const fs = `#version 300 es
			precision mediump float; in vec2 uv; out vec4 color; uniform sampler2D image;
			void main() { color = vec4(texture(image, uv).rgb, 1.0); }`;
		const compile = (type, source) => { const s = gl.createShader(type); gl.shaderSource(s, source); gl.compileShader(s); return s; };
		const program = gl.createProgram();
		gl.attachShader(program, compile(gl.VERTEX_SHADER, vs));
		gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fs));
		gl.bindAttribLocation(program, 0, 'p');
		gl.linkProgram(program);
		const buffer = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
		gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
		xr.texture = gl.createTexture();
		gl.bindTexture(gl.TEXTURE_2D, xr.texture);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
		xr.gl = gl; xr.layer = layer; xr.program = program; xr.buffer = buffer; xr.session = session;
		xr.eyeWidth = Math.floor(layer.framebufferWidth / 2);
		xr.eyeHeight = layer.framebufferHeight;
		gpuCanvas.width = xr.eyeWidth * 2;
		gpuCanvas.height = xr.eyeHeight;
		xr.running = true;
		button.textContent = 'Exit VR';
		session.addEventListener('end', () => {
			xr.running = false; xr.session = null; button.textContent = 'Enter VR';
		});
		session.requestAnimationFrame(xr.onFrame);
	};
	button.addEventListener('click', start);

	const writePose = (out, offset, transform) => {
		const p = transform.position, o = transform.orientation;
		out[offset] = o.x; out[offset + 1] = o.y; out[offset + 2] = o.z; out[offset + 3] = o.w;
		out[offset + 4] = p.x; out[offset + 5] = p.y; out[offset + 6] = p.z;
	};
	xr.onFrame = (time, frame) => {
		const session = frame.session;
		session.requestAnimationFrame(xr.onFrame);
		xr.dt = xr.lastTime > 0 ? Math.min(0.1, (time - xr.lastTime) / 1000) : 1 / 72;
		xr.lastTime = time;
		const s = xr.state;
		s.fill(0);
		const pose = frame.getViewerPose(xr.refSpace);
		if (pose) {
			s[0] = 1; // shouldRender
			s[1] = xr.dt;
			s[2] = xr.noFloor ? 1 : 0;
			writePose(s, 3, pose.transform);
			for (let i = 0; i < Math.min(2, pose.views.length); i++) {
				const view = pose.views[i];
				const eye = view.eye === 'right' ? 1 : 0;
				const base = 10 + eye * 11;
				writePose(s, base, view.transform);
				const m = view.projectionMatrix; // column-major; recover the tangents of the frustum
				s[base + 7] = Math.atan((m[8] - 1) / m[0]);
				s[base + 8] = Math.atan((m[8] + 1) / m[0]);
				s[base + 9] = Math.atan((m[9] + 1) / m[5]);
				s[base + 10] = Math.atan((m[9] - 1) / m[5]);
			}
		}
		for (const source of session.inputSources) {
			const hand = source.handedness === 'left' ? 0 : source.handedness === 'right' ? 1 : -1;
			if (hand < 0) continue;
			const base = 32 + hand * 16;
			const aim = frame.getPose(source.targetRaySpace, xr.refSpace);
			s[base] = 1;
			if (aim) { writePose(s, base + 1, aim.transform); s[base + 8] = 1; }
			const pad = source.gamepad;
			if (pad) {
				const button = (i) => (pad.buttons.length > i ? pad.buttons[i] : null);
				s[base + 9] = pad.axes.length > 3 ? pad.axes[2] : 0;
				s[base + 10] = pad.axes.length > 3 ? -pad.axes[3] : 0;
				s[base + 11] = button(0) ? button(0).value : 0;
				s[base + 12] = button(1) ? button(1).value : 0;
				s[base + 13] = button(4) && button(4).pressed ? 1 : 0;
				s[base + 14] = button(5) && button(5).pressed ? 1 : 0;
				s[base + 15] = button(3) && button(3).pressed ? 1 : 0;
			}
		}
		s[64] = xr.eyeWidth;
		s[65] = xr.eyeHeight;

		// Show the last stereo image the game rendered.
		const gl = xr.gl, layer = session.renderState.baseLayer;
		gl.bindFramebuffer(gl.FRAMEBUFFER, layer.framebuffer);
		gl.clearColor(0, 0, 0, 1);
		gl.clear(gl.COLOR_BUFFER_BIT);
		if (pose && xr.gpuCanvas.width > 2) {
			gl.bindTexture(gl.TEXTURE_2D, xr.texture);
			try {
				gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, xr.gpuCanvas);
			} catch (e) {
			}
			gl.useProgram(xr.program);
			gl.bindBuffer(gl.ARRAY_BUFFER, xr.buffer);
			gl.enableVertexAttribArray(0);
			gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
			const rect = gl.getUniformLocation(xr.program, 'rect');
			for (const view of pose.views) {
				const viewport = layer.getViewport(view);
				gl.viewport(viewport.x, viewport.y, viewport.width, viewport.height);
				gl.uniform4f(rect, view.eye === 'right' ? 0.5 : 0.0, 0, 0.5, 1);
				gl.drawArrays(gl.TRIANGLES, 0, 3);
			}
		}
		// The page gets no window animation frames while immersive: wake the game's frame yield (engine/dx.cpp).
		const y = Module.dxFrameYield;
		if (y && y.onRaf) y.onRaf(time);
	};
});
// clang-format on

EM_JS(int, dvl_xr_read_state, (float *out, int count), {
	const xr = Module.dvlXr;
	if (!xr || !xr.running) return 0;
	HEAPF32.set(xr.state.subarray(0, count), out >> 2);
	return 1;
});

Pose ReadPose(const float *s, bool valid)
{
	Pose pose;
	pose.orientation = { s[0], s[1], s[2], s[3] };
	pose.position = { s[4], s[5], s[6] };
	pose.valid = valid;
	return pose;
}

class WebXrBackend final : public Backend {
public:
	[[nodiscard]] const char *Name() const override
	{
		return "WebXR";
	}

	bool PreDeviceInit() override
	{
		if (dvl_xr_supported() == 0) {
			LogError("WebXR: immersive-vr is not supported by this browser / device");
			return false;
		}
		return true;
	}

	[[nodiscard]] WGPUTextureFormat PreferredColorFormat() const override
	{
		return WGPUTextureFormat_Undefined; // any: the canvas is read back by WebGL
	}

	bool Initialize(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device, WGPUQueue queue, WGPUTextureFormat colorFormat, float renderScale) override
	{
		dvl_xr_setup(renderScale);
		instance_ = instance;
		adapter_ = adapter;
		device_ = device;
		queue_ = queue;
		colorFormat_ = colorFormat;
		WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
		canvas.selector = { "#dvl-xr-canvas", WGPU_STRLEN };
		WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
		descriptor.nextInChain = &canvas.chain;
		surface_ = wgpuInstanceCreateSurface(instance, &descriptor);
		return surface_ != nullptr;
	}

	bool Update() override
	{
		running_ = dvl_xr_read_state(state_.data(), kStateFloats) != 0;
		return running_;
	}

	bool BeginFrame(FrameInput &input) override
	{
		if (!running_)
			return false;
		const float *s = state_.data();
		input.shouldRender = s[0] > 0.5F;
		input.deltaSeconds = s[1];
		input.trackingHasNoFloor = s[2] > 0.5F;
		input.head = ReadPose(s + 3, input.shouldRender);
		for (int eye = 0; eye < 2; ++eye) {
			const float *e = s + 10 + eye * 11;
			input.eyes[eye].pose = ReadPose(e, input.shouldRender);
			input.eyes[eye].fov = { e[7], e[8], e[9], e[10] };
		}
		for (int hand = 0; hand < 2; ++hand) {
			const float *h = s + 32 + hand * 16;
			HandInput &out = input.hands[hand];
			out.active = h[0] > 0.5F;
			out.aim = ReadPose(h + 1, h[8] > 0.5F);
			out.stickX = h[9];
			out.stickY = h[10];
			out.trigger = h[11];
			out.grip = h[12];
			out.primary = h[13] > 0.5F;
			out.secondary = h[14] > 0.5F;
			out.stickClick = h[15] > 0.5F;
		}
		eyeWidth_ = static_cast<uint32_t>(std::max(0.0F, s[64]));
		eyeHeight_ = static_cast<uint32_t>(std::max(0.0F, s[65]));
		return true;
	}

	bool AcquireEyeTargets(std::array<WGPUTextureView, 2> &targets, uint32_t &width, uint32_t &height) override
	{
		if (eyeWidth_ == 0 || eyeHeight_ == 0 || !EnsureEyes())
			return false;
		targets = { eyeViews_[0], eyeViews_[1] };
		width = eyeWidth_;
		height = eyeHeight_;
		return true;
	}

	void EndFrame(bool rendered) override
	{
		if (!rendered || surface_ == nullptr)
			return;
		WGPUSurfaceTexture surfaceTexture = WGPU_SURFACE_TEXTURE_INIT;
		wgpuSurfaceGetCurrentTexture(surface_, &surfaceTexture);
		if (surfaceTexture.texture == nullptr)
			return;
		WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
		for (uint32_t eye = 0; eye < 2; ++eye) {
			WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			source.texture = eyeTextures_[eye];
			WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			destination.texture = surfaceTexture.texture;
			destination.origin = { eye * eyeWidth_, 0, 0 };
			const WGPUExtent3D size { eyeWidth_, eyeHeight_, 1 };
			wgpuCommandEncoderCopyTextureToTexture(encoder, &source, &destination, &size);
		}
		WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
		wgpuCommandEncoderRelease(encoder);
		wgpuQueueSubmit(queue_, 1, &commands);
		wgpuCommandBufferRelease(commands);
		wgpuTextureRelease(surfaceTexture.texture);
		// Presented when the game yields to the browser; the next XR animation frame uploads it.
	}

	void Shutdown() override
	{
		ReleaseEyes();
		if (surface_ != nullptr)
			wgpuSurfaceRelease(surface_);
		surface_ = nullptr;
	}

private:
	bool EnsureEyes()
	{
		if (eyeTextures_[0] != nullptr && configuredWidth_ == eyeWidth_ && configuredHeight_ == eyeHeight_)
			return true;
		ReleaseEyes();
		WGPUSurfaceConfiguration configuration = WGPU_SURFACE_CONFIGURATION_INIT;
		configuration.device = device_;
		configuration.format = colorFormat_;
		configuration.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
		configuration.width = eyeWidth_ * 2;
		configuration.height = eyeHeight_;
		configuration.alphaMode = WGPUCompositeAlphaMode_Opaque;
		configuration.presentMode = WGPUPresentMode_Fifo;
		wgpuSurfaceConfigure(surface_, &configuration);
		for (int eye = 0; eye < 2; ++eye) {
			WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
			descriptor.label = { "DamnationX WebXR eye", WGPU_STRLEN };
			descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
			descriptor.dimension = WGPUTextureDimension_2D;
			descriptor.size = { eyeWidth_, eyeHeight_, 1 };
			descriptor.format = colorFormat_;
			descriptor.mipLevelCount = 1;
			eyeTextures_[eye] = wgpuDeviceCreateTexture(device_, &descriptor);
			if (eyeTextures_[eye] == nullptr)
				return false;
			eyeViews_[eye] = wgpuTextureCreateView(eyeTextures_[eye], nullptr);
		}
		configuredWidth_ = eyeWidth_;
		configuredHeight_ = eyeHeight_;
		Log("WebXR: eyes {}x{}", eyeWidth_, eyeHeight_);
		return eyeViews_[0] != nullptr && eyeViews_[1] != nullptr;
	}

	void ReleaseEyes()
	{
		for (int eye = 0; eye < 2; ++eye) {
			if (eyeViews_[eye] != nullptr)
				wgpuTextureViewRelease(eyeViews_[eye]);
			if (eyeTextures_[eye] != nullptr) {
				wgpuTextureDestroy(eyeTextures_[eye]);
				wgpuTextureRelease(eyeTextures_[eye]);
			}
			eyeViews_[eye] = nullptr;
			eyeTextures_[eye] = nullptr;
		}
		configuredWidth_ = configuredHeight_ = 0;
	}

	WGPUInstance instance_ = nullptr;
	WGPUAdapter adapter_ = nullptr;
	WGPUDevice device_ = nullptr;
	WGPUQueue queue_ = nullptr;
	WGPUTextureFormat colorFormat_ = WGPUTextureFormat_Undefined;
	WGPUSurface surface_ = nullptr;
	std::array<WGPUTexture, 2> eyeTextures_ {};
	std::array<WGPUTextureView, 2> eyeViews_ {};
	uint32_t eyeWidth_ = 0;
	uint32_t eyeHeight_ = 0;
	uint32_t configuredWidth_ = 0;
	uint32_t configuredHeight_ = 0;
	bool running_ = false;
	std::array<float, kStateFloats> state_ {};
};

} // namespace

Backend *CreateBackend()
{
	return new WebXrBackend();
}

} // namespace devilution::xr

#endif // __EMSCRIPTEN__
