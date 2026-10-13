# Only StormLib's decompressors are used (Huffman, IMA ADPCM and PKWARE DCL explode), by the
# Diablo II sound importer's MPQ reader (see Source/d2sound/d2_mpq.cpp). Diablo II stores most
# of its sounds with the MPQ "wave" compression (ADPCM + Huffman), which mpqfs does not support.
# The rest of StormLib is not built: its file stream functions would clash with CascLib's.
include(FetchContent)
# `SOURCE_SUBDIR` points to a directory without a CMakeLists.txt, so nothing is added.
FetchContent_Declare(stormlib
  URL https://github.com/ladislav-zezula/StormLib/archive/44ebfbfc109d76e2a85bbd5d8b0c949df7e65c6f.tar.gz
  URL_HASH SHA256=a0b12751f3ff48cd63c091d8d216f429fb2c3c753e49e5842cf4f20f35ff9438
  SOURCE_SUBDIR devilutionx-sources-only
)
FetchContent_MakeAvailable(stormlib)

add_library(stormlib_codecs STATIC
  ${stormlib_SOURCE_DIR}/src/adpcm/adpcm.cpp
  ${stormlib_SOURCE_DIR}/src/huffman/huff.cpp
  ${stormlib_SOURCE_DIR}/src/pklib/explode.c
)
target_include_directories(stormlib_codecs PUBLIC
  ${stormlib_SOURCE_DIR}/src/adpcm
  ${stormlib_SOURCE_DIR}/src/huffman
  ${stormlib_SOURCE_DIR}/src/pklib
)

# External library, disable warnings.
target_compile_options(stormlib_codecs PRIVATE -w)
