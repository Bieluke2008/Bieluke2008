# Desktop (Linux/macOS) build of the port, for development and testing.
# The Switch build is the Makefile next to this file.
cmake_minimum_required(VERSION 3.16)
project(openfront_switch C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(QJS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/quickjs)
add_library(quickjs STATIC
  ${QJS_DIR}/dtoa.c
  ${QJS_DIR}/libregexp.c
  ${QJS_DIR}/libunicode.c
  ${QJS_DIR}/quickjs.c)
target_include_directories(quickjs PUBLIC ${QJS_DIR})
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
target_compile_options(quickjs PRIVATE -w)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2 SDL2_ttf SDL2_image SDL2_mixer)
find_package(ZLIB REQUIRED)

add_executable(openfront
  src/main.c src/engine.c src/input.c src/gfx.c src/ui.c src/hud.c src/map.c
  src/radial.c src/menus.c src/solo.c src/native.c src/clocks.c src/sound.c src/settings.c src/audio_panel.c deps/cjson/cJSON.c)
# DetMath's exp/log in native.c must round like the TS: no fused multiply-add.
set_source_files_properties(src/native.c PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
target_include_directories(openfront PRIVATE src deps/cjson)
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION OPENFRONT_VERSION LIMIT_COUNT 1)
target_compile_definitions(openfront PRIVATE OPENFRONT_VERSION="${OPENFRONT_VERSION}")
target_compile_options(openfront PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(openfront PRIVATE quickjs PkgConfig::SDL2 ZLIB::ZLIB m pthread)
