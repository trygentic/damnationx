include(functions/FetchContent_ExcludeFromAll_backport)
include(FetchContent)

# Decoder only: the native WebGPU client reads the HD tile atlases (hd/tiles-*.webp).
set(WEBP_BUILD_ANIM_UTILS OFF)
set(WEBP_BUILD_CWEBP OFF)
set(WEBP_BUILD_DWEBP OFF)
set(WEBP_BUILD_GIF2WEBP OFF)
set(WEBP_BUILD_IMG2WEBP OFF)
set(WEBP_BUILD_VWEBP OFF)
set(WEBP_BUILD_WEBPINFO OFF)
set(WEBP_BUILD_LIBWEBPMUX OFF)
set(WEBP_BUILD_WEBPMUX OFF)
set(WEBP_BUILD_EXTRAS OFF)
set(BUILD_SHARED_LIBS OFF)

FetchContent_Declare_ExcludeFromAll(libwebp
  URL https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
  URL_HASH SHA256=93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136
)
FetchContent_MakeAvailable_ExcludeFromAll(libwebp)

# libwebp only exports its source root (its own headers use "src/webp/..."); expose src/ so <webp/decode.h>
# resolves to this copy instead of depending on a system libwebp being on the include path.
target_include_directories(webpdecoder INTERFACE $<BUILD_INTERFACE:${libwebp_SOURCE_DIR}/src>)
add_library(WebP::webpdecoder ALIAS webpdecoder)
