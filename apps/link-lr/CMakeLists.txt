cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

set(PROJECT_VER_BASE "1.0.0")

#
# Version, fixed at CMake configure time. It is baked into esp_app_desc
# and is what OTA and get_version report, so it must not move between
# two builds of the same release.
#
#   LINK_VERSION set   the string is used as given (tools/release.ps1)
#   LINK_RELEASE set   PROJECT_VER_BASE, unsuffixed
#   neither            PROJECT_VER_BASE with -dev
#
if(DEFINED ENV{LINK_VERSION})
    set(PROJECT_VER "$ENV{LINK_VERSION}")
    set(LINK_IS_RELEASE 1)
elseif(DEFINED ENV{LINK_RELEASE})
    set(PROJECT_VER "${PROJECT_VER_BASE}")
    set(LINK_IS_RELEASE 1)
else()
    set(PROJECT_VER "${PROJECT_VER_BASE}-dev")
    set(LINK_IS_RELEASE 0)
endif()

#
# A separate build stamp, regenerated on every `idf.py build`, so a dev
# build can be told from the one before it without a fullclean - which
# PROJECT_VER above cannot do, being fixed at configure time.
#
# The timestamp has to be taken at build time, not configure time, so it
# goes through a generated script rather than STRING(TIMESTAMP) here.
#
set(BUILD_STAMP_HEADER "${CMAKE_CURRENT_LIST_DIR}/main/build_stamp.h")
set(BUILD_STAMP_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/gen_build_stamp.cmake")

file(WRITE "${BUILD_STAMP_SCRIPT}"
"string(TIMESTAMP STAMP \"%Y%m%d-%H%M\" UTC)\n"
"file(WRITE \"${BUILD_STAMP_HEADER}\"\n"
"\"/* Generated at every \`idf.py build\` - do not edit.\\n\"\n"
"\" * See the root CMakeLists.txt. */\\n\"\n"
"\"#pragma once\\n\"\n"
"\"#define LINK_BUILD_STAMP \\\"\${STAMP}\\\"\\n\"\n"
"\"#define LINK_IS_RELEASE  ${LINK_IS_RELEASE}\\n\"\n"
")\n"
)

# Once at configure time, so a build in a clean tree finds the header
# that main.c includes.
execute_process(COMMAND ${CMAKE_COMMAND} -P "${BUILD_STAMP_SCRIPT}")

add_custom_target(build_stamp ALL
    COMMAND ${CMAKE_COMMAND} -P "${BUILD_STAMP_SCRIPT}"
    COMMENT "Regenerating main/build_stamp.h"
    VERBATIM
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(link-lr)

# Before main.c compiles, not before the link: without this the stamp
# could be rewritten after the file that includes it was compiled.
if(TARGET __idf_main)
    add_dependencies(__idf_main build_stamp)
endif()

# The bring-up default is sized for four flat pages. These screens nest
# six levels deep, which LVGL 9 renders through a correspondingly deeper
# call chain - see the note in link_lvgl.c.
idf_component_get_property(ui_lib link-core-ui COMPONENT_LIB)
target_compile_definitions(${ui_lib} PRIVATE LVGL_TASK_STACK=10240)
