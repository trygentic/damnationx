cmake_minimum_required(VERSION 3.22)

set(ROOT "${PROJECT_SOURCE_DIR}")
set(MANIFEST "${ROOT}/manifest.ini")
set(PROVENANCE "${ROOT}/assets/samples/PROVENANCE.md")
if(NOT EXISTS "${MANIFEST}" OR NOT EXISTS "${PROVENANCE}")
  message(FATAL_ERROR "DXM-A06 manifest or provenance is missing")
endif()
file(READ "${MANIFEST}" CONTENT)
file(READ "${ROOT}/assets.json" ASSETS)
foreach(required
  "[schema]"
  "id = dxm.a06.original.samples"
  "aliases = dxm-a06-original-samples"
  "index=assets.json")
  string(FIND "${CONTENT}" "${required}" FOUND)
  if(FOUND EQUAL -1)
    message(FATAL_ERROR "Missing manifest contract: ${required}")
  endif()
endforeach()
foreach(required
  "\"schema\": 1"
  "\"id\": \"dungeon.floor\""
  "\"id\": \"dungeon.lava\""
  "\"id\": \"monster.gloomling\""
  "\"id\": \"item.starshard\""
  "\"id\": \"ui.spellbook\"")
  string(FIND "${ASSETS}" "${required}" FOUND)
  if(FOUND EQUAL -1)
    message(FATAL_ERROR "Missing assets.json record: ${required}")
  endif()
endforeach()
if(NOT EXISTS "${ROOT}/assets/samples/partial/manifest.ini")
  message(FATAL_ERROR "DXM-A06 partial pack is missing")
endif()
file(READ "${PROVENANCE}" PROVENANCE_CONTENT)
foreach(required "original" "not extracted" "CC-BY-4.0" "proprietary")
  string(FIND "${PROVENANCE_CONTENT}" "${required}" FOUND)
  if(FOUND EQUAL -1)
    message(FATAL_ERROR "Missing provenance statement: ${required}")
  endif()
endforeach()
set(FILES
  assets/samples/dungeon/floor.png
  assets/samples/dungeon/wall.png
  assets/samples/dungeon/lava-0.png
  assets/samples/dungeon/lava-1.png
  assets/samples/dungeon/lava-2.png
  assets/samples/monster/gloomling-idle-0.png
  assets/samples/monster/gloomling-idle-1.png
  assets/samples/monster/gloomling-idle-2.png
  assets/samples/monster/gloomling-idle-3.png
  assets/samples/item/starshard.png
  assets/samples/ui/spellbook.png)
foreach(relative IN LISTS FILES)
  if(NOT EXISTS "${ROOT}/${relative}")
    message(FATAL_ERROR "Missing distributable sample: ${relative}")
  endif()
endforeach()
file(GLOB_RECURSE SAMPLE_FILES RELATIVE "${ROOT}/assets/samples" "${ROOT}/assets/samples/*")
foreach(relative IN LISTS SAMPLE_FILES)
  string(TOLOWER "${relative}" LOWER)
  if(LOWER MATCHES "(clx|cel|cl2|mpq|\.dun$|devilution|hellfire)")
    message(FATAL_ERROR "Proprietary or game-data-looking sample file: ${relative}")
  endif()
endforeach()
message(STATUS "DXM-A06 sample-pack manifest and provenance validation passed")
