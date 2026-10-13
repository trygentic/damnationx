/**
 * @file hd_sprites_loader_native.cpp
 *
 * Desktop (Dawn) implementation of hd_sprites_loader.hpp. `hd/<file>` names a file in the HD art folder
 * (GetHdAssetDirectory, `--hd-dir`). Images are read and decoded on worker threads (libpng / libwebp), premultiplied
 * and downscaled like the browser's createImageBitmap path, then written to the texture with wgpuQueueWriteTexture.
 */
#include "engine/render/hd_sprites_loader.hpp"

#if DEVILUTIONX_WEBGPU

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <png.h>
#include <webp/decode.h>

#include "engine/assets.hpp"
#include "utils/file_util.h"
#include "utils/log.hpp"

namespace devilution::hd_loader {
namespace {

struct ImageJob {
	int id = 0;
	std::string path;
	int width = 0;
	int height = 0;
	int expectedWidth = 0;
	int expectedHeight = 0;
	bool premultiply = true;
	// Written by the worker before `state` leaves Pending (guarded by Loader::mutex).
	JobState state = JobState::Pending;
	int imageWidth = 0;
	int imageHeight = 0;
	std::vector<uint8_t> pixels; // RGBA8, imageWidth * imageHeight
};

/** `hd/<file>` -> the HD folder; other names are used as given. */
std::string ResolvePath(const char *url)
{
	const std::string_view name { url };
	const std::string &directory = GetHdAssetDirectory();
	const bool inHdFolder = !directory.empty() && (name.starts_with("hd/") || name.starts_with("hd\\"));
	std::string path { inHdFolder ? name.substr(3) : name };
#ifndef _WIN32
	// Manifests may name files with backslashes (Windows-made packs); POSIX paths need '/'.
	std::replace(path.begin(), path.end(), '\\', '/');
#endif
	if (inHdFolder)
		path.insert(0, directory);
	return path;
}

bool ReadFile(const std::string &path, std::vector<uint8_t> &bytes)
{
	// OpenFile, not std::fopen: on Windows it converts the UTF-8 path, so a pack under a non-ASCII user folder opens.
	std::unique_ptr<FILE, decltype(&std::fclose)> file { OpenFile(path.c_str(), "rb"), &std::fclose };
	if (file == nullptr)
		return false;
	if (std::fseek(file.get(), 0, SEEK_END) != 0)
		return false;
	const long size = std::ftell(file.get());
	if (size < 0 || std::fseek(file.get(), 0, SEEK_SET) != 0)
		return false;
	bytes.resize(static_cast<size_t>(size));
	return size == 0 || std::fread(bytes.data(), 1, bytes.size(), file.get()) == bytes.size();
}

bool DecodePng(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &pixels, int &width, int &height)
{
	png_image image {};
	image.version = PNG_IMAGE_VERSION;
	if (png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()) == 0)
		return false;
	image.format = PNG_FORMAT_RGBA;
	pixels.resize(PNG_IMAGE_SIZE(image));
	if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) == 0) {
		png_image_free(&image);
		return false;
	}
	width = static_cast<int>(image.width);
	height = static_cast<int>(image.height);
	return true;
}

bool DecodeWebP(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &pixels, int &width, int &height)
{
	if (WebPGetInfo(bytes.data(), bytes.size(), &width, &height) == 0)
		return false;
	pixels.resize(static_cast<size_t>(width) * height * 4);
	return WebPDecodeRGBAInto(bytes.data(), bytes.size(), pixels.data(), pixels.size(), width * 4) != nullptr;
}

bool Decode(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &pixels, int &width, int &height)
{
	if (bytes.size() >= 12 && std::equal(bytes.begin(), bytes.begin() + 4, "RIFF") && std::equal(bytes.begin() + 8, bytes.begin() + 12, "WEBP"))
		return DecodeWebP(bytes, pixels, width, height);
	return DecodePng(bytes, pixels, width, height);
}

void Premultiply(std::vector<uint8_t> &pixels)
{
	for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
		const unsigned alpha = pixels[i + 3];
		if (alpha == 255)
			continue;
		for (size_t c = 0; c < 3; ++c)
			pixels[i + c] = static_cast<uint8_t>((pixels[i + c] * alpha + 127) / 255);
	}
}

/** Box-filtered resize (area average for downscales, the browser's resizeQuality 'high' stand-in). */
std::vector<uint8_t> Resize(const std::vector<uint8_t> &source, int sourceWidth, int sourceHeight, int width, int height)
{
	std::vector<uint8_t> result(static_cast<size_t>(width) * height * 4);
	for (int y = 0; y < height; ++y) {
		const int y0 = static_cast<int>(static_cast<int64_t>(y) * sourceHeight / height);
		const int y1 = std::max(y0 + 1, static_cast<int>((static_cast<int64_t>(y) + 1) * sourceHeight / height));
		for (int x = 0; x < width; ++x) {
			const int x0 = static_cast<int>(static_cast<int64_t>(x) * sourceWidth / width);
			const int x1 = std::max(x0 + 1, static_cast<int>((static_cast<int64_t>(x) + 1) * sourceWidth / width));
			uint32_t sum[4] = {};
			for (int sy = y0; sy < y1; ++sy) {
				const uint8_t *row = source.data() + (static_cast<size_t>(sy) * sourceWidth + x0) * 4;
				for (int sx = x0; sx < x1; ++sx, row += 4) {
					for (int c = 0; c < 4; ++c)
						sum[c] += row[c];
				}
			}
			const uint32_t count = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
			uint8_t *out = result.data() + (static_cast<size_t>(y) * width + x) * 4;
			for (int c = 0; c < 4; ++c)
				out[c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
		}
	}
	return result;
}

/** Fills a job (worker thread); returns the final state. */
JobState Load(ImageJob &job)
{
	std::vector<uint8_t> bytes;
	if (!ReadFile(job.path, bytes)) {
		LogError("HD image load failed: {}: cannot read the file", job.path);
		return JobState::Failed;
	}
	int width = 0;
	int height = 0;
	std::vector<uint8_t> pixels;
	if (!Decode(bytes, pixels, width, height)) {
		LogError("HD image load failed: {}: cannot decode", job.path);
		return JobState::Failed;
	}
	if (job.expectedWidth > 0 && job.expectedHeight > 0 && (width != job.expectedWidth || height != job.expectedHeight)) {
		LogError("HD image {} is {}x{}, the index says {}x{}", job.path, width, height, job.expectedWidth, job.expectedHeight);
		return JobState::Failed;
	}
	if (job.premultiply)
		Premultiply(pixels);
	if (job.width > 0 && job.height > 0 && (job.width != width || job.height != height)) {
		pixels = Resize(pixels, width, height, job.width, job.height);
		width = job.width;
		height = job.height;
	}
	job.pixels = std::move(pixels);
	job.imageWidth = width;
	job.imageHeight = height;
	return JobState::Ready;
}

class Loader {
public:
	~Loader()
	{
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		wake.notify_all();
		for (std::thread &worker : workers)
			worker.join();
	}

	int Request(std::shared_ptr<ImageJob> job)
	{
		std::lock_guard lock(mutex);
		if (workers.empty()) {
			const unsigned count = std::clamp(std::thread::hardware_concurrency() / 2, 1U, 4U);
			for (unsigned i = 0; i < count; ++i)
				workers.emplace_back([this]() { Work(); });
		}
		job->id = nextId++;
		jobs.emplace(job->id, job);
		queue.push_back(job);
		wake.notify_one();
		return job->id;
	}

	JobState Poll(int id)
	{
		std::lock_guard lock(mutex);
		const auto it = jobs.find(id);
		return it != jobs.end() ? it->second->state : JobState::Failed;
	}

	/** Removes a job; a Pending one is dropped by its worker. */
	std::shared_ptr<ImageJob> Take(int id)
	{
		std::lock_guard lock(mutex);
		const auto it = jobs.find(id);
		if (it == jobs.end())
			return nullptr;
		std::shared_ptr<ImageJob> job = std::move(it->second);
		jobs.erase(it);
		return job;
	}

	/** A Ready job, kept in the table (the main thread is the only one that removes jobs). */
	std::shared_ptr<ImageJob> Find(int id)
	{
		std::lock_guard lock(mutex);
		const auto it = jobs.find(id);
		return it != jobs.end() && it->second->state == JobState::Ready ? it->second : nullptr;
	}

	/** Hands a removed job to a worker to free: a decoded page is up to 64 MiB, too much to unmap on the main thread. */
	void Discard(std::shared_ptr<ImageJob> job)
	{
		if (job == nullptr || job->pixels.empty())
			return;
		{
			std::lock_guard lock(mutex);
			if (!workers.empty()) {
				garbage.push_back(std::move(job));
				job = nullptr;
			}
		}
		wake.notify_one();
	}

private:
	void Work()
	{
		while (true) {
			std::shared_ptr<ImageJob> job;
			ImageJob result;
			{
				std::unique_lock lock(mutex);
				wake.wait(lock, [this]() { return stopping || !queue.empty() || !garbage.empty(); });
				if (stopping)
					return;
				if (!garbage.empty()) {
					std::shared_ptr<ImageJob> old = std::move(garbage.back());
					garbage.pop_back();
					lock.unlock();
					old.reset();
					continue;
				}
				job = std::move(queue.front());
				queue.pop_front();
				if (!jobs.contains(job->id))
					continue; // cancelled before it started
				result = *job;
			}
			const JobState state = Load(result);
			std::lock_guard lock(mutex);
			if (!jobs.contains(job->id))
				continue; // cancelled while decoding
			job->pixels = std::move(result.pixels);
			job->imageWidth = result.imageWidth;
			job->imageHeight = result.imageHeight;
			job->state = state;
		}
	}

	std::mutex mutex;
	std::condition_variable wake;
	std::vector<std::thread> workers;
	std::deque<std::shared_ptr<ImageJob>> queue;
	std::vector<std::shared_ptr<ImageJob>> garbage; // finished jobs a worker frees
	std::unordered_map<int, std::shared_ptr<ImageJob>> jobs;
	int nextId = 1;
	bool stopping = false;
};

Loader &GetLoader()
{
	static Loader Instance;
	return Instance;
}

int Request(const char *url, int width, int height, int expectedWidth, int expectedHeight, bool premultiply)
{
	auto job = std::make_shared<ImageJob>();
	job->path = ResolvePath(url);
	job->width = width;
	job->height = height;
	job->expectedWidth = expectedWidth;
	job->expectedHeight = expectedHeight;
	job->premultiply = premultiply;
	return GetLoader().Request(std::move(job));
}

} // namespace

int RequestImage(const char *url, int width, int height, int expectedWidth, int expectedHeight)
{
	return Request(url, width, height, expectedWidth, expectedHeight, /*premultiply=*/true);
}

int RequestStraightImage(const char *url)
{
	return Request(url, 0, 0, 0, 0, /*premultiply=*/false);
}

JobState PollImage(int job)
{
	return GetLoader().Poll(job);
}

bool UploadImage(int job, WGPUDevice device, WGPUTexture texture, int width, int height)
{
	const std::shared_ptr<ImageJob> image = GetLoader().Take(job);
	if (image == nullptr || image->state != JobState::Ready || device == nullptr || texture == nullptr)
		return false;
	const int copyWidth = std::min(width, image->imageWidth);
	const int copyHeight = std::min(height, image->imageHeight);
	if (copyWidth <= 0 || copyHeight <= 0)
		return false;
	WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	destination.texture = texture;
	WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	layout.bytesPerRow = static_cast<uint32_t>(image->imageWidth) * 4;
	layout.rowsPerImage = static_cast<uint32_t>(image->imageHeight);
	const WGPUExtent3D size { static_cast<uint32_t>(copyWidth), static_cast<uint32_t>(copyHeight), 1 };
	WGPUQueue queue = wgpuDeviceGetQueue(device);
	wgpuQueueWriteTexture(queue, &destination, image->pixels.data(), image->pixels.size(), &layout, &size);
	wgpuQueueRelease(queue);
	return true;
}

bool UploadImageRows(int job, WGPUDevice device, WGPUTexture texture, int width, int height, int firstRow, int rowCount)
{
	const std::shared_ptr<ImageJob> image = GetLoader().Find(job);
	if (image == nullptr || device == nullptr || texture == nullptr || firstRow < 0)
		return false;
	const int copyWidth = std::min(width, image->imageWidth);
	const int endRow = std::min({ firstRow + rowCount, height, image->imageHeight });
	if (copyWidth <= 0)
		return false;
	if (endRow <= firstRow)
		return true;
	WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	destination.texture = texture;
	destination.origin = { 0, static_cast<uint32_t>(firstRow), 0 };
	WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	layout.bytesPerRow = static_cast<uint32_t>(image->imageWidth) * 4;
	layout.rowsPerImage = static_cast<uint32_t>(endRow - firstRow);
	const WGPUExtent3D size { static_cast<uint32_t>(copyWidth), static_cast<uint32_t>(endRow - firstRow), 1 };
	const size_t offset = static_cast<size_t>(firstRow) * layout.bytesPerRow;
	// Only the slice's rows are handed over: Dawn stages exactly the data range it is given.
	const size_t dataSize = static_cast<size_t>(endRow - firstRow - 1) * layout.bytesPerRow + static_cast<size_t>(copyWidth) * 4;
	WGPUQueue queue = wgpuDeviceGetQueue(device);
	wgpuQueueWriteTexture(queue, &destination, image->pixels.data() + offset, dataSize, &layout, &size);
	wgpuQueueRelease(queue);
	return true;
}

void CancelImage(int job)
{
	Loader &loader = GetLoader();
	loader.Discard(loader.Take(job));
}

bool FetchText(const char *url, std::string &text, bool &failed)
{
	std::vector<uint8_t> bytes;
	const std::string path = ResolvePath(url);
	failed = !ReadFile(path, bytes);
	if (failed) {
		LogError("HD text load failed: {}", path);
		return false;
	}
	text.assign(bytes.begin(), bytes.end());
	return true;
}

} // namespace devilution::hd_loader

#endif // DEVILUTIONX_WEBGPU
