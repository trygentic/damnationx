# CascLib reads Blizzard CASC storages. It is only used by the Diablo II sound importer
# (see Source/d2sound/) to read the sound files of an installed Diablo II: Resurrected.
include(FetchContent)
# Only the sources are needed: we build our own static library target below instead of using
# CascLib's CMakeLists.txt, which builds a shared library and adds install rules.
# `SOURCE_SUBDIR` points to a directory without a CMakeLists.txt, so nothing is added.
FetchContent_Declare(casclib
  URL https://github.com/ladislav-zezula/CascLib/archive/38a34665624b8775bb875274b36191b21c38d97b.tar.gz
  URL_HASH SHA256=3ae220d2154b21121a5f33ef2c1d86c522bde776004fa5e8240cf12e892211dc
  SOURCE_SUBDIR devilutionx-sources-only
)
FetchContent_MakeAvailable(casclib)

add_library(casclib STATIC
  ${casclib_SOURCE_DIR}/src/common/Common.cpp
  ${casclib_SOURCE_DIR}/src/common/Directory.cpp
  ${casclib_SOURCE_DIR}/src/common/Csv.cpp
  ${casclib_SOURCE_DIR}/src/common/FileStream.cpp
  ${casclib_SOURCE_DIR}/src/common/FileTree.cpp
  ${casclib_SOURCE_DIR}/src/common/ListFile.cpp
  ${casclib_SOURCE_DIR}/src/common/Mime.cpp
  ${casclib_SOURCE_DIR}/src/common/RootHandler.cpp
  ${casclib_SOURCE_DIR}/src/common/Sockets.cpp
  ${casclib_SOURCE_DIR}/src/hashes/md5.cpp
  ${casclib_SOURCE_DIR}/src/hashes/sha1.cpp
  ${casclib_SOURCE_DIR}/src/jenkins/lookup3.c
  ${casclib_SOURCE_DIR}/src/overwatch/apm.cpp
  ${casclib_SOURCE_DIR}/src/overwatch/cmf.cpp
  ${casclib_SOURCE_DIR}/src/overwatch/aes.cpp
  ${casclib_SOURCE_DIR}/src/CascDecompress.cpp
  ${casclib_SOURCE_DIR}/src/CascDecrypt.cpp
  ${casclib_SOURCE_DIR}/src/CascDumpData.cpp
  ${casclib_SOURCE_DIR}/src/CascFiles.cpp
  ${casclib_SOURCE_DIR}/src/CascFindFile.cpp
  ${casclib_SOURCE_DIR}/src/CascIndexFiles.cpp
  ${casclib_SOURCE_DIR}/src/CascOpenFile.cpp
  ${casclib_SOURCE_DIR}/src/CascOpenStorage.cpp
  ${casclib_SOURCE_DIR}/src/CascReadFile.cpp
  ${casclib_SOURCE_DIR}/src/CascRootFile_Diablo3.cpp
  ${casclib_SOURCE_DIR}/src/CascRootFile_Install.cpp
  ${casclib_SOURCE_DIR}/src/CascRootFile_MNDX.cpp
  ${casclib_SOURCE_DIR}/src/CascRootFile_Text.cpp
  ${casclib_SOURCE_DIR}/src/CascRootFile_TVFS.cpp
  ${casclib_SOURCE_DIR}/src/CascRootFile_OW.cpp
  ${casclib_SOURCE_DIR}/src/CascRootFile_WoW.cpp
)
target_include_directories(casclib PUBLIC ${casclib_SOURCE_DIR}/src)
target_compile_definitions(casclib
  PUBLIC CASCLIB_NO_AUTO_LINK_LIBRARY CASCLIB_NODEBUG
  PRIVATE CASC_USE_SYSTEM_ZLIB)
target_link_libraries(casclib PRIVATE ZLIB::ZLIB)
if(WIN32)
  target_link_libraries(casclib PRIVATE ws2_32 wininet)
elseif(EMSCRIPTEN)
  # Built into the single-threaded browser import worker (Source/d2sound/d2sound_web_worker.cpp):
  # no -pthread (CascLib's locks use Emscripten's no-op pthread stubs). musl only declares the
  # `*64` file functions that CascLib uses with _LARGEFILE64_SOURCE.
  target_compile_definitions(casclib PRIVATE LPDWORD=PDWORD _LARGEFILE64_SOURCE)
else()
  # CascFiles.cpp uses the Windows-only `LPDWORD` type, which CascPort.h does not define on
  # other platforms (it only defines the equivalent `PDWORD`).
  target_compile_definitions(casclib PRIVATE LPDWORD=PDWORD)
  find_package(Threads REQUIRED)
  target_link_libraries(casclib PRIVATE Threads::Threads)
endif()

# External library, disable warnings.
target_compile_options(casclib PRIVATE -w)
