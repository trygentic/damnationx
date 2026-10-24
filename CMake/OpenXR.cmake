# OpenXR for DEVILUTIONX_XR (docs/design/vr.md).
#
# Android (Quest): the Khronos loader AAR (org.khronos.openxr:openxr_loader_for_android, android-project/app/build.gradle
# with -Pvr) is consumed through Gradle's prefab support: find_package(OpenXR CONFIG) -> OpenXR::openxr_loader.
# Windows and macOS: the pinned OpenXR-SDK release is fetched and its loader built (or point DEVILUTIONX_OPENXR_SDK_DIR
# at an extracted OpenXR-SDK). On macOS the loader finds the active runtime (e.g. OXRSys) through its
# active_runtime.json or XR_RUNTIME_JSON. Linux has no OpenXR graphics bridge yet and builds the null backend.

set(DEVILUTIONX_OPENXR_SDK_VERSION "1.1.65" CACHE STRING "Pinned OpenXR-SDK release (desktop loader and headers)")
set(DEVILUTIONX_OPENXR_SDK_SHA256 "27aa797baf680c5f836b6138e78e59e2f72d6becfef6d4587ed9a57756d6d3b3" CACHE STRING "SHA256 of OpenXR-SDK release-<version>.tar.gz")
set(DEVILUTIONX_OPENXR_SDK_DIR "" CACHE PATH "Extracted OpenXR-SDK to use instead of downloading the pinned release")

function(devilutionx_link_openxr target)
  if(ANDROID)
    find_package(OpenXR CONFIG)
    if(NOT TARGET OpenXR::openxr_loader)
      message(FATAL_ERROR
        "DEVILUTIONX_XR on Android needs the OpenXR loader prefab package. Build through Gradle with -Pvr "
        "(android-project/app/build.gradle adds org.khronos.openxr:openxr_loader_for_android and enables prefab).")
    endif()
    target_link_libraries(${target} PUBLIC OpenXR::openxr_loader vulkan nativewindow android)
  elseif(WIN32 OR APPLE)
    if(DEVILUTIONX_OPENXR_SDK_DIR)
      set(_openxr_source "${DEVILUTIONX_OPENXR_SDK_DIR}")
    else()
      include(FetchContent)
      FetchContent_Declare(openxr_sdk
        URL "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-${DEVILUTIONX_OPENXR_SDK_VERSION}.tar.gz"
        URL_HASH "SHA256=${DEVILUTIONX_OPENXR_SDK_SHA256}")
      FetchContent_GetProperties(openxr_sdk)
      if(NOT openxr_sdk_POPULATED)
        FetchContent_Populate(openxr_sdk)
      endif()
      set(_openxr_source "${openxr_sdk_SOURCE_DIR}")
    endif()
    set(BUILD_TESTS OFF CACHE INTERNAL "")
    set(BUILD_API_LAYERS OFF CACHE INTERNAL "")
    set(DYNAMIC_LOADER OFF CACHE BOOL "OpenXR: static loader" FORCE)
    add_subdirectory("${_openxr_source}" "${CMAKE_BINARY_DIR}/_deps/openxr-build" EXCLUDE_FROM_ALL)
    target_link_libraries(${target} PUBLIC OpenXR::openxr_loader)
    if(WIN32)
      target_link_libraries(${target} PUBLIC d3d11 dxgi)
    else()
      target_link_libraries(${target} PUBLIC "-framework Metal" "-framework IOSurface" "-framework Foundation")
    endif()
  endif()
endfunction()
