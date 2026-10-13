# SDL3 is mandatory in the browser; native/legacy defaults are unchanged.
set(USE_SDL3 ON)
if(USE_SDL1)
  message(FATAL_ERROR "Emscripten requires SDL3, not SDL1")
endif()

# Validate user/toolchain flags before adding any source-built dependencies.
get_cmake_property(_browser_variables VARIABLES)
foreach(_variable IN LISTS _browser_variables)
  if(_variable MATCHES "^CMAKE_.*FLAGS")
    if("${${_variable}}" MATCHES "-pthread|-matomics|--shared-memory|USE_PTHREADS|PTHREAD_POOL|PROXY_TO_PTHREAD|SHARED_MEMORY|WASM_WORKERS|USE_SDL=2|USE_SDL_IMAGE=2")
      message(FATAL_ERROR "Single-threaded SDL3 browser profile conflicts with ${_variable}: ${${_variable}}")
    endif()
  endif()
endforeach()
# Use the existing main-thread loading path, not an inline worker followed by
# a progress loop that assumes concurrent execution.
add_compile_definitions(DEVILUTIONX_BROWSER_SINGLE_THREADED=1 LOAD_ON_MAIN_THREAD)
add_link_options(-sUSE_PTHREADS=0 -sSHARED_MEMORY=0)

set(BUILD_TESTING OFF)
set(BUILD_ASSETS_MPQ OFF)
set(DISABLE_ZERO_TIER ON)
set(DISABLE_TCP ON)
set(DEVILUTIONX_SYSTEM_SDL_AUDIOLIB OFF)
set(DEVILUTIONX_SYSTEM_LIBSODIUM OFF)
set(NOEXIT ON)
# Emscripten ports do have a bzip2 but it fails to link with this error:
#   warning: _BZ2_bzDecompress may need to be added to EXPORTED_FUNCTIONS if it arrives from a system library
#   error: undefined symbol: BZ2_bzDecompressEnd (referenced by top-level compiled C/C++ code)
set(DEVILUTIONX_SYSTEM_BZIP2 OFF)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Packaging/emscripten/index.html" "${CMAKE_CURRENT_BINARY_DIR}/index.html" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Packaging/emscripten/file-manager.js" "${CMAKE_CURRENT_BINARY_DIR}/file-manager.js" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Packaging/emscripten/d2sound-ui.js" "${CMAKE_CURRENT_BINARY_DIR}/d2sound-ui.js" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Packaging/emscripten/hd-folder.js" "${CMAKE_CURRENT_BINARY_DIR}/hd-folder.js" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Packaging/emscripten/privacy.html" "${CMAKE_CURRENT_BINARY_DIR}/privacy.html" COPYONLY)
