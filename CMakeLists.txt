cmake_minimum_required(VERSION 3.22)

foreach(_policy CMP0135 CMP0141)
  if(POLICY ${_policy})
    cmake_policy(SET ${_policy} NEW)
    set(CMAKE_POLICY_DEFAULT_${_policy} NEW)
  endif()
endforeach()

# Projects added via `add_subdirectory` or `FetchContent` may have a lower
# `cmake_minimum_required` than we set here. Set policies that we require
# to their new value so that they still apply.
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
  message("-- Detected a source distribution with the required FetchContent dependencies and devilutionx.mpq included")
  set(SRC_DIST ON)
  add_subdirectory(dist)
endif()

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(WARNING [[In-source build detected, please eg. create a new directory and use `cmake ..`]])
endif()

include(CMakeDependentOption)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake/finders")
include(functions/genex)

# Options required by `VcPkgManifestFeatures`, which must be included before the `project` call.
option(USE_SDL1 "Use SDL1.2 instead of SDL2" OFF)
option(USE_SDL3 "Use SDL3 instead of SDL2" OFF)
option(DEVILUTIONX_WEBGPU "Build the shared WebGPU client" OFF)
option(NONET "Disable network support" OFF)
cmake_dependent_option(PACKET_ENCRYPTION "Encrypt network packets" ON "NOT NONET" OFF)
# The gettext[tools] package takes a very long time to install
if(CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake$")
  option(USE_GETTEXT_FROM_VCPKG "Add vcpkg dependency for gettext[tools] for compiling translations" OFF)
endif()
option(BUILD_TESTING "Build tests." ON)

# The restricted WebAssembly mod host is not finished.
# Keep the integration opt-in until that ABI and its capability policy are
# available; Dependencies.cmake performs the pinned toolchain checks when enabled.
option(DEVILUTIONX_ENABLE_WASM_MOD_HOST "Enable the dxmod-core-1 Wasm mod host" OFF)
set(DEVILUTIONX_RUST_VERSION "1.82.0" CACHE STRING "Pinned Rust version for the Wasm mod host")
set(DEVILUTIONX_EMSCRIPTEN_VERSION "3.1.74" CACHE STRING "Pinned Emscripten version for the Wasm mod host")
set(DEVILUTIONX_WASMI_VERSION "0.31.2" CACHE STRING "Pinned wasmi version for the mod runtime (native and browser)")
set(DEVILUTIONX_WASM_RUST_TARGET "wasm32-unknown-emscripten" CACHE STRING "Rust target for the preferred browser adapter")
set(DEVILUTIONX_WASM_STD_ABI "rustc-${DEVILUTIONX_RUST_VERSION}-emscripten" CACHE STRING "Pinned Rust standard-library ABI identifier")
set(DEVILUTIONX_WASM_ALLOCATOR "dlmalloc" CACHE STRING "Allocator used by the restricted Wasm adapter")
set(DEVILUTIONX_WASM_PANIC "abort" CACHE STRING "Panic strategy used by the restricted Wasm adapter")
mark_as_advanced(
  DEVILUTIONX_RUST_VERSION DEVILUTIONX_EMSCRIPTEN_VERSION
  DEVILUTIONX_WASMI_VERSION
  DEVILUTIONX_WASM_RUST_TARGET DEVILUTIONX_WASM_STD_ABI
  DEVILUTIONX_WASM_ALLOCATOR DEVILUTIONX_WASM_PANIC)

# These must be included after the options above but before the `project` call.
include(VcPkgManifestFeatures)

# Set up the `project` before the rest of the options so that:
#
# 1. Properties such as `TARGET_SUPPORTS_SHARED_LIBS` are defined.
# 2. Toolchain file is evaluated, required for `Platforms.cmake`,
#    which can override the options.
file(STRINGS "VERSION" VERSION_STR)
if(NOT "${VERSION_STR}" STREQUAL "")
  string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+).*" VERSION_PREFIX ${VERSION_STR})
  set(VERSION_NUM ${CMAKE_MATCH_1})
endif()

if(NOT VERSION_SUFFIX)
  # For versions with a suffix, e.g. "1.5.0-dev", include
  # the build type and the git hash.
  set(VERSION_SUFFIX "-$<CONFIG>")
  if(VERSION_PREFIX MATCHES "-")
    if(NOT GIT_COMMIT_HASH)
      include(functions/git)
      get_git_commit_hash(GIT_COMMIT_HASH)
    endif()
    if(GIT_COMMIT_HASH)
      set(VERSION_SUFFIX "${VERSION_SUFFIX}-${GIT_COMMIT_HASH}")
    endif()
  else()
    # For versions without a suffix, e.g. "1.5.0" rather than "1.5.0-dev",
    # only include the build type if it is a debug build.
    set(VERSION_SUFFIX "$<$<CONFIG:Debug>:$<CONFIG>>")
  endif()
endif()

# The iOS toolchain file reads DEPLOYMENT_TARGET when it is processed by
# `project()`, so the default must be set before that.
# libc++'s <format> requires the floating-point std::to_chars symbols,
# which are only available in the iOS 16.3+ system libc++.
if(CMAKE_TOOLCHAIN_FILE MATCHES "ios\\.toolchain\\.cmake" AND NOT DEFINED DEPLOYMENT_TARGET)
  set(DEPLOYMENT_TARGET "16.3")
endif()

project(DevilutionX
  VERSION ${VERSION_NUM}
  LANGUAGES C CXX)
set(PROJECT_VERSION_WITH_SUFFIX "${VERSION_PREFIX}${VERSION_SUFFIX}")

# Release identity is shared by native packages, browser shell/Wasm/data assets,
# WGSL/runtime assets, and reference mod packs. Keep it explicit and validate it
# at configure time so immutable artifacts cannot silently drift apart.
set(DEVILUTIONX_RELEASE_VERSION "${VERSION_STR}" CACHE STRING "Release version declared by VERSION and manifest.ini")
set(DEVILUTIONX_RELEASE_COMMIT "" CACHE STRING "Release source commit (defaults to the current Git commit)")
set(DEVILUTIONX_RELEASE_NAMESPACE "devilutionx" CACHE STRING "Immutable release artifact namespace")

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/manifest.ini" _release_manifest_lines)
set(_manifest_release_version "")
set(_manifest_release_commit "")
set(_manifest_release_namespace "")
set(_in_release_section OFF)
foreach(_line IN LISTS _release_manifest_lines)
  if(_line MATCHES "^\\[release\\][ \\t]*$")
    set(_in_release_section ON)
  elseif(_line MATCHES "^\\[.*\\][ \\t]*$")
    set(_in_release_section OFF)
  elseif(_in_release_section AND _line MATCHES "^version=([^#;]+)$")
    string(STRIP "${CMAKE_MATCH_1}" _manifest_release_version)
  elseif(_in_release_section AND _line MATCHES "^commit=([^#;]+)$")
    string(STRIP "${CMAKE_MATCH_1}" _manifest_release_commit)
  elseif(_in_release_section AND _line MATCHES "^artifactNamespace=([^#;]+)$")
    string(STRIP "${CMAKE_MATCH_1}" _manifest_release_namespace)
  endif()
endforeach()

if(NOT _manifest_release_version OR NOT DEVILUTIONX_RELEASE_VERSION)
  message(FATAL_ERROR "Release metadata is missing a version in VERSION, manifest.ini, or DEVILUTIONX_RELEASE_VERSION")
endif()
if(NOT "${DEVILUTIONX_RELEASE_VERSION}" STREQUAL "${VERSION_STR}" OR
   NOT "${_manifest_release_version}" STREQUAL "${VERSION_STR}")
  message(FATAL_ERROR "Release version mismatch: VERSION='${VERSION_STR}', manifest.ini='${_manifest_release_version}', configured='${DEVILUTIONX_RELEASE_VERSION}'")
endif()

if(NOT DEVILUTIONX_RELEASE_COMMIT)
  include(functions/git)
  get_git_commit_hash(DEVILUTIONX_RELEASE_COMMIT)
endif()
if(NOT DEVILUTIONX_RELEASE_COMMIT OR DEVILUTIONX_RELEASE_COMMIT MATCHES "^[ \\t]*$")
  message(FATAL_ERROR "Release commit metadata is missing; set -DDEVILUTIONX_RELEASE_COMMIT=<full-or-short-commit>")
endif()
if(_manifest_release_commit AND NOT "${_manifest_release_commit}" STREQUAL "auto" AND
   NOT "${_manifest_release_commit}" STREQUAL "${DEVILUTIONX_RELEASE_COMMIT}")
  message(FATAL_ERROR "Release commit mismatch: manifest.ini='${_manifest_release_commit}', configured='${DEVILUTIONX_RELEASE_COMMIT}'")
endif()
if(NOT _manifest_release_namespace OR NOT "${_manifest_release_namespace}" STREQUAL "${DEVILUTIONX_RELEASE_NAMESPACE}")
  message(FATAL_ERROR "Release artifact namespace mismatch between manifest.ini and DEVILUTIONX_RELEASE_NAMESPACE")
endif()

set(DEVILUTIONX_RELEASE_ID "${DEVILUTIONX_RELEASE_NAMESPACE}-${DEVILUTIONX_RELEASE_VERSION}-${DEVILUTIONX_RELEASE_COMMIT}" CACHE INTERNAL "Validated immutable release identity")
message(STATUS "Validated release identity: ${DEVILUTIONX_RELEASE_ID}")

# Platform definitions can override options and we want `cmake_dependent_option` to see the effects.
# Note that a few options are still defined before this because they're needed by `VcPkgManifestFeatures.cmake`.
include(Platforms)

# This built-in CMake module adds a BUILD_TESTING option (ON by default).
# Must be included in the top-level `CMakeLists.txt` after calling `project`.
# Because we must include `VcPkgManifestFeatures` before the `project` call,
# we add a BUILD_TESTING option ourselves above as well.
include(CTest)

# Debugging / profiling options
DEBUG_OPTION(ASAN "Enable address sanitizer")
DEBUG_OPTION(UBSAN "Enable undefined behaviour sanitizer")
option(TSAN "Enable thread sanitizer (not compatible with ASAN=ON)" OFF)
DEBUG_OPTION(DEBUG "Enable debug mode in engine")
option(GPERF "Build with GPerfTools profiler" OFF)
cmake_dependent_option(GPERF_HEAP_FIRST_GAME_ITERATION "Save heap profile of the first game iteration" OFF "GPERF" OFF)
option(ENABLE_CODECOVERAGE "Instrument code for code coverage (only enabled with BUILD_TESTING)" OFF)

# Packaging options
RELEASE_OPTION(CPACK "Configure CPack")
option(MACOSX_STANDALONE_APP_BUNDLE "Generate a portable app bundle to use on other devices (requires sudo)" OFF)
option(WIN_NSIS "Generate an NSIS installer" OFF)

# Network options
cmake_dependent_option(DISABLE_TCP "Disable TCP multiplayer option" OFF "NOT NONET" ON)
cmake_dependent_option(DISABLE_ZERO_TIER "Disable ZeroTier multiplayer option" OFF "NOT NONET" ON)
if(EMSCRIPTEN)
  set(_lobby_default ON)
else()
  set(_lobby_default OFF)
endif()
cmake_dependent_option(DEVILUTIONX_LOBBY "Build the public lobby provider" ${_lobby_default} "NOT NONET" OFF)
set(DEVILUTIONX_DEFAULT_LOBBY_ORIGIN "https://lobby.damnationx.net" CACHE STRING "Lobby that builds use: desktop builds when DAMNATIONX_LOBBY_ORIGIN is unset, browser builds unless served from localhost (empty: none)")

if(USE_SDL1 AND USE_SDL3)
  message(FATAL_ERROR "USE_SDL1 and USE_SDL3 cannot be set at the same time")
endif()

# Graphics options
if(NOT USE_SDL1)
if(USE_SDL3)
  set(_texture_format_default "SDL_PIXELFORMAT_XRGB8888")
else()
  set(_texture_format_default "SDL_PIXELFORMAT_RGB888")
endif()
  set(DEVILUTIONX_DISPLAY_TEXTURE_FORMAT "${_texture_format_default}" CACHE STRING "Texture format for DevilutionX textures when using the GPU renderer")
  mark_as_advanced(DEVILUTIONX_DISPLAY_TEXTURE_FORMAT)
endif()

if(USE_SDL1)
  # SDL_image in SDL1 does not support PNG, making PCX the only option.
  set(DEVILUTIONX_SCREENSHOT_FORMAT "DEVILUTIONX_SCREENSHOT_FORMAT_PCX")
else()
  set(DEVILUTIONX_SCREENSHOT_FORMAT "DEVILUTIONX_SCREENSHOT_FORMAT_PNG" CACHE STRING "Screenshot format")
  set_property(CACHE DEVILUTIONX_SCREENSHOT_FORMAT PROPERTY STRINGS "DEVILUTIONX_SCREENSHOT_FORMAT_PNG;DEVILUTIONX_SCREENSHOT_FORMAT_PCX")
  mark_as_advanced(DEVILUTIONX_SCREENSHOT_FORMAT)
endif()

# Sound options
option(NOSOUND "Disable sound support" OFF)
option(DEVILUTIONX_RESAMPLER_SPEEX "Build with Speex resampler" ON)
cmake_dependent_option(DEVILUTIONX_RESAMPLER_SDL "Build with SDL resampler" ON "NOT USE_SDL1" OFF)
if(DEVILUTIONX_RESAMPLER_SPEEX)
  list(APPEND _resamplers Speex)
endif()
if(DEVILUTIONX_RESAMPLER_SDL)
  list(APPEND _resamplers SDL)
endif()
list(GET _resamplers 0 _default_resampler)
set(DEVILUTIONX_DEFAULT_RESAMPLER ${_default_resampler} CACHE STRING "Default resampler")
set_property(CACHE DEVILUTIONX_DEFAULT_RESAMPLER PROPERTY STRINGS ${_resamplers})

# Optimization / link options
option(DISABLE_LTO "Disable link-time optimization (by default enabled in release mode)" OFF)
option(PIE "Generate position-independent code" OFF)
cmake_dependent_option(DEVILUTIONX_DISABLE_RTTI "Disable RTTI" ON "NONET" OFF)
cmake_dependent_option(DEVILUTIONX_DISABLE_EXCEPTIONS "Disable exceptions" ON "DISABLE_ZERO_TIER" OFF)
RELEASE_OPTION(DEVILUTIONX_STATIC_CXX_STDLIB "Link C++ standard library statically (if available)")
option(DEVILUTIONX_PROFILE_GENERATE "Build a binary that generates the profile for PGO" OFF)
option(DEVILUTIONX_PROFILE_USE "Build with PGO using the given profile file" OFF)
set(DEVILUTIONX_PROFILE_DIR "" CACHE STRING "Directory where the profile is stored")

include(MoldLinker)

# Memory / performance trade-off options
option(UNPACKED_MPQS "Expect MPQs to be unpacked and the data converted with devilutionx-mpq-tools" OFF)
option(UNPACKED_SAVES "Uses unpacked save files instead of MPQ .sv/.hsv files" OFF)
option(DISABLE_STREAMING_MUSIC "Disable streaming music (to work around broken platform implementations)" OFF)
mark_as_advanced(DISABLE_STREAMING_MUSIC)
option(DISABLE_STREAMING_SOUNDS "Disable streaming sounds (to work around broken platform implementations)" OFF)
mark_as_advanced(DISABLE_STREAMING_SOUNDS)
set(STREAM_ALL_AUDIO_MIN_FILE_SIZE "" CACHE STRING "If set, stream all the audio files larger than this size")
mark_as_advanced(STREAM_ALL_AUDIO_MIN_FILE_SIZE)
option(DEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT "Whether to use a lookup table for transparency blending with black. This improves performance of blending transparent black overlays, such as quest dialog background, at the cost of 128 KiB of RAM." ON)
mark_as_advanced(DEVILUTIONX_PALETTE_TRANSPARENCY_BLACK_16_LUT)

# Additional features
option(DISABLE_DEMOMODE "Disable demo mode support" OFF)
option(DISCORD_INTEGRATION "Build with Discord SDK for rich presence support" OFF)
option(SCREEN_READER_INTEGRATION "Build with screen reader support" OFF)
mark_as_advanced(SCREEN_READER_INTEGRATION)

# Importing the sounds of an installed Diablo II / Diablo II: Resurrected is only useful on desktop systems
# and in the browser (where the page imports the files that the user chooses).
if((TARGET_PLATFORM STREQUAL "host" OR EMSCRIPTEN) AND NOT (NOSOUND OR ANDROID OR IOS OR UWP_LIB OR NINTENDO_SWITCH OR NINTENDO_3DS OR VITA OR PS4 OR AMIGA OR HAIKU OR USE_SDL1))
  set(_d2_sound_import_default ON)
else()
  set(_d2_sound_import_default OFF)
endif()
cmake_dependent_option(DEVILUTIONX_D2_SOUND_IMPORT "Allow importing sound packs from an installed Diablo II or Diablo II: Resurrected" ${_d2_sound_import_default} "NOT NOSOUND" OFF)
if(EMSCRIPTEN AND _d2_sound_import_default AND NOT DEVILUTIONX_D2_SOUND_IMPORT)
  # The option used to default to OFF in the browser, and a build directory keeps that value.
  message(WARNING "DEVILUTIONX_D2_SOUND_IMPORT is OFF: the page's \"Diablo II Sounds\" dialog can't import. Configure with -DDEVILUTIONX_D2_SOUND_IMPORT=ON to enable it.")
endif()

# If both UNPACKED_MPQS and UNPACKED_SAVES are enabled, we completely remove MPQ support.
if(UNPACKED_MPQS AND UNPACKED_SAVES)
  set(SUPPORTS_MPQ OFF)
else()
  set(SUPPORTS_MPQ ON)
endif()

# By default, devilutionx.mpq and mods/hf.mpq are built only if smpq is installed and MPQ support is enabled.
if(SUPPORTS_MPQ AND NOT UNPACKED_MPQS)
  if(BUILD_ASSETS_MPQ OR (CPACK STREQUAL "ON" AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")))
    find_program(SMPQ smpq REQUIRED)
  elseif(NOT DEFINED BUILD_ASSETS_MPQ AND NOT SRC_DIST)
    find_program(SMPQ smpq)
  endif()
  if(SMPQ)
    set(_has_smpq ON)
  else()
    set(_has_smpq OFF)
  endif()
  option(BUILD_ASSETS_MPQ "If true, assets are packaged into devilutionx.mpq and mods/hf to hf.mpq." ${_has_smpq})
else()
  set(BUILD_ASSETS_MPQS OFF)
endif()

# === Option overrides ===
# TSAN is not compatible with ASAN.
if(TSAN)
  set(ASAN OFF)
endif()

if(MSVC AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT DISABLE_LTO)
  # Work around MSVC + CMake bug when LTO is enabled.
  # See https://github.com/diasurgical/devilutionX/issues/3778
  # and https://gitlab.kitware.com/cmake/cmake/-/issues/23035
  set(BUILD_TESTING OFF)
endif()

# Note: `CMAKE_CROSSCOMPILING` is only available after the `project` call.
if(CMAKE_CROSSCOMPILING)
  set(BUILD_TESTING OFF)
endif()

if(DISABLE_DEMOMODE)
  # Testing requires demomode.
  set(BUILD_TESTING OFF)
endif()

if(BUILD_TESTING)
  # When tests are enabled, we build a shared devilutionx_so library, which needs to be PIC to link.
  set(PIE ON)
endif()

# Recalculate the dependent options that are defined before `include(Platforms)`:
if(NONET)
  # PACKET_ENCRYPTION is defined before `Platforms.cmake` is included.
  # This means that if a `Platforms.cmake` sets NONET to OFF, PACKET_ENCRYPTION will not automatically
  # reflect that.
  set(PACKET_ENCRYPTION OFF)
endif()
# === End of option overrides ===

if(PIE)
  set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

if(DEVILUTIONX_DISABLE_RTTI)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
  elseif(MSVC)
    string(REGEX REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  endif()
endif()

if(DEVILUTIONX_DISABLE_EXCEPTIONS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
  elseif(MSVC)
    string(REGEX REPLACE "/EHsc" "/EHs-c-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  endif()
endif()

# Remove unused symbols in non-debug mode.
# This is useful even with LTO (-84 KiB with MinSizeRel).
#
# PS4 toolchain crashes in `create-fself` when linking with these flags, so we exclude it:
# https://github.com/PacBrew/ps4-openorbis/issues/8
# On m68k-amigaos, `--gc-sections` discards static constexpr data that is still
# referenced (magic_enum's per-enum name tables are the visible casualty: the
# string_views survive with correct lengths but point at collected storage, so
# every enum name reads as garbage). Excluded here for the same reason as PS4.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT PS4 AND NOT AMIGA)
  add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-ffunction-sections;-fdata-sections>")
  if(APPLE)
    add_link_options("$<$<NOT:$<CONFIG:Debug>>:LINKER:-dead_strip>")
  else()
    add_link_options("$<$<NOT:$<CONFIG:Debug>>:LINKER:--gc-sections>")
    # Emscripten's linker (wasm-ld) does not support --as-needed.
    if(NOT EMSCRIPTEN)
      add_link_options("$<$<NOT:$<CONFIG:Debug>>:LINKER:--as-needed>")
    endif()
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  # -fipa-pta and -fdevirtualize-at-ltrans improve performance.
  add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-fipa-pta;-fdevirtualize-at-ltrans>")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options("$<$<BOOL:${DEVILUTIONX_PROFILE_GENERATE}>:-fprofile-generate>")
  add_link_options("$<$<BOOL:${DEVILUTIONX_PROFILE_GENERATE}>:-fprofile-generate>")
  add_compile_options("$<$<BOOL:${DEVILUTIONX_PROFILE_USE}>:-fprofile-use>")
  add_link_options("$<$<BOOL:${DEVILUTIONX_PROFILE_USE}>:-fprofile-use>")
  add_compile_options("$<$<BOOL:${DEVILUTIONX_PROFILE_DIR}>:-fprofile-dir=${DEVILUTIONX_PROFILE_DIR};-fprofile-prefix-path=${CMAKE_CURRENT_BINARY_DIR}>")
  add_link_options("$<$<BOOL:${DEVILUTIONX_PROFILE_DIR}>:-fprofile-dir=${DEVILUTIONX_PROFILE_DIR};-fprofile-prefix-path=${CMAKE_CURRENT_BINARY_DIR}>")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  # u8path() function is deprecated but there is no sensible alternative and it might even get un-deprecated.
  add_definitions(-D_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
  if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.25")
    # This enables Edit & Continue support, see https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio#edit-and-continue-for-cmake-projects
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:EditAndContinue>") # Sets /ZI compiler option, see https://cmake.org/cmake/help/latest/variable/CMAKE_MSVC_DEBUG_INFORMATION_FORMAT.html
    add_link_options("$<$<CONFIG:Debug>:/INCREMENTAL>")
  endif()
endif()

# Not a genexp because CMake doesn't support it
# https://gitlab.kitware.com/cmake/cmake/-/issues/20546
if(NOT DISABLE_LTO AND NOT EMSCRIPTEN)
  # LTO if supported:
  include(CheckIPOSupported)
  check_ipo_supported(RESULT is_ipo_supported OUTPUT lto_error)
  if(is_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
  endif()
endif()

if(GPERF)
  set(DEVILUTIONX_STATIC_CXX_STDLIB OFF)

  if(GPERF_HEAP_FIRST_GAME_ITERATION)
    set(GPERF_HEAP_MAIN ON)
  endif()

  # Compile with information about file and line numbers for everything
  # even in non-Debug build types.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-g2>")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Use the more size-efficient `-gmlt` option on clang.
    add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-gmlt>")
  endif()
endif()

set(CMAKE_CXX_STANDARD 23)

# On some platforms, such as DJGPP,
# `-std=c++23` defines `__STRICT_ANSI__` which disables
# all POSIX extensions, so we need to use `-std=gnu++23` instead.
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clang-tidy
set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
if(EMSCRIPTEN)
  # The browser target is deliberately single-threaded. CMake's stock
  # FindThreads adds -pthread, which makes Emscripten enable pthread support
  # despite the browser platform profile disabling it at link time.
  include(platforms/threads-stub/FindThreads)
  set_property(TARGET Threads::Threads PROPERTY INTERFACE_COMPILE_OPTIONS "")
  set_property(TARGET Threads::Threads PROPERTY INTERFACE_LINK_OPTIONS "")
  set_property(TARGET Threads::Threads PROPERTY INTERFACE_LINK_LIBRARIES "")
else()
  find_package(Threads REQUIRED)
endif()

# Dependencies must be included after Platforms.
include(Dependencies)

# If a C++ library is linked dynamically, it will load libstdc++.so at runtime.
# Mixing `-static-libstdc++` with a dynamic C++ dependency causes both copies
# to initialize, which is UB and can cause crashes, e.g. in
# `std::ios_base_library_init()`.
if(NOT NOSOUND AND NOT DEVILUTIONX_STATIC_SDL_AUDIOLIB)
  set(DEVILUTIONX_STATIC_CXX_STDLIB OFF)
endif()

add_subdirectory(Source)

set(BIN_TARGET devilutionx)
if(NINTENDO_3DS)
  set(BIN_TARGET ${BIN_TARGET}.elf)
elseif(TARGET_PLATFORM STREQUAL "dos")
  set(BIN_TARGET devx)
endif()

if(ANDROID AND NOT TERMUX)
  add_library(${BIN_TARGET} SHARED Source/main.cpp)
elseif(UWP_LIB)
  set(BIN_TARGET libdevilutionx)
else()
  add_executable(${BIN_TARGET}
    WIN32
    MACOSX_BUNDLE
    Source/main.cpp
    Packaging/windows/devilutionx.exe.manifest
    Packaging/windows/devilutionx.rc
    Packaging/apple/LaunchScreen.storyboard)

    if(CMAKE_STRIP AND NOT DEVILUTIONX_DISABLE_STRIP AND NOT EMSCRIPTEN)
      add_custom_command(
        TARGET ${BIN_TARGET} POST_BUILD
        COMMAND $<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>:${CMAKE_STRIP}>
        ARGS $<TARGET_FILE:${BIN_TARGET}>)
    endif()
endif()

if(TARGET_PLATFORM STREQUAL "dos")
    # Allow multiple definitions for math stubs in DJGPP
    set_target_properties(${BIN_TARGET} PROPERTIES
        LINK_FLAGS "-Wl,--allow-multiple-definition -static"
    )
    target_link_libraries(${BIN_TARGET} PRIVATE m)
endif()

if(NOT UWP_LIB)
  target_link_dependencies(${BIN_TARGET} PRIVATE libdevilutionx)
endif()

if(GPERF)
  target_link_libraries(${BIN_TARGET} PUBLIC ${GPERFTOOLS_LIBRARIES})
endif()

# Must be included after `BIN_TARGET` and `libdevilutionx` are defined.
include(Assets)
include(Mods)

if(EMSCRIPTEN)
  if(DEVILUTIONX_WEBGPU)
    # Emdawnwebgpu port selection.
    # Preferred: the release package pinned by DEVILUTIONX_EMDAWNWEBGPU_REVISION, downloaded
    # once into the build directory and verified against DEVILUTIONX_EMDAWNWEBGPU_PORT_SHA256
    # (CMake/Dependencies.cmake). Emscripten's builtin `emdawnwebgpu` port is only used on
    # explicit request because it floats the Dawn snapshot with the Emscripten release.
    if(DEVILUTIONX_EMDAWNWEBGPU_PORT_FILE)
      if(NOT EXISTS "${DEVILUTIONX_EMDAWNWEBGPU_PORT_FILE}")
        message(FATAL_ERROR "DEVILUTIONX_EMDAWNWEBGPU_PORT_FILE does not exist: ${DEVILUTIONX_EMDAWNWEBGPU_PORT_FILE}")
      endif()
      set(_emdawnwebgpu_port "${DEVILUTIONX_EMDAWNWEBGPU_PORT_FILE}")
      message(STATUS "Emdawnwebgpu port: local file ${_emdawnwebgpu_port} (DEVILUTIONX_EMDAWNWEBGPU_PORT_FILE; the pinned revision is not verified)")
    elseif(NOT DEVILUTIONX_EMDAWNWEBGPU_BUILTIN_PORT)
      set(_emdawnwebgpu_dir "${CMAKE_BINARY_DIR}/emdawnwebgpu")
      set(_emdawnwebgpu_archive_name "emdawnwebgpu_pkg-${DEVILUTIONX_EMDAWNWEBGPU_REVISION}.zip")
      set(_emdawnwebgpu_archive "${_emdawnwebgpu_dir}/${_emdawnwebgpu_archive_name}")
      set(_emdawnwebgpu_url "https://github.com/google/dawn/releases/download/${DEVILUTIONX_EMDAWNWEBGPU_REVISION}/${_emdawnwebgpu_archive_name}")
      set(_emdawnwebgpu_port "${_emdawnwebgpu_dir}/emdawnwebgpu_pkg/emdawnwebgpu.port.py")
      set(_emdawnwebgpu_stamp "${_emdawnwebgpu_dir}/extracted-${DEVILUTIONX_EMDAWNWEBGPU_PORT_SHA256}.stamp")
      if(NOT EXISTS "${_emdawnwebgpu_port}" OR NOT EXISTS "${_emdawnwebgpu_stamp}")
        message(STATUS "Emdawnwebgpu port: downloading ${_emdawnwebgpu_url} (SHA256=${DEVILUTIONX_EMDAWNWEBGPU_PORT_SHA256})")
        file(DOWNLOAD "${_emdawnwebgpu_url}" "${_emdawnwebgpu_archive}"
          EXPECTED_HASH SHA256=${DEVILUTIONX_EMDAWNWEBGPU_PORT_SHA256}
          STATUS _emdawnwebgpu_download_status
          TLS_VERIFY ON)
        list(GET _emdawnwebgpu_download_status 0 _emdawnwebgpu_download_code)
        if(NOT _emdawnwebgpu_download_code EQUAL 0)
          list(GET _emdawnwebgpu_download_status 1 _emdawnwebgpu_download_message)
          file(REMOVE "${_emdawnwebgpu_archive}")
          message(FATAL_ERROR
            "Failed to fetch the pinned Emdawnwebgpu package ${_emdawnwebgpu_url}: ${_emdawnwebgpu_download_message}\n"
            "The package must match SHA256 ${DEVILUTIONX_EMDAWNWEBGPU_PORT_SHA256} (DEVILUTIONX_EMDAWNWEBGPU_PORT_SHA256). "
            "For an offline build, unpack that release's emdawnwebgpu_pkg zip yourself and pass -DDEVILUTIONX_EMDAWNWEBGPU_PORT_FILE=<...>/emdawnwebgpu_pkg/emdawnwebgpu.port.py.")
        endif()
        file(REMOVE_RECURSE "${_emdawnwebgpu_dir}/emdawnwebgpu_pkg")
        file(ARCHIVE_EXTRACT INPUT "${_emdawnwebgpu_archive}" DESTINATION "${_emdawnwebgpu_dir}")
        if(NOT EXISTS "${_emdawnwebgpu_port}")
          message(FATAL_ERROR "The Emdawnwebgpu package ${_emdawnwebgpu_archive_name} does not contain emdawnwebgpu_pkg/emdawnwebgpu.port.py")
        endif()
        file(WRITE "${_emdawnwebgpu_stamp}" "${_emdawnwebgpu_url}\n")
      endif()
      message(STATUS "Emdawnwebgpu port: pinned ${DEVILUTIONX_EMDAWNWEBGPU_REVISION} (SHA256=${DEVILUTIONX_EMDAWNWEBGPU_PORT_SHA256}) at ${_emdawnwebgpu_port}")
    elseif(EMSCRIPTEN_VERSION VERSION_GREATER_EQUAL 4.0.10)
      set(_emdawnwebgpu_port "emdawnwebgpu")
      message(WARNING "Emdawnwebgpu port: Emscripten's builtin port (DEVILUTIONX_EMDAWNWEBGPU_BUILTIN_PORT=ON). Its Dawn snapshot floats with Emscripten ${EMSCRIPTEN_VERSION} and is not the pinned ${DEVILUTIONX_EMDAWNWEBGPU_REVISION}.")
    else()
      message(FATAL_ERROR
        "DEVILUTIONX_EMDAWNWEBGPU_BUILTIN_PORT=ON requires Emscripten >= 4.0.10 (found ${EMSCRIPTEN_VERSION}): older releases ship no emdawnwebgpu port. "
        "Install Emscripten ${DEVILUTIONX_WEBGPU_EMSCRIPTEN_VERSION} in a second emsdk clone (see docs/building.md, Emscripten) or drop the option to use the pinned package.")
    endif()
    target_compile_options(libdevilutionx PRIVATE "SHELL:--use-port=${_emdawnwebgpu_port}:cpp_bindings=false")
    target_link_options(${BIN_TARGET} PRIVATE
      "SHELL:--use-port=${_emdawnwebgpu_port}:cpp_bindings=false"
      # The normal-plane arena and the G-buffer planes are carved from the initial heap so
      # they never trigger a mid-play growth (a full heap copy that invalidates JS views);
      # -sALLOW_MEMORY_GROWTH below stays as the safety net.
      -sINITIAL_MEMORY=512MB
    )
  endif()
  target_link_options(${BIN_TARGET} PRIVATE
    "SHELL:--preload-file assets"
    "SHELL:--preload-file mods"
    -sFORCE_FILESYSTEM=1
    -sALLOW_MEMORY_GROWTH=1
    -sASYNCIFY
    $<$<BOOL:${DEVILUTIONX_LOBBY}>:-lwebsocket.js>
    -sEXPORTED_RUNTIME_METHODS=callMain
    -lidbfs.js
    --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/Packaging/emscripten/index.html
  )

  add_dependencies(${BIN_TARGET} hellfire_copied_assets)
  # Add JavaScript to load MPQ files from the server directory at runtime
  target_link_options(${BIN_TARGET} PRIVATE --pre-js ${CMAKE_CURRENT_SOURCE_DIR}/Packaging/emscripten/emscripten_pre.js)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/diabdat.mpq")
    add_custom_command(TARGET ${BIN_TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${CMAKE_CURRENT_SOURCE_DIR}/diabdat.mpq"
        "${CMAKE_CURRENT_BINARY_DIR}/diabdat.mpq"
      COMMENT "Linking diabdat.mpq into the web build")
  else()
    message(WARNING "diabdat.mpq was not found in the project root; the browser will require an uploaded MPQ")
  endif()
endif()

if(NOT USE_SDL1 AND NOT USE_SDL3 AND NOT UWP_LIB)
  target_link_libraries(${BIN_TARGET} PUBLIC ${SDL2_MAIN})
endif()

if(BUILD_TESTING)
  include(Tests)
endif()

include(functions/set_relative_file_macro)
set_relative_file_macro(${BIN_TARGET})

if(APPLE)
  if(IOS)
    # iOS keeps saves in the app container, which follows the bundle id.
    set(MACOSX_BUNDLE_GUI_IDENTIFIER com.diasurgical.devilutionx)
  else()
    # Own identity so damnationx:// links never open stock DevilutionX. Saves and
    # diablo.ini stay in SDL_GetPrefPath("diasurgical", "devilution"), not keyed by this id.
    set(MACOSX_BUNDLE_GUI_IDENTIFIER net.damnationx)
  endif()
  set(MACOSX_BUNDLE_COPYRIGHT Unlicense)
  set(MACOSX_BUNDLE_BUNDLE_NAME devilutionx)
  set(MACOSX_BUNDLE_DISPLAY_NAME DevilutionX)
  set(MACOSX_BUNDLE_INFO_STRING ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_LONG_VERSION_STRING "Version ${PROJECT_VERSION}")
  if(IOS)
    set(MACOSX_BUNDLE_REQUIRED_PLATFORM IPhoneOS)
    set_target_properties(${BIN_TARGET} PROPERTIES XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    # libc++'s <format> requires the floating-point std::to_chars symbols,
    # which are only available in the iOS 16.3+ system libc++.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "16.3")
  elseif(DARWIN_MAJOR_VERSION GREATER_EQUAL 17)
    set(MACOSX_BUNDLE_REQUIRED_PLATFORM Carbon)
    # libc++'s <format> requires the floating-point std::to_chars symbols,
    # which are only available in the macOS 13.3+ system libc++.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
  endif()
  if(DARWIN_MAJOR_VERSION VERSION_LESS 9)
    # Finder on OSX Tiger can only handle icns files with up to 128x128 icons.
    set(_icon_file AppIcon_128)
  else()
    set(_icon_file AppIcon)
  endif()
  target_sources(${BIN_TARGET} PRIVATE "Packaging/apple/${_icon_file}.icns")
  set_source_files_properties("./Packaging/apple/${_icon_file}.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(${BIN_TARGET} PROPERTIES MACOSX_BUNDLE_ICON_FILE "${_icon_file}.icns")
  set_target_properties(${BIN_TARGET} PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Packaging/apple/Info.plist")

  install (TARGETS ${BIN_TARGET} DESTINATION ./)

  if(MACOSX_STANDALONE_APP_BUNDLE)
      install(CODE "
        include(BundleUtilities)
        fixup_bundle(${CMAKE_BINARY_DIR}/${MACOSX_BUNDLE_BUNDLE_NAME}.app \"\" \"\")
        "
        COMPONENT Runtime)
  endif()

  find_library(COREFOUNDATION_LIBRARY CoreFoundation)
  if(COREFOUNDATION_LIBRARY)
    target_link_libraries(libdevilutionx PUBLIC "${COREFOUNDATION_LIBRARY}")
    target_compile_definitions(libdevilutionx PRIVATE USE_COREFOUNDATION)
  endif()

  set(MACOSX_BUNDLE_LONG_VERSION_STRING "Version ${PROJECT_VERSION}")
  set(CPACK On)
endif()

if(NINTENDO_SWITCH)
  nx_generate_nacp (${BIN_TARGET}.nacp
    NAME    "DevilutionX"
    AUTHOR  "Devilution Team"
    VERSION "${PROJECT_VERSION}"
  )

  file(MAKE_DIRECTORY "${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}")
  nx_create_nro(${BIN_TARGET}
    NACP  ${BIN_TARGET}.nacp
    ICON  "${PROJECT_SOURCE_DIR}/Packaging/switch/icon.jpg"
    ROMFS ${DEVILUTIONX_ASSETS_OUTPUT_DIRECTORY}
  )
endif()

if(VITA)
  set(VITA_APP_NAME "devilutionX")
  set(VITA_TITLEID  "DVLX00001")
  set(VITA_VERSION  "01.00")
  set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")
  set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
  vita_create_self(devilutionx.self devilutionx UNSAFE)
  if(BUILD_ASSETS_MPQ OR SRC_DIST)
    vita_create_vpk(devilutionx.vpk ${VITA_TITLEID} devilutionx.self
      VERSION ${VITA_VERSION}
      NAME ${VITA_APP_NAME}
      FILE Packaging/vita/sce_sys sce_sys
      FILE ${DEVILUTIONX_MPQ} devilutionx.mpq
      FILE ${HELLFIRE_MPQ} mods/hf.mpq
    )
  else()
    vita_create_vpk(devilutionx.vpk ${VITA_TITLEID} devilutionx.self
      VERSION ${VITA_VERSION}
      NAME ${VITA_APP_NAME}
      FILE Packaging/vita/sce_sys sce_sys
      FILE assets assets
      ${VITA_TRANSLATIONS_LIST}
    )
  endif()

endif()

if(PS4)
  add_custom_command(
    TARGET devilutionx_mpq POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${DEVILUTIONX_MPQ} "${PROJECT_SOURCE_DIR}/Packaging/ps4/")
  add_custom_command(
    TARGET hellfire_mpq POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_SOURCE_DIR}/Packaging/ps4/mods"
    COMMAND ${CMAKE_COMMAND} -E copy ${HELLFIRE_MPQ} "${PROJECT_SOURCE_DIR}/Packaging/ps4/mods/")
  add_self(${BIN_TARGET})
  add_pkg(${BIN_TARGET} "${PROJECT_SOURCE_DIR}/Packaging/ps4"
    "DVLX00001" "DevilutionX" "${PROJECT_VERSION}")
endif()

if(NINTENDO_3DS)
  set(APP_TITLE       "DevilutionX")
  set(APP_DESCRIPTION "DevilutionX port for 3DS")
  set(APP_AUTHOR      "Diasurgical Team")
  set(APP_ICON        "${PROJECT_SOURCE_DIR}/Packaging/ctr/icon.png")
  set(APP_BANNER      "${PROJECT_SOURCE_DIR}/Packaging/ctr/banner.png")
  set(APP_AUDIO       "${CMAKE_BINARY_DIR}/banner_audio.wav")
  set(APP_RSF         "${PROJECT_SOURCE_DIR}/Packaging/ctr/template.rsf")
  set(APP_VERSION ${PROJECT_VERSION})

  find_program(FFMPEG ffmpeg)
  if(FFMPEG)
    add_custom_command(OUTPUT ${APP_AUDIO}
      COMMAND ${FFMPEG} -y -ss 3.3 -t 3 -i "${PROJECT_SOURCE_DIR}/Packaging/resources/shareware-startup.wav" -af "afade=t=in:st=0:d=0.1,afade=t=out:st=2.9:d=0.1" ${APP_AUDIO}
      DEPENDS ${PROJECT_SOURCE_DIR}/Packaging/resources/shareware-startup.wav
      VERBATIM)
  else()
    add_custom_command(OUTPUT ${APP_AUDIO}
      COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/Packaging/ctr/audio_silent.wav ${APP_AUDIO}
      DEPENDS ${PROJECT_SOURCE_DIR}/Packaging/ctr/audio_silent.wav
      VERBATIM)
  endif()

  include(Tools3DS)
  add_3dsx_target(${BIN_TARGET})
  add_cia_target(${BIN_TARGET} ${APP_RSF} ${APP_BANNER} ${APP_AUDIO})
endif()

if(NXDK)
  target_link_libraries(${BIN_TARGET} PRIVATE "${NXDK_DIR}/lib/libnxdk_automount_d.lib")
  target_link_options(${BIN_TARGET} PRIVATE "-include:_automount_d_drive")

  set(_nxdk_pkg_dir "${CMAKE_BINARY_DIR}/pkg")
  set(_xbe_path "${_nxdk_pkg_dir}/default.xbe")
  add_custom_command(
    OUTPUT "${_xbe_path}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_nxdk_pkg_dir}"
    COMMAND "${NXDK_DIR}/tools/cxbe/cxbe" "-OUT:${_xbe_path}" -TITLE:DevilutionX "-Logo:${PROJECT_SOURCE_DIR}/Packaging/xbox_nxdk/xbe_logo.pgm" "${CMAKE_BINARY_DIR}/${BIN_TARGET}.exe"
    DEPENDS "${BIN_TARGET}"
  )
  add_custom_target(nxdk_xbe DEPENDS "${_xbe_path}")
endif()

if(CPACK AND (APPLE OR BUILD_ASSETS_MPQ OR SRC_DIST))
  if(WIN32)
    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
      set(SDL2_WIN32_DLLS_DIR "${CMAKE_BINARY_DIR}")
    else()
      set(SDL2_WIN32_DLLS_DIR "${SDL2_EXEC_PREFIX}")
    endif()
    set(SDL2_WIN32_LICENSES_DIR "${PROJECT_SOURCE_DIR}/Packaging/resources")

    file(GLOB SDL2_WIN32_ALL_DLLS
      LIST_DIRECTORIES false
      "${SDL2_WIN32_DLLS_DIR}/*.dll")
    file(GLOB SDL2_WIN32_ALL_LICENSES
      LIST_DIRECTORIES false
      "${SDL2_WIN32_LICENSES_DIR}/LICENSE*.txt"
      "${SDL2_WIN32_LICENSES_DIR}/README*.txt")

    set(CPACK_PACKAGE_NAME ${project_name})
    if(WIN_NSIS)
      set(CPACK_PACKAGE_FILE_NAME "${PROJECT_NAME}_${PROJECT_VERSION}_Installer")
      set(CPACK_GENERATOR "NSIS")
      set(CPACK_PACKAGE_INSTALL_DIRECTORY "${PROJECT_NAME}")
      set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/Packaging/windows/icon.ico")
      set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/Packaging/windows/icon.ico")
      set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE.md")
      set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
      set(CPACK_PACKAGE_EXECUTABLES "devilutionx" "DevilutionX")
      set(CPACK_NSIS_MUI_FINISHPAGE_RUN "${BIN_TARGET}")
    else()
      set(CPACK_PACKAGE_FILE_NAME "devilutionx")
      set(CPACK_GENERATOR "ZIP")
    endif()
    set(CPACK_STRIP_FILES TRUE)
    install(TARGETS ${BIN_TARGET} DESTINATION .)
    install(FILES "${PROJECT_SOURCE_DIR}/Packaging/windows/README.txt"
      DESTINATION "."
    )
    install(FILES "${DEVILUTIONX_MPQ}"
      DESTINATION "."
    )
    install(FILES "${HELLFIRE_MPQ}"
      DESTINATION "mods"
    )

    foreach(_SDL2_WIN32_DLL_PATH ${SDL2_WIN32_ALL_DLLS} ${WIN32_INSTALL_DLLS})
      install(FILES "${_SDL2_WIN32_DLL_PATH}"
        DESTINATION "."
      )
    endforeach()

    foreach(_SDL2_WIN32_LICENSE_PATH ${SDL2_WIN32_ALL_LICENSES})
      install(FILES "${_SDL2_WIN32_LICENSE_PATH}"
        DESTINATION "LICENSE"
      )
    endforeach()

    if(DISCORD_SHARED_LIB)
      install(FILES "${DISCORD_SHARED_LIB}"
        DESTINATION "."
      )
    endif()

    if(SCREEN_READER_INTEGRATION)
      # libTolk.dll with MinGW, Tolk.dll with MSVC.
      install(FILES "$<TARGET_FILE:Tolk>"
        DESTINATION "."
      )
    endif()

  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(TOLOWER ${PROJECT_NAME} project_name)
    set(CPACK_PACKAGE_NAME ${project_name})
    set(CPACK_GENERATOR "7Z")
    # Common *nix files
    set(CPACK_STRIP_FILES TRUE)
    install(TARGETS ${BIN_TARGET} DESTINATION bin)
    set(desktop_file "${PROJECT_SOURCE_DIR}/Packaging/nix/${project_name}.desktop")
    set(desktop_file_hellfire "${PROJECT_SOURCE_DIR}/Packaging/nix/${project_name}-hellfire.desktop")

    find_program(DFI desktop-file-install)
    if(DFI)
      execute_process(COMMAND ${DFI} --dir=${CMAKE_BINARY_DIR} ${desktop_file})
      set(desktop_file "${CMAKE_BINARY_DIR}/${project_name}.desktop")
      execute_process(COMMAND ${DFI} --dir=${CMAKE_BINARY_DIR} ${desktop_file_hellfire})
      set(desktop_file_hellfire "${CMAKE_BINARY_DIR}/${project_name}-hellfire.desktop")
    endif()

    install(FILES "${desktop_file}"
      DESTINATION "share/applications"
    )
    install(FILES "${desktop_file_hellfire}"
      DESTINATION "share/applications"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/Packaging/nix/README.txt"
      DESTINATION "share/diasurgical/${project_name}"
    )
    install(FILES "${DEVILUTIONX_MPQ}"
      DESTINATION "share/diasurgical/${project_name}"
    )
    install(FILES "${HELLFIRE_MPQ}"
      DESTINATION "share/diasurgical/${project_name}/mods"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/Packaging/resources/icon_flat.png"
      DESTINATION "share/icons/hicolor/512x512/apps"
      RENAME "${project_name}.png"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/Packaging/resources/hellfire.png"
      DESTINATION "share/icons/hicolor/512x512/apps"
      RENAME "${project_name}-hellfire.png"
    )
    install(FILES "${PROJECT_SOURCE_DIR}/Packaging/nix/devilutionx.metainfo.xml"
      DESTINATION "share/metainfo"
      RENAME "${project_name}.metainfo.xml"
    )
    if(DISCORD_SHARED_LIB)
      install(FILES "${DISCORD_SHARED_LIB}" DESTINATION "lib")
    endif()

    # -G DEB
    set(CPACK_PACKAGE_CONTACT "anders@jenbo.dk")
    if(USE_SDL1)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsdl1.2debian")
    elseif(USE_SDL3)
      # SDL3/SDL3_image are linked statically unless the system copies are used; SDL3 dlopens the
      # X11/Wayland/audio libraries it finds at runtime, so they are not hard dependencies.
      set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libstdc++6, libgcc-s1, libpng16-16t64 | libpng16-16, zlib1g")
      if(DEVILUTIONX_SYSTEM_SDL3)
        string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libsdl3-0")
      endif()
      if(DEVILUTIONX_SYSTEM_SDL_IMAGE)
        string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libsdl3-image0")
      endif()
      if(DEVILUTIONX_WEBGPU)
        # Dawn loads the Vulkan loader at runtime and needs a Vulkan driver (ICD) to present.
        string(APPEND CPACK_DEBIAN_PACKAGE_DEPENDS ", libvulkan1")
        set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "mesa-vulkan-drivers | vulkan-icd")
      endif()
    else()
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsdl2-2.0-0, libsdl2-image-2.0-0")
    endif()
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

    # -G RPM
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)

    find_program(RPMBUILD rpmbuild)
    if(RPMBUILD)
      list(APPEND CPACK_GENERATOR "RPM")
    endif()
    find_program(DPKG dpkg)
    if(DPKG)
      list(APPEND CPACK_GENERATOR "DEB")
    endif()

  elseif(APPLE)
    set(CPACK_PACKAGE_FILE_NAME "devilutionx")
    set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK "ON")
    set(CPACK_STRIP_FILES TRUE)
    set(CPACK_GENERATOR "DragNDrop")
  endif()

  set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
  set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
  set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
  set(CPACK_PACKAGE_VERSION "${DEVILUTIONX_RELEASE_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "${DEVILUTIONX_RELEASE_NAMESPACE}-${DEVILUTIONX_RELEASE_VERSION}-${DEVILUTIONX_RELEASE_COMMIT}")

  # Ship provenance without proprietary game data. Runtime libraries and their
  # notices are supplied by the platform/package manager or explicit install
  # rules above; this contract makes the release identity auditable.
  configure_file(
    "${PROJECT_SOURCE_DIR}/Packaging/release-manifest.ini.in"
    "${CMAKE_BINARY_DIR}/release-manifest.ini"
    @ONLY)
  install(FILES "${CMAKE_BINARY_DIR}/release-manifest.ini" DESTINATION ".")
  include(CPack)
endif()

# Contract-only fixtures never pull Rust into ordinary or legacy builds.
option(DEVILUTIONX_BUILD_DXMOD_CONTRACT_TESTS "Build native C/Rust ABI fixtures" OFF)
if(DEVILUTIONX_BUILD_DXMOD_CONTRACT_TESTS)
  if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "The dxmod-core byte fixtures require a native build")
  endif()
  enable_testing()
  add_subdirectory(test/dxmod_core)
endif()

resolve_target_link_dependencies()
if(UWP_LIB)
  get_target_property(_linked_objects libdevilutionx LINKED_OBJECTS)
  target_sources(libdevilutionx PRIVATE ${_linked_objects})
endif()
