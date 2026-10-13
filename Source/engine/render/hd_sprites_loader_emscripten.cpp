/**
 * @file hd_sprites_loader_emscripten.cpp
 *
 * Browser implementation of hd_sprites_loader.hpp (fetch + createImageBitmap + copyExternalImageToTexture).
 * See dx_hd_request_image for how decodes are bounded and how uploads are spread over frames.
 */
#include "engine/render/hd_sprites_loader.hpp"

#if DEVILUTIONX_WEBGPU

#include <emscripten.h>

#include <string>
#include <unordered_map>

// EM_JS bodies may contain top-level commas (EM_ASM blocks may not).
//
// The shared loader state (Module.hdImageLoader) is created by the first request:
//  - Every request revalidates with the server (cache: 'no-cache'): the art packs are rebuilt in place, and a grid
//    served from the HTTP cache next to a fresh index (or the other way round) is sliced with the wrong cell size,
//    which shows as frames flipping between poses. The full size is read from the PNG header before decoding; a grid
//    whose size is not the one the index describes (expectedWidth x expectedHeight, 0 = unchecked) is fetched once
//    more past the cache (cache: 'reload') and fails if it still differs.
//  - A downscaled grid is decoded and resized by ONE createImageBitmap(blob, { resizeWidth, resizeHeight }) call, which
//    the browser runs off the main thread. Decoding at full size and then resizing the ImageBitmap in a second call
//    resizes on the main thread (about 9 ms for a 4071x2508 grid in Chrome). The two-step path is only a fallback for
//    an image whose header cannot be read or a browser that ignores the resize options.
//  - At most maxDecodes fetch + decodes run at once (a slot is held from the fetch to the finished bitmap), and a job
//    cancelled while it waits for a slot never fetches or decodes.
//  - Uploads are spread over frames: dx_hd_poll_image reports a decoded image as ready only while this frame's upload
//    budget (uploadBudget bytes, at least one image per frame) lasts; the others stay pending for a later frame. The
//    budget is reset by a zero-delay timeout, i.e. once the task that runs the frame has finished. A clip grid is
//    larger than the budget, so a frame installs at most one of them (several small UI sheets may share a frame):
//    in Chrome two clip uploads in one frame measurably raised the count of frames over 25 ms after a level entry.
//  - The decode options keep texel values exact (no colour space conversion, and premultiplication only changes
//    texels with partial alpha), so the palette-indexed font sheets, which load at full size, stay bit-exact.
EM_JS(int, dx_hd_request_image, (const char *urlPtr, int width, int height, int expectedWidth, int expectedHeight), {
	if (!Module.hdImageLoader) {
		const L = {
			maxDecodes: 3,
			uploadBudget: 1024 * 1024,
			decoding: 0,
			waiting: [],
			uploadUsed: 0,
			uploadResetQueued: false,
			decodeOptions: { premultiplyAlpha: 'premultiply', colorSpaceConversion: 'none' }
		};
		L.acquire = () => new Promise(resolve => {
			if (L.decoding < L.maxDecodes) {
				++L.decoding;
				resolve();
			} else {
				L.waiting.push(resolve);
			}
		});
		L.release = () => {
			const next = L.waiting.shift();
			if (next)
				next();
			else
				--L.decoding;
		};
		/* The size a PNG header promises, or null for anything else. */
		L.headerSize = blob => blob.slice(0, 24).arrayBuffer().then(buffer => {
			if (buffer.byteLength < 24)
				return null;
			const view = new DataView(buffer);
			if (view.getUint32(0) !== 0x89504E47 || view.getUint32(4) !== 0x0D0A1A0A || view.getUint32(12) !== 0x49484452)
				return null;
			return { width: view.getUint32(16), height: view.getUint32(20) };
		});
		L.resize = (bitmap, width, height) => createImageBitmap(bitmap, { resizeWidth: width, resizeHeight: height, resizeQuality: 'high' })
		    .finally(() => bitmap.close());
		L.sizeWarning = (url, w, h, expectedWidth, expectedHeight) => console.warn('HD sprite ' + url + ' is ' + w + 'x' + h
		    + ', the index says ' + expectedWidth + 'x' + expectedHeight + ': fetching it again past the cache');
		/* Fetches and decodes `url`; resolves to an ImageBitmap of width x height (0 = the image's own size), or to null
		   when isCancelled() turned true before the decode. */
		L.load = (url, width, height, expectedWidth, expectedHeight, isCancelled) => {
			const checked = expectedWidth > 0 && expectedHeight > 0;
			const sizeError = (w, h) => new Error('size ' + w + 'x' + h + ' does not match the index (' + expectedWidth + 'x'
			    + expectedHeight + ')');
			const attempt = (cacheMode, retry) => fetch(url, { cache: cacheMode })
			    .then(response => {
				    if (!response.ok)
					    throw new Error('HTTP ' + response.status);
				    return response.blob();
			    })
			    .then(blob => L.headerSize(blob).then(header => {
				    if (isCancelled())
					    return null;
				    if (checked && header && (header.width !== expectedWidth || header.height !== expectedHeight)) {
					    if (!retry)
						    throw sizeError(header.width, header.height);
					    L.sizeWarning(url, header.width, header.height, expectedWidth, expectedHeight);
					    return attempt('reload', false);
				    }
				    const resize = width > 0 && height > 0;
				    if (header && resize && (width !== header.width || height !== header.height)) {
					    const options = Object.assign({ resizeWidth: width, resizeHeight: height, resizeQuality: 'high' }, L.decodeOptions);
					    return createImageBitmap(blob, options).then(bitmap => {
						    if (bitmap.width === width && bitmap.height === height)
							    return bitmap;
						    return L.resize(bitmap, width, height);
					    });
				    }
				    return createImageBitmap(blob, L.decodeOptions).then(bitmap => {
					    if (checked && (bitmap.width !== expectedWidth || bitmap.height !== expectedHeight)) {
						    bitmap.close();
						    if (!retry)
							    throw sizeError(bitmap.width, bitmap.height);
						    L.sizeWarning(url, bitmap.width, bitmap.height, expectedWidth, expectedHeight);
						    return attempt('reload', false);
					    }
					    if (resize && (bitmap.width !== width || bitmap.height !== height))
						    return L.resize(bitmap, width, height);
					    return bitmap;
				    });
			    }));
			return L.acquire().then(() => {
				if (isCancelled()) {
					L.release();
					return null;
				}
				return attempt('no-cache', true).finally(L.release);
			});
		};
		Module.hdImageLoader = L;
	}
	const L = Module.hdImageLoader;
	const state = Module.hdSpriteJobs || (Module.hdSpriteJobs = { next: 1, jobs: new Map() });
	const id = state.next++;
	const url = UTF8ToString(urlPtr);
	const job = { state: 0, bitmap: null, admitted: false, url: url, width: width, height: height };
	state.jobs.set(id, job);
	const isCancelled = () => state.jobs.get(id) !== job;
	L.load(url, width, height, expectedWidth, expectedHeight, isCancelled)
	    .then(bitmap => {
		    if (!bitmap)
			    return;
		    if (isCancelled()) {
			    bitmap.close();
			    return;
		    }
		    job.bitmap = bitmap;
		    job.state = 1;
	    })
	    .catch(error => {
		    if (!isCancelled())
			    console.warn('HD sprite load failed: ' + url + ': ' + error);
		    job.state = 2;
	    });
	return id;
});

// A decoded job only reads as Ready (1) once this frame's upload budget admits it (see dx_hd_request_image).
EM_JS(int, dx_hd_poll_image, (int id), {
	const state = Module.hdSpriteJobs;
	const job = state && state.jobs.get(id);
	if (!job)
		return 2;
	if (job.state !== 1 || job.admitted)
		return job.state;
	const L = Module.hdImageLoader;
	const bytes = job.bitmap.width * job.bitmap.height * 4;
	if (L.uploadUsed > 0 && L.uploadUsed + bytes > L.uploadBudget)
		return 0;
	L.uploadUsed += bytes;
	job.admitted = true;
	if (!L.uploadResetQueued) {
		L.uploadResetQueued = true;
		setTimeout(() => {
			L.uploadUsed = 0;
			L.uploadResetQueued = false;
		}, 0);
	}
	return 1;
});

// Copies a decoded image into the texture. What can be checked up front (a closed bitmap; a texture that is too small,
// lacks CopyDst | RenderAttachment or is not rgba8unorm) fails the upload synchronously, so the caller never marks the
// texture resident. The copy itself runs inside validation and out-of-memory error scopes: an error the GPU reports
// later is logged with the image's URL and the texture handle is queued on Module.hdFailedUploads, so the sprite layer
// drops the clip and draws it classic.
EM_JS(int, dx_hd_upload_image, (int id, void *devicePtr, void *texturePtr, int width, int height), {
	const state = Module.hdSpriteJobs;
	const job = state && state.jobs.get(id);
	if (!job || !job.bitmap)
		return 0;
	state.jobs.delete(id);
	const bitmap = job.bitmap;
	job.bitmap = null;
	const copy = (device, texture, source, label, onError) => {
		const copyWidth = Math.min(width, source.width);
		const copyHeight = Math.min(height, source.height);
		if (copyWidth <= 0 || copyHeight <= 0)
			throw new Error('the decoded image is empty or closed');
		if (texture.width < copyWidth || texture.height < copyHeight || texture.format !== 'rgba8unorm'
		    || (texture.usage & GPUTextureUsage.COPY_DST) === 0 || (texture.usage & GPUTextureUsage.RENDER_ATTACHMENT) === 0)
			throw new Error('the texture (' + texture.width + 'x' + texture.height + ' ' + texture.format + ', usage '
			    + texture.usage + ') cannot take a ' + copyWidth + 'x' + copyHeight + ' copy');
		device.pushErrorScope('out-of-memory');
		device.pushErrorScope('validation');
		device.queue.copyExternalImageToTexture(
		    { source: source },
		    { texture: texture, premultipliedAlpha: true },
		    [ copyWidth, copyHeight ]);
		const report = error => {
			if (error)
				onError(label + ': ' + (error.message || error));
		};
		device.popErrorScope().then(report, () => {});
		device.popErrorScope().then(report, () => {});
	};
	try {
		const device = WebGPU.getJsObject(devicePtr);
		const texture = WebGPU.getJsObject(texturePtr);
		let reported = false;
		copy(device, texture, bitmap, 'HD sprite upload failed (' + job.url + ')', message => {
			console.error(message);
			if (reported)
				return;
			reported = true;
			// The C++ side already marked the clip resident: hand it the texture so it drops the clip and draws it classic
			// (hd_sprites_webgpu.cpp dx_hd_take_failed_upload).
			(Module.hdFailedUploads || (Module.hdFailedUploads = [])).push(texturePtr);
		});
		return 1;
	} catch (error) {
		console.warn('HD sprite upload failed (' + job.url + '): ' + error);
		return 0;
	} finally {
		bitmap.close();
	}
});

EM_JS(void, dx_hd_cancel_image, (int id), {
	const state = Module.hdSpriteJobs;
	const job = state && state.jobs.get(id);
	if (!job)
		return;
	if (job.bitmap)
		job.bitmap.close();
	state.jobs.delete(id);
});

// Text indexes (sprites.txt and friends) revalidate like the images (see dx_hd_request_image).
EM_JS(int, dx_hd_request_text, (const char *urlPtr), {
	const state = Module.hdTextJobs || (Module.hdTextJobs = { next: 1, jobs: new Map() });
	const id = state.next++;
	const job = { state: 0, bytes: null };
	state.jobs.set(id, job);
	const url = UTF8ToString(urlPtr);
	fetch(url, { cache: 'no-cache' })
	    .then(response => {
		    if (!response.ok)
			    throw new Error('HTTP ' + response.status);
		    return response.arrayBuffer();
	    })
	    .then(buffer => {
		    job.bytes = new Uint8Array(buffer);
		    job.state = 1;
	    })
	    .catch(error => {
		    console.warn('HD text load failed: ' + url + ': ' + error);
		    job.state = 2;
	    });
	return id;
});

EM_JS(int, dx_hd_poll_text, (int id), {
	const job = Module.hdTextJobs && Module.hdTextJobs.jobs.get(id);
	return job ? job.state : 2;
});

EM_JS(int, dx_hd_text_size, (int id), {
	const job = Module.hdTextJobs && Module.hdTextJobs.jobs.get(id);
	return job && job.bytes ? job.bytes.length : 0;
});

/** Copies a loaded text into `dest` (dx_hd_text_size bytes; null only releases it) and releases the job. */
EM_JS(void, dx_hd_take_text, (int id, char *dest), {
	const state = Module.hdTextJobs;
	const job = state && state.jobs.get(id);
	if (!job)
		return;
	if (dest && job.bytes)
		HEAPU8.set(job.bytes, dest);
	state.jobs.delete(id);
});

namespace devilution::hd_loader {

int RequestImage(const char *url, int width, int height, int expectedWidth, int expectedHeight)
{
	return dx_hd_request_image(url, width, height, expectedWidth, expectedHeight);
}

JobState PollImage(int job)
{
	return static_cast<JobState>(dx_hd_poll_image(job));
}

bool UploadImage(int job, WGPUDevice device, WGPUTexture texture, int width, int height)
{
	return dx_hd_upload_image(job, device, texture, width, height) != 0;
}

void CancelImage(int job)
{
	dx_hd_cancel_image(job);
}

namespace {

struct TextRequest {
	int job = 0;
	bool done = false;
	bool failed = false;
	std::string text;
};

std::unordered_map<std::string, TextRequest> TextRequests;

} // namespace

bool FetchText(const char *url, std::string &text, bool &failed)
{
	failed = false;
	auto [it, inserted] = TextRequests.try_emplace(url);
	TextRequest &request = it->second;
	if (inserted)
		request.job = dx_hd_request_text(url);
	if (!request.done) {
		const int state = dx_hd_poll_text(request.job);
		if (state == 0)
			return false;
		request.done = true;
		request.failed = state != 1;
		if (!request.failed) {
			request.text.resize(static_cast<size_t>(dx_hd_text_size(request.job)));
			dx_hd_take_text(request.job, request.text.data());
		} else {
			dx_hd_take_text(request.job, nullptr);
		}
	}
	failed = request.failed;
	if (failed)
		return false;
	text = request.text;
	return true;
}

} // namespace devilution::hd_loader

#endif // DEVILUTIONX_WEBGPU
