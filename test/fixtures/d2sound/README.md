# d2sound fixtures

Synthetic files for `test/d2sound_test.cpp` (the Diablo II sound importer).

- `tone.wav`: 0.25 s of a decaying 440 Hz tone with noise (22050 Hz, mono, 16-bit), generated with numpy.
- `text.txt`: "The quick brown fox jumps over the lazy dog. " repeated 120 times.
- `tone.flac`: `tone.wav` encoded with `flac --best --no-padding`.
- `test.mpq`: an MPQ v1 archive (sector size 4096) made with StormLib 9.30 (`SFileCreateArchive` with
  `MPQ_CREATE_ARCHIVE_V1`, then `SFileAddFileEx`), covering the compressions that Diablo II uses:

  | File                                  | Source        | Flags                              | Compression (first sector / rest) |
  | ------------------------------------- | ------------- | ---------------------------------- | --------------------------------- |
  | `data\global\sfx\Test\wave_mono.wav`  | `tone.wav`    | compress, encrypted, fix key       | PKWARE / Huffman + ADPCM mono     |
  | `data\global\sfx\Test\wave_stereo.wav`| stereo tone   | compress, encrypted                | PKWARE / Huffman + ADPCM stereo   |
  | `data\local\sfx\implode.wav`          | `tone.wav`    | implode, encrypted                 | PKWARE (no compression mask)      |
  | `data\pkware.txt`                     | `text.txt`    | compress                           | PKWARE                            |
  | `data\zlib.txt`                       | `text.txt`    | compress, encrypted, fix key       | zlib                              |
  | `data\huff.txt`                       | `text.txt`    | compress                           | Huffman                           |
  | `data\single.txt`                     | `text.txt`    | compress, single unit, encrypted   | zlib                              |
  | `data\stored.txt`                     | `text.txt`    | encrypted                          | none                              |

- `wave_mono_expected.wav`, `wave_stereo_expected.wav`: the ADPCM-compressed (lossy) files as extracted by
  StormLib's `SFileExtractFile`.
