/**
 * @file hd_webgpu_async.hpp
 *
 * Background creation of the HD layers' render pipelines (hd_tiles_webgpu.cpp, hd_sprites_webgpu.cpp).
 *
 * A cold WGSL compile of the HD tile and sprite pipelines takes 250-350 ms on the desktop (Dawn/Metal) and blocked the
 * first town and first dungeon frame. The pipelines are started with wgpuDeviceCreateRenderPipelineAsync on the first
 * presented frame (title screen) instead, so they compile on the driver's threads while the menus run. A pipeline that
 * is needed before its compile finished is still created synchronously, exactly as before, so what is drawn never
 * depends on how fast the compile was.
 */
#pragma once

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <mutex>
#include <string>
#include <utility>

#include <webgpu/webgpu.h>

namespace devilution::hd_gpu {

/** One wgpuDeviceCreateRenderPipelineAsync in flight. Main thread only, except the completion callback. */
class AsyncPipeline {
public:
	AsyncPipeline() = default;
	AsyncPipeline(const AsyncPipeline &) = delete;
	AsyncPipeline &operator=(const AsyncPipeline &) = delete;
	~AsyncPipeline() { abandon(); }

	[[nodiscard]] bool pending() const { return shared_ != nullptr; }

	/** Starts creating the pipeline described by `descriptor` (its shader modules may be released afterwards). */
	void start(WGPUDevice device, const WGPURenderPipelineDescriptor &descriptor)
	{
		abandon();
		shared_ = new Shared();
		WGPUCreateRenderPipelineAsyncCallbackInfo callback = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
		callback.mode = WGPUCallbackMode_AllowSpontaneous;
		callback.callback = &OnCreated;
		callback.userdata1 = shared_;
		wgpuDeviceCreateRenderPipelineAsync(device, &descriptor, callback);
	}

	/**
	 * True once the creation has finished: `pipeline` then holds the result (owned by the caller; nullptr when it
	 * failed) and the request is gone. False while it is still compiling (or nothing was started).
	 */
	bool take(WGPURenderPipeline &pipeline)
	{
		if (shared_ == nullptr)
			return false;
		{
			std::lock_guard lock(shared_->mutex);
			if (!shared_->done)
				return false;
			pipeline = shared_->pipeline;
		}
		delete shared_;
		shared_ = nullptr;
		return true;
	}

	/** Drops the request; a result arriving later is released by the callback. */
	void abandon()
	{
		if (shared_ == nullptr)
			return;
		Shared *shared = shared_;
		shared_ = nullptr;
		std::unique_lock lock(shared->mutex);
		if (!shared->done) {
			shared->abandoned = true; // the callback frees it
			return;
		}
		WGPURenderPipeline pipeline = shared->pipeline;
		lock.unlock();
		if (pipeline != nullptr)
			wgpuRenderPipelineRelease(pipeline);
		delete shared;
	}

private:
	struct Shared {
		std::mutex mutex;
		bool done = false;
		bool abandoned = false;
		WGPURenderPipeline pipeline = nullptr;
	};

	static void OnCreated(WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline, WGPUStringView /*message*/, void *userdata1, void * /*userdata2*/)
	{
		auto *shared = static_cast<Shared *>(userdata1);
		if (status != WGPUCreatePipelineAsyncStatus_Success && pipeline != nullptr) {
			wgpuRenderPipelineRelease(pipeline);
			pipeline = nullptr;
		}
		std::unique_lock lock(shared->mutex);
		if (shared->abandoned) {
			lock.unlock();
			if (pipeline != nullptr)
				wgpuRenderPipelineRelease(pipeline);
			delete shared;
			return;
		}
		shared->pipeline = pipeline;
		shared->done = true;
	}

	Shared *shared_ = nullptr;
};

/**
 * The HD layers' instanced quad pipeline: `vertexMain` / `fragmentMain` of one WGSL module, a triangle list, premultiplied
 * alpha over one `format` target, auto layout. Holds the shader module and the descriptor state the descriptor points
 * to (so it is neither copied nor moved).
 */
struct QuadPipelineDescriptor {
	QuadPipelineDescriptor(WGPUDevice device, WGPUTextureFormat format, const char *source, const char *label)
	{
		WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl.code = { source, WGPU_STRLEN };
		WGPUShaderModuleDescriptor moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		moduleDescriptor.nextInChain = &wgsl.chain;
		moduleDescriptor.label = { label, WGPU_STRLEN };
		shader = wgpuDeviceCreateShaderModule(device, &moduleDescriptor);

		blend.color = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha };
		blend.alpha = { WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha };
		colorTarget.format = format;
		colorTarget.blend = &blend;
		fragment.module = shader;
		fragment.entryPoint = { "fragmentMain", WGPU_STRLEN };
		fragment.targetCount = 1;
		fragment.targets = &colorTarget;
		descriptor.label = { label, WGPU_STRLEN };
		descriptor.vertex.module = shader;
		descriptor.vertex.entryPoint = { "vertexMain", WGPU_STRLEN };
		descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		descriptor.fragment = &fragment;
	}
	~QuadPipelineDescriptor()
	{
		if (shader != nullptr)
			wgpuShaderModuleRelease(shader);
	}
	QuadPipelineDescriptor(const QuadPipelineDescriptor &) = delete;
	QuadPipelineDescriptor &operator=(const QuadPipelineDescriptor &) = delete;

	[[nodiscard]] bool valid() const { return shader != nullptr; }

	WGPUShaderModule shader = nullptr;
	WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
	WGPUColorTargetState colorTarget = WGPU_COLOR_TARGET_STATE_INIT;
	WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
	WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
};

/**
 * One HD quad pipeline: compiled in the background from the first presented frame (`start`), handed out by `get` when a
 * frame draws with it. `get` blocks on a synchronous creation only when the background compile has not finished yet.
 */
class QuadPipeline {
public:
	QuadPipeline(std::string source, const char *label)
	    : source_(std::move(source))
	    , label_(label)
	{
	}
	~QuadPipeline() { release(); }
	QuadPipeline(const QuadPipeline &) = delete;
	QuadPipeline &operator=(const QuadPipeline &) = delete;

	/** Starts the background compile for `device` / `format` once (again after `release`). */
	void start(WGPUDevice device, WGPUTextureFormat format)
	{
		if (started_)
			return;
		started_ = true;
		const QuadPipelineDescriptor description(device, format, source_.c_str(), label_);
		if (description.valid())
			async_.start(device, description.descriptor);
	}

	/**
	 * The pipeline (nullptr when creation failed). The first call takes the background result, or creates it now
	 * (a failed background compile is retried synchronously, as the original code path did).
	 */
	WGPURenderPipeline get(WGPUDevice device, WGPUTextureFormat format)
	{
		if (resolved_)
			return pipeline_;
		started_ = true;
		resolved_ = true;
		WGPURenderPipeline pipeline = nullptr;
		if (async_.take(pipeline) && pipeline != nullptr) {
			pipeline_ = pipeline;
			return pipeline_;
		}
		async_.abandon();
		const QuadPipelineDescriptor description(device, format, source_.c_str(), label_);
		if (description.valid())
			pipeline_ = wgpuDeviceCreateRenderPipeline(device, &description.descriptor);
		return pipeline_;
	}

	/** True once `get` handed out a pipeline (it stays until `release`). */
	[[nodiscard]] WGPURenderPipeline current() const { return pipeline_; }

	/** Drops the pipeline and any background compile (device teardown, surface format change). */
	void release()
	{
		async_.abandon();
		if (pipeline_ != nullptr)
			wgpuRenderPipelineRelease(pipeline_);
		pipeline_ = nullptr;
		started_ = false;
		resolved_ = false;
	}

private:
	std::string source_;
	const char *label_;
	AsyncPipeline async_;
	WGPURenderPipeline pipeline_ = nullptr;
	bool started_ = false;
	bool resolved_ = false;
};

} // namespace devilution::hd_gpu

#endif // DEVILUTIONX_WEBGPU
