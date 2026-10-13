/**
 * @file hd_sprites_loader.hpp
 *
 * Loading of the HD sprite art. In the browser (hd_sprites_loader_emscripten.cpp) the PNG grids are fetched from
 * `hd/` next to the page and decoded by the browser (createImageBitmap, off the main thread, premultiplied and
 * optionally downscaled), then copied straight into a WebGPU texture: the decoded pixels never enter the Wasm heap.
 * On the desktop (hd_sprites_loader_native.cpp) `hd/` is the HD art folder and worker threads decode the files.
 */
#pragma once

#include "config.h"

#if DEVILUTIONX_WEBGPU

#include <string>

#include <webgpu/webgpu.h>

namespace devilution::hd_loader {

enum class JobState : int { Pending = 0, Ready = 1, Failed = 2 };

/**
 * @brief Starts fetching and decoding `url`; a non-zero `width` x `height` resizes the decoded image to it.
 *
 * Every fetch revalidates with the server, so an art pack rebuilt in place never pairs a cached grid with a fresh
 * index. With a non-zero `expectedWidth` x `expectedHeight` (the full-size grid the index describes,
 * hd_sprites::ClipImageRequest) a decoded image of another size is fetched once more past the HTTP cache and the job
 * fails if it still differs, instead of drawing frames cut from the wrong cells.
 * @return A job id (> 0).
 */
int RequestImage(const char *url, int width, int height, int expectedWidth = 0, int expectedHeight = 0);
JobState PollImage(int job);
/** Copies a Ready job's image into `texture` (rgba8unorm, CopyDst | RenderAttachment) and releases the job. */
bool UploadImage(int job, WGPUDevice device, WGPUTexture texture, int width, int height);
/** Abandons a job (a late result is discarded). */
void CancelImage(int job);

#ifndef __EMSCRIPTEN__
/** Desktop: a full-size decode that keeps straight alpha (the HD tile atlases keep colour under alpha 0). */
int RequestStraightImage(const char *url);
/**
 * Desktop: writes rows [firstRow, firstRow + rowCount) of a Ready job's image into the same rows of `texture`
 * (`width` x `height`, rgba8unorm, CopyDst); rows outside the image or the texture are skipped. The job stays until
 * CancelImage, so a large image is uploaded a slice per frame. Each call is one queue write: callers keep a slice
 * within Dawn's 4 MiB upload ring (UploadSliceBytes in the HD tile and sprite uploaders).
 */
bool UploadImageRows(int job, WGPUDevice device, WGPUTexture texture, int width, int height, int firstRow, int rowCount);
#endif

/** Fetches a small text file once (revalidated with the server); true when `text` holds it. `failed` is set when the fetch failed. */
bool FetchText(const char *url, std::string &text, bool &failed);

} // namespace devilution::hd_loader

#endif // DEVILUTIONX_WEBGPU
