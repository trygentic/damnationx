# Preserve an already-provided ZLIB target.  This is needed for Emscripten,
# where the zlib port is selected through link flags instead of a host library
# that CMake's stock finder can locate.
if(TARGET ZLIB::ZLIB)
  set(ZLIB_FOUND TRUE)
  set(ZLIB_LIBRARY ZLIB::ZLIB)
  set(ZLIB_LIBRARIES ZLIB::ZLIB)
  return()
endif()

include("${CMAKE_ROOT}/Modules/FindZLIB.cmake")
