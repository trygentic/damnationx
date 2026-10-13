# Also usable independently to avoid game/assets dependencies for wire checks.
cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dxmod_contract LANGUAGES C)
  enable_testing()
endif()
find_program(DXMOD_RUSTC rustc REQUIRED)
add_executable(dxmod_contract_c fixture.c)
set_property(TARGET dxmod_contract_c PROPERTY C_STANDARD 99)
set(rust_fixture "${CMAKE_CURRENT_BINARY_DIR}/dxmod_contract_rust${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(OUTPUT "${rust_fixture}"
  COMMAND "${DXMOD_RUSTC}" --edition=2021 "${CMAKE_CURRENT_SOURCE_DIR}/fixture.rs" -o "${rust_fixture}"
  DEPENDS fixture.rs VERBATIM)
add_custom_target(dxmod_contract_rust_build ALL DEPENDS "${rust_fixture}")
add_test(NAME dxmod_contract_bytes COMMAND "${CMAKE_COMMAND}"
  "-DC_FIXTURE=$<TARGET_FILE:dxmod_contract_c>" "-DRUST_FIXTURE=${rust_fixture}"
  "-DGOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/golden.txt" -P "${CMAKE_CURRENT_SOURCE_DIR}/compare.cmake")
