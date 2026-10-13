# cgltf v1.14 (https://github.com/jkuhlmann/cgltf, MIT, see LICENSE): single-header glTF 2.0 parser, vendored.
add_library(cgltf STATIC cgltf.c)
target_include_directories(cgltf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(cgltf PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
