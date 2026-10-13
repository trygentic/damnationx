if(DEVILUTIONX_STATIC_SDL3)
  set(BUILD_SHARED_LIBS OFF)
  set(SDL_SHARED OFF)
  set(SDL_STATIC ON)
  if(PIE)
    set(SDL_STATIC_PIC ON)
  endif()
else()
  set(BUILD_SHARED_LIBS ON)
  set(SDL_SHARED ON)
  set(SDL_STATIC OFF)
endif()
set(SDL_TEST_ENABLED_BY_DEFAULT OFF)
if(WIN32)
  # Newer Windows SDKs ship gameinput.lib, and SDL then imports GameInput.dll directly, so the exe
  # would not start where GameInput is not installed. Skip that check so SDL loads it at runtime.
  set(HAVE_GAMEINPUT_LIB OFF CACHE INTERNAL "")
endif()

include(functions/FetchContent_ExcludeFromAll_backport)
include(FetchContent)

FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/archive/e442a9a5e1d6eb2621e9206f5176c53b0d034e09.tar.gz
  URL_HASH SHA256=8ebc84ff04c076c0e6249c885fc1efc6dc5f13a411b31de8621478646ff76bcf
)
FetchContent_MakeAvailable_ExcludeFromAll(SDL3)
