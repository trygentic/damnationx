# DAMNATIONX_HOST_LIB: the game as a static library for an embedding host (Source/host/host_api.hpp,
# docs/design/bevy-host.md).
#
# Outputs, next to the other build products:
#   libdamnationx_host.a (damnationx_host.lib)  every object of the game (libdevilutionx and its object libraries),
#                                               including Source/host/host_api.cpp. No window, renderer or audio
#                                               device is created by the host entry points.
#   damnationx_host_link.txt                    what else the final link needs, one item per line, in link order:
#                                               absolute paths of static/shared libraries (fmt, Lua, SDL_audiolib,
#                                               libsmackerdec, zlib, bzip2, SDL2, ...), linker flags (-lfoo,
#                                               -framework Foo) and plain library names. The first line is
#                                               libdamnationx_host.a itself.
#   assets/ (or devilutionx.mpq when smpq       the game's own assets; pass the folder with --host-assets-dir
#   is installed, not on Apple)                 (or put devilutionx.mpq next to the game data).
#
# A host links the archive and every line of the manifest (the C++ standard library comes from the host's own
# toolchain: libc++ on Apple, libstdc++ elsewhere). SDL stays a dependency of the core for surfaces, events and
# timers (phase 6 of the plan removes it); the host never needs to initialise its video or audio subsystems.

add_library(damnationx_host STATIC)
set_target_properties(damnationx_host PROPERTIES
  OUTPUT_NAME damnationx_host
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  POSITION_INDEPENDENT_CODE ON)
target_link_dependencies(damnationx_host PUBLIC libdevilutionx)

if(TARGET devilutionx_copied_assets)
  add_dependencies(damnationx_host devilutionx_copied_assets)
endif()

# Imported targets of system packages are local to the directory that found them; find them here as well so the
# manifest can name their files.
find_package(ZLIB QUIET)
find_package(BZip2 QUIET)
find_package(PNG QUIET)

# Collects the link closure of `target` (static/shared libraries, flags) into `out_var`, depth first, without the
# object libraries whose objects are already inside the archive.
function(_damnationx_host_collect target out_var visited_var)
  set(result ${${out_var}})
  set(visited ${${visited_var}})
  get_target_property(_aliased ${target} ALIASED_TARGET)
  if(_aliased)
    set(target ${_aliased})
  endif()
  if(target IN_LIST visited)
    return()
  endif()
  list(APPEND visited ${target})

  get_target_property(_type ${target} TYPE)
  set(_libs)
  if(NOT _type STREQUAL "INTERFACE_LIBRARY")
    get_target_property(_direct ${target} LINK_LIBRARIES)
    if(_direct)
      list(APPEND _libs ${_direct})
    endif()
  endif()
  get_target_property(_iface ${target} INTERFACE_LINK_LIBRARIES)
  if(_iface)
    list(APPEND _libs ${_iface})
  endif()

  foreach(_lib IN LISTS _libs)
    # Resolve build-only wrappers before target traversal. Otherwise an
    # interface target name becomes a nonexistent library after file(GENERATE).
    while(_lib MATCHES "^\\$<(LINK_ONLY|BUILD_INTERFACE):(.+)>$")
      set(_lib "${CMAKE_MATCH_2}")
    endwhile()
    if(_lib MATCHES "^\\$<INSTALL_INTERFACE:")
      continue()
    endif()
    if(_lib MATCHES "^::@")
      continue() # CMake's internal directory-scope markers
    endif()
    if(TARGET ${_lib})
      get_target_property(_aliased ${_lib} ALIASED_TARGET)
      if(_aliased)
        set(_lib ${_aliased})
      endif()
      get_target_property(_libtype ${_lib} TYPE)
      if(_libtype STREQUAL "STATIC_LIBRARY" OR _libtype STREQUAL "SHARED_LIBRARY" OR _libtype STREQUAL "UNKNOWN_LIBRARY"
          OR _libtype STREQUAL "MODULE_LIBRARY")
        get_target_property(_name ${_lib} NAME)
        if(NOT _name MATCHES "SDL2main")
          # SDL2main defines main(): the host has its own.
          list(APPEND result "$<TARGET_LINKER_FILE:${_lib}>")
        endif()
      endif()
      set(_sub_result ${result})
      _damnationx_host_collect(${_lib} _sub_result visited)
      set(result ${_sub_result})
    elseif(_lib MATCHES "^\\$<")
      # Other generator expressions (build-type or option switches) are evaluated by file(GENERATE).
      list(APPEND result "${_lib}")
    elseif(NOT _lib STREQUAL "")
      list(APPEND result "${_lib}")
    endif()
  endforeach()

  set(${out_var} ${result} PARENT_SCOPE)
  set(${visited_var} ${visited} PARENT_SCOPE)
endfunction()

function(damnationx_host_write_link_manifest)
  set(_items)
  set(_visited)
  _damnationx_host_collect(damnationx_host _items _visited)
  # Keep the last occurrence of each item: static libraries must follow the libraries that use them.
  list(REVERSE _items)
  list(REMOVE_DUPLICATES _items)
  list(REVERSE _items)
  list(REMOVE_ITEM _items "$<TARGET_LINKER_FILE:damnationx_host>")
  string(REPLACE ";" "\n" _content "$<TARGET_LINKER_FILE:damnationx_host>;${_items}")
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/damnationx_host_link.txt" CONTENT "${_content}\n" TARGET damnationx_host)
endfunction()
