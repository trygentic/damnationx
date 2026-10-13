#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace devilution {

enum class AudioFileFormat : uint8_t {
	Wav,
	Mp3,
	Flac,
};

/**
 * @brief Number of leading bytes that `DetectAudioFileFormat` looks at.
 */
constexpr size_t AudioFileFormatMagicSize = 4;

/**
 * @brief Detects the format of an audio file from its first bytes.
 *
 * The file name extension is not always reliable: sound packs (such as the ones imported from
 * Diablo II: Resurrected) store FLAC data under the `.wav` file name that the game requests.
 *
 * @param header The first bytes of the file (up to `AudioFileFormatMagicSize`).
 * @param isMp3 What the file name says the format is, used when the data is not recognized.
 */
inline AudioFileFormat DetectAudioFileFormat(std::span<const uint8_t> header, bool isMp3)
{
	if (header.size() >= 4) {
		if (header[0] == 'f' && header[1] == 'L' && header[2] == 'a' && header[3] == 'C')
			return AudioFileFormat::Flac;
		if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F')
			return AudioFileFormat::Wav;
	}
	return isMp3 ? AudioFileFormat::Mp3 : AudioFileFormat::Wav;
}

} // namespace devilution
