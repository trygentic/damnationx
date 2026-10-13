# Native (desktop) WebGPU client: Dawn plus the HD art decoders (docs/building.md, "Native WebGPU (Dawn)").
#
# The browser build gets WebGPU from the pinned Emdawnwebgpu port; the desktop build links Dawn itself at
# DEVILUTIONX_DAWN_REVISION. Either point DEVILUTIONX_DAWN_DIR at an installed Dawn (a prefix holding
# lib/cmake/Dawn), or leave it empty and the pinned revision is fetched, built (Metal on macOS,
# D3D12 on Windows, Vulkan on Linux/Android; no
# samples, tests or Tint tools) and installed under DEVILUTIONX_DAWN_BUILD_ROOT at configure time, once.

if(NOT USE_SDL3 OR USE_SDL1)
  message(FATAL_ERROR "The native WebGPU client requires USE_SDL3=ON (the shared presenter uses SDL3 surfaces)")
endif()

set(DEVILUTIONX_DAWN_DIR "" CACHE PATH "Installed Dawn prefix (contains lib/cmake/Dawn); empty builds the pinned DEVILUTIONX_DAWN_REVISION")
set(DEVILUTIONX_DAWN_JOBS "" CACHE STRING "Parallel jobs for the one-time Dawn build (empty: CMAKE_BUILD_PARALLEL_LEVEL, else all cores)")
set(DEVILUTIONX_DAWN_BUILD_ROOT "${CMAKE_BINARY_DIR}/dawn" CACHE PATH "Where the pinned Dawn is fetched, built and installed when DEVILUTIONX_DAWN_DIR is empty (several GB with its dependencies; may be shared between build directories)")

if(NOT DEVILUTIONX_DAWN_DIR)
  string(TOUPPER "${DEVILUTIONX_DAWN_LINKAGE}" _dawn_monolithic)
  set(_dawn_src "${DEVILUTIONX_DAWN_BUILD_ROOT}/src")
  set(_dawn_target "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
  if(ANDROID)
    string(APPEND _dawn_target "-${ANDROID_ABI}")
  endif()
  if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded")
    # A static-CRT Dawn cannot be mixed with a /MD one; keep them in separate build and install trees.
    string(APPEND _dawn_target "-mt")
  endif()
  set(_dawn_build "${DEVILUTIONX_DAWN_BUILD_ROOT}/build-${DEVILUTIONX_DAWN_REVISION}-${_dawn_target}-${DEVILUTIONX_DAWN_LINKAGE}")
  set(_dawn_prefix "${DEVILUTIONX_DAWN_BUILD_ROOT}/install-${DEVILUTIONX_DAWN_REVISION}-${_dawn_target}-${DEVILUTIONX_DAWN_LINKAGE}")
  set(_dawn_stamp "${_dawn_prefix}/devilutionx-dawn.stamp")
  if(NOT EXISTS "${_dawn_stamp}")
    find_package(Git REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    message(STATUS "Dawn: building ${DEVILUTIONX_DAWN_REVISION} (${DEVILUTIONX_DAWN_LINKAGE}) in ${DEVILUTIONX_DAWN_BUILD_ROOT}; this takes a few minutes once")
    if(NOT EXISTS "${_dawn_src}/.git")
      file(MAKE_DIRECTORY "${_dawn_src}")
      execute_process(COMMAND "${GIT_EXECUTABLE}" init -q WORKING_DIRECTORY "${_dawn_src}" COMMAND_ERROR_IS_FATAL ANY)
      execute_process(COMMAND "${GIT_EXECUTABLE}" remote add origin https://github.com/google/dawn.git WORKING_DIRECTORY "${_dawn_src}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${_dawn_src}"
      OUTPUT_VARIABLE _dawn_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _dawn_head STREQUAL DEVILUTIONX_DAWN_REVISION)
      execute_process(COMMAND "${GIT_EXECUTABLE}" fetch --depth 1 origin "${DEVILUTIONX_DAWN_REVISION}" WORKING_DIRECTORY "${_dawn_src}" COMMAND_ERROR_IS_FATAL ANY)
      execute_process(COMMAND "${GIT_EXECUTABLE}" checkout -q --force FETCH_HEAD WORKING_DIRECTORY "${_dawn_src}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    set(_dawn_args
      -DCMAKE_BUILD_TYPE=Release
      "-DCMAKE_INSTALL_PREFIX=${_dawn_prefix}"
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DPython3_EXECUTABLE=${Python3_EXECUTABLE}"
      -DDAWN_FETCH_DEPENDENCIES=ON
      -DDAWN_ENABLE_INSTALL=ON
      "-DDAWN_BUILD_MONOLITHIC_LIBRARY=${_dawn_monolithic}"
      -DDAWN_BUILD_SAMPLES=OFF
      -DDAWN_BUILD_TESTS=OFF
      -DDAWN_USE_GLFW=OFF
      -DDAWN_ENABLE_NULL=OFF
      -DDAWN_ENABLE_DESKTOP_GL=OFF
      -DDAWN_ENABLE_OPENGLES=OFF
      -DTINT_BUILD_TESTS=OFF
      -DTINT_BUILD_CMD_TOOLS=OFF
    )
    if(APPLE)
      # Metal only.
      list(APPEND _dawn_args -DDAWN_ENABLE_VULKAN=OFF)
    elseif(ANDROID)
      # Dawn must use the same NDK toolchain and ABI as the game.
      list(APPEND _dawn_args
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DANDROID_ABI=${ANDROID_ABI}"
        "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
        -DDAWN_ENABLE_OPENGLES=OFF)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # SDL3 can choose X11 or Wayland at runtime.
      list(APPEND _dawn_args -DDAWN_USE_X11=ON -DDAWN_USE_WAYLAND=ON -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF)
    endif()
    if(MSVC AND CMAKE_MSVC_RUNTIME_LIBRARY)
      # Dawn and its fetched dependencies must use the game's CRT (/MT for a self-contained exe).
      list(APPEND _dawn_args "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}" -DCMAKE_POLICY_DEFAULT_CMP0091=NEW)
      if(NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
        # Abseil picks its CRT from this option rather than CMAKE_MSVC_RUNTIME_LIBRARY.
        list(APPEND _dawn_args -DABSL_MSVC_STATIC_RUNTIME=ON)
      endif()
    endif()
    if(CMAKE_CROSSCOMPILING AND CMAKE_TOOLCHAIN_FILE AND NOT ANDROID)
      # Generic cross-compile (e.g. x86_64-linux-gnu from an arm64 host): build Dawn with the same toolchain
      # so its sysroot, find-root modes and pkg-config wrapper match the game. Dawn's code generators are
      # Python scripts and it builds no other host tools with TINT_BUILD_CMD_TOOLS/tests/samples off.
      get_filename_component(_dawn_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
      list(APPEND _dawn_args "-DCMAKE_TOOLCHAIN_FILE=${_dawn_toolchain}")
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      list(APPEND _dawn_args "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
      list(APPEND _dawn_args "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
    endif()
    # Dawn's build is memory-hungry; cap it with DEVILUTIONX_DAWN_JOBS or CMAKE_BUILD_PARALLEL_LEVEL.
    if(DEVILUTIONX_DAWN_JOBS)
      set(_dawn_jobs ${DEVILUTIONX_DAWN_JOBS})
    elseif(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL} AND NOT "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" STREQUAL "")
      set(_dawn_jobs $ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    else()
      include(ProcessorCount)
      ProcessorCount(_dawn_jobs)
      if(_dawn_jobs EQUAL 0)
        set(_dawn_jobs 4)
      endif()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_dawn_src}" -B "${_dawn_build}" -G "${CMAKE_GENERATOR}" ${_dawn_args} COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_dawn_build}" --target webgpu_dawn --config Release --parallel ${_dawn_jobs} COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${_dawn_build}" --config Release COMMAND_ERROR_IS_FATAL ANY)
    file(WRITE "${_dawn_stamp}" "${DEVILUTIONX_DAWN_REVISION}\n")
  endif()
  set(_dawn_search "${_dawn_prefix}")
else()
  set(_dawn_search "${DEVILUTIONX_DAWN_DIR}")
endif()

# NO_CMAKE_FIND_ROOT_PATH: cross toolchains (the Android NDK) set CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY,
# which would re-root this host-side prefix under the sysroot and never find it.
find_package(Dawn CONFIG REQUIRED PATHS "${_dawn_search}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
message(STATUS "Dawn: ${Dawn_DIR}")

if(WIN32 AND MSVC)
  # Dawn's D3D12 backend loads the FXC compiler (d3dcompiler_47.dll) at runtime from the exe's folder and
  # fails without it (it does not fall back to System32). Ship the Windows SDK's redistributable copy next to
  # the exe. DXC (dxcompiler.dll + dxil.dll, ~15 MB) is only used with Dawn's use_dxc toggle, which is off.
  set(_d3d_redist_hints)
  if(DEFINED ENV{WindowsSdkDir})
    list(APPEND _d3d_redist_hints "$ENV{WindowsSdkDir}/Redist/D3D/x64")
  endif()
  list(APPEND _d3d_redist_hints "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64")
  find_path(DEVILUTIONX_D3D_REDIST_DIR d3dcompiler_47.dll PATHS ${_d3d_redist_hints} NO_DEFAULT_PATH
    DOC "Folder with the redistributable d3dcompiler_47.dll to ship with the WebGPU build")
  if(NOT DEVILUTIONX_D3D_REDIST_DIR OR NOT EXISTS "${DEVILUTIONX_D3D_REDIST_DIR}/d3dcompiler_47.dll")
    message(FATAL_ERROR "d3dcompiler_47.dll not found; set DEVILUTIONX_D3D_REDIST_DIR to the Windows SDK's Redist/D3D/x64 folder")
  endif()
  # Next to the exe for runs from the build folder (and picked up by the MSVC CPack DLL glob).
  file(COPY "${DEVILUTIONX_D3D_REDIST_DIR}/d3dcompiler_47.dll" DESTINATION "${CMAKE_BINARY_DIR}")
  message(STATUS "Dawn: shipping ${DEVILUTIONX_D3D_REDIST_DIR}/d3dcompiler_47.dll")
endif()

# HD art decoders: libpng (already a dependency of SDL_image) and the libwebp decoder (tile atlases are WebP).
if(NOT TARGET PNG::PNG)
  find_package(PNG REQUIRED)
endif()
add_subdirectory(3rdParty/libwebp)
