# PNG::PNG is built from source (3rdParty/libpng); report it as found to find_package(PNG) callers.
if(TARGET PNG::PNG)
  set(PNG_FOUND TRUE)
  set(PNG_LIBRARY PNG::PNG)
  set(PNG_LIBRARIES PNG::PNG)
endif()
