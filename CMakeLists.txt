# ------------------------------------------------------------------------------
# ORB "The Glowing Orb" - A C11 Game Engine
# ------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.12)
project(orb LANGUAGES C)

# ------------------------------------------------------------------------------
# BUILD MODE TOGGLE
# ------------------------------------------------------------------------------

# This determines if modules like 'render' are built as DLLs or static libs
option(ENGINE_MONOLITHIC "Build as a single static executable" OFF)

if(ENGINE_MONOLITHIC)
    # This makes -DMODULE_LINK_STATIC visible to EVERY target in the project
    add_compile_definitions(BUILD_STATIC)
    message(STATUS "Global Flag: MODULE_LINK_STATIC is active.")
endif()

if(ENGINE_MONOLITHIC)
    message(STATUS "Build Mode: Monolithic (Statically Linked)")
else()
    message(STATUS "Build Mode: Dynamic (Hot-Reload Enabled)")
endif()

if(ENGINE_MONOLITHIC)
    set(MODULE_TYPE STATIC)
else()
    set(MODULE_TYPE SHARED)
endif()

# ------------------------------------------------------------------------------
# LANGUAGE STANDARD
# ------------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ------------------------------------------------------------------------------
# Visual Studio Solution Folders
# ------------------------------------------------------------------------------

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ------------------------------------------------------------------------------
# Output Directories
# ------------------------------------------------------------------------------

# CMAKE_BINARY_DIR resolves to 'build_dynamic' or 'build_monolithic' based on your .bat
set(TARGET_BIN_DIR ${CMAKE_BINARY_DIR}/bin)
set(TARGET_LIB_DIR ${CMAKE_BINARY_DIR}/lib)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${TARGET_BIN_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${TARGET_BIN_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${TARGET_LIB_DIR})

# Still remove the nastyy Release/Debug subfolders so hot-reload file watching works
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${TARGET_BIN_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${TARGET_BIN_DIR})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${TARGET_LIB_DIR})
endforeach()

# PDB files (debug symbols) should also go to the bin directory for easy cleanup and hot-reload watching.
set(CMAKE_PDB_OUTPUT_DIRECTORY ${TARGET_BIN_DIR})
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_PDB_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${TARGET_BIN_DIR})
endforeach()

# ------------------------------------------------------------------------------
# Build Type (for single-config generators)
# ------------------------------------------------------------------------------

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build" FORCE)
endif()

# ------------------------------------------------------------------------------
# Compiler-specific settings
# ------------------------------------------------------------------------------

if (MSVC)
    # Note: This block naturally covers BOTH Native MSVC (cl.exe) and Clang-CL.
    message(STATUS "Compiler: MSVC-compatible (${CMAKE_C_COMPILER_ID})")
    
    # /W4: High warnings
    # /WX: Warnings as errors
    # /Zc:preprocessor: Strict C11 standard preprocessor (Critical for modern macros)
    add_compile_options(/W4 /WX /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    
    # Release-Only Suppressions:
    # Often needed when assert() macros compile out, leaving unused variables.
    # C4101: unreferenced local variable
    # C4189: initialized but not referenced
    # C4100: unreferenced formal parameter
    add_compile_options("$<$<CONFIG:Release>:/wd4101;/wd4189;/wd4100>")

    # Force Debug Info (PDBs) in standard Release builds
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")

else()
    
    # Catch-all for Pure Clang (Unix/macOS) and GCC
    message(STATUS "Compiler: Unix-style (${CMAKE_C_COMPILER_ID})")
    
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-function)    
    # Release-Only Suppressions and Debug Info
    add_compile_options("$<$<CONFIG:Release>:-Wno-unused-variable;-Wno-unused-parameter;-g>")

endif()

# ------------------------------------------------------------------------------
# Debug Info for Release Builds
# ------------------------------------------------------------------------------

if (MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")

    # 1. Tell the Compiler to generate debug information
    # /Zi: Produces a PDB file. /Z7: Embeds debug info in .obj (alternative)
    add_compile_options("$<$<CONFIG:Release>:/Zi>")

    # 2. Tell the Linker to create the PDB file
    # /DEBUG: Generates the actual .pdb
    # /OPT:REF and /OPT:ICF: Ensure optimizations (stripping unused code) stay ON
    add_link_options("$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")    
    message(STATUS "Release configuration: PDB generation enabled.")
	
else()

    # For Pure Clang or GCC
    add_compile_options("$<$<CONFIG:Release>:-g>")
	
endif()

# ------------------------------------------------------------------------------
# Global Include Directories
# ------------------------------------------------------------------------------

# This allows `#include "orb.h"` and `#include "engine/core/core.h"` 
# to work from anywhere in the codebase.

include_directories( ${CMAKE_SOURCE_DIR}/source )

# ------------------------------------------------------------------------------
# CMAKE HELPER FUNCTIONS
# ------------------------------------------------------------------------------

# add_unity_library( TARGET_NAME TARGET_TYPE SOURCE_DIR VS_FOLDER [unity.c ...] )
#
#   Creates a library with a unity build pattern. All .c/.h files in SOURCE_DIR
#   are collected for IDE visibility; only the listed unity files are compiled.

function(add_unity_library TARGET_NAME TARGET_TYPE SOURCE_DIR VS_FOLDER)

    set(MODULE_ROOT "${CMAKE_SOURCE_DIR}/${SOURCE_DIR}")

    file(GLOB_RECURSE TARGET_FILES
        "${MODULE_ROOT}/*.c"
        "${MODULE_ROOT}/*.h"
        "${MODULE_ROOT}/*.natvis"
    )

    set(TARGET_COMPILED "")
    foreach(SRC_FILE IN LISTS ARGN)
        list(APPEND TARGET_COMPILED "${MODULE_ROOT}/${SRC_FILE}")
    endforeach()

    set(TARGET_HEADERS ${TARGET_FILES})
    if(TARGET_COMPILED)
        list(REMOVE_ITEM TARGET_HEADERS ${TARGET_COMPILED})
    endif()

    set_source_files_properties(${TARGET_HEADERS} PROPERTIES HEADER_FILE_ONLY ON)
    set_source_files_properties(${TARGET_COMPILED} PROPERTIES HEADER_FILE_ONLY OFF)

    add_library(${TARGET_NAME} ${TARGET_TYPE}
        ${TARGET_HEADERS}
        ${TARGET_COMPILED}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${VS_FOLDER})
    source_group(TREE "${MODULE_ROOT}" FILES ${TARGET_FILES})
    source_group("CMake Rules" FILES "CMakeLists.txt")

endfunction()

# add_unity_executable( TARGET_NAME SOURCE_DIR VS_FOLDER [main.c ...] )
#
#   Same as add_unity_library but produces an executable.

function(add_unity_executable TARGET_NAME SOURCE_DIR VS_FOLDER)

    set(MODULE_ROOT "${CMAKE_SOURCE_DIR}/${SOURCE_DIR}")

    file(GLOB_RECURSE TARGET_FILES
        "${MODULE_ROOT}/*.c"
        "${MODULE_ROOT}/*.h"
        "${MODULE_ROOT}/*.natvis"
    )

    set(TARGET_COMPILED "")
    foreach(SRC_FILE IN LISTS ARGN)
        list(APPEND TARGET_COMPILED "${MODULE_ROOT}/${SRC_FILE}")
    endforeach()

    set(TARGET_HEADERS ${TARGET_FILES})
    if(TARGET_COMPILED)
        list(REMOVE_ITEM TARGET_HEADERS ${TARGET_COMPILED})
    endif()

    set_source_files_properties(${TARGET_HEADERS} PROPERTIES HEADER_FILE_ONLY ON)
    set_source_files_properties(${TARGET_COMPILED} PROPERTIES HEADER_FILE_ONLY OFF)

    add_executable(${TARGET_NAME}
        ${TARGET_HEADERS}
        ${TARGET_COMPILED}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${VS_FOLDER})
    source_group(TREE "${MODULE_ROOT}" FILES ${TARGET_FILES})
    source_group("CMake Rules" FILES "CMakeLists.txt")

endfunction()

# target_unlock_pdb_on_rebuild( TARGET_NAME )
#
#   Adds a PRE_LINK step that deletes the PDB so MSVC can rewrite it during a
#   hot-reload rebuild while the debugger holds the previous file open.
#   No-op in monolithic builds and on non-MSVC compilers.

function(target_unlock_pdb_on_rebuild TARGET_NAME)
    if(MSVC AND NOT ENGINE_MONOLITHIC)
        add_custom_command(TARGET ${TARGET_NAME} PRE_LINK
            COMMAND ${CMAKE_COMMAND}
                -D "PDB_PATH=$<TARGET_FILE_DIR:${TARGET_NAME}>/${TARGET_NAME}.pdb"
                -P "${CMAKE_SOURCE_DIR}/cmake/unlock_pdb.cmake"
            COMMENT "[${TARGET_NAME}] unlocking PDB for hot-reload rebuild"
            VERBATIM
        )
    endif()
endfunction()

# add_module( TARGET_NAME SOURCE_DIR VS_FOLDER [unity.c ...] )
#
#   Wrapper for hot-reloadable modules. Calls add_unity_library with MODULE_TYPE,
#   injects <NAME>_STATIC so the module can call its own gateway safely in dynamic
#   builds, and registers the PDB unlock pre-link step.

function(add_module TARGET_NAME SOURCE_DIR VS_FOLDER)
    add_unity_library( ${TARGET_NAME} ${MODULE_TYPE} ${SOURCE_DIR} ${VS_FOLDER} ${ARGN} )

    string(TOUPPER "${TARGET_NAME}" UPPER)
    target_compile_definitions( ${TARGET_NAME} PRIVATE ${UPPER}_STATIC )

    target_unlock_pdb_on_rebuild( ${TARGET_NAME} )
endfunction()

# target_add_reflect_gen( TARGET_NAME SOURCE_DIR MODULE_NAME )
#
#   Wires build_reflect as a pre-compile step for TARGET_NAME. Generated files
#   land in ${CMAKE_BINARY_DIR}/generated/ and are added as sources so the IDE
#   shows them and MSBuild rebuilds when headers change.

function( target_add_reflect_gen TARGET_NAME SOURCE_DIR MODULE_NAME )

    set( GEN_DIR  "${CMAKE_BINARY_DIR}/generated" )
    set( GEN_H    "${GEN_DIR}/${MODULE_NAME}.generated.h" )
    set( GEN_C    "${GEN_DIR}/${MODULE_NAME}.generated.c" )
  # set( GEN_INC  "${GEN_DIR}/${MODULE_NAME}.gen_desc.inc" )

    file( MAKE_DIRECTORY "${GEN_DIR}" )
    file( GLOB_RECURSE RS_HEADERS "${CMAKE_SOURCE_DIR}/${SOURCE_DIR}/*.h" )
    file( GLOB_RECURSE RS_SOURCES "${CMAKE_SOURCE_DIR}/${SOURCE_DIR}/*.c" )

    add_custom_command(
        OUTPUT  "${GEN_H}" "${GEN_C}" "${GEN_INC}"
        COMMAND $<TARGET_FILE:build_reflect>
                "${CMAKE_SOURCE_DIR}/${SOURCE_DIR}"
                "${GEN_DIR}"
                "${MODULE_NAME}"
        DEPENDS build_reflect ${RS_HEADERS} ${RS_SOURCES}
        COMMENT "[build_reflect] generating reflection for ${MODULE_NAME}"
        VERBATIM
    )

    target_sources( ${TARGET_NAME} PRIVATE "${GEN_H}" "${GEN_C}" "${GEN_INC}" )
    # PUBLIC so that consumers of this target can also find {module}.generated.h
    target_include_directories( ${TARGET_NAME} PUBLIC "${GEN_DIR}" )

    set_source_files_properties( "${GEN_H}"   PROPERTIES HEADER_FILE_ONLY ON )
    set_source_files_properties( "${GEN_C}"   PROPERTIES HEADER_FILE_ONLY OFF )
    set_source_files_properties( "${GEN_INC}" PROPERTIES HEADER_FILE_ONLY ON )

    source_group( "Generated" FILES "${GEN_H}" "${GEN_C}" "${GEN_INC}" )

endfunction()

# ------------------------------------------------------------------------------
# 10 TOOLS
#
# Declared before all other targets: build_reflect is a pre-build dependency
# for any target that calls target_add_reflect_gen.
# ------------------------------------------------------------------------------

set( BUILD_REFLECT_ROOT  "${CMAKE_SOURCE_DIR}/source/tools/rs_gen" )
set( BUILD_REFLECT_UNITY "${BUILD_REFLECT_ROOT}/rs_gen.c" )

file( GLOB_RECURSE BUILD_REFLECT_FILES
    "${BUILD_REFLECT_ROOT}/*.c"
    "${BUILD_REFLECT_ROOT}/*.h"
)

set_source_files_properties( ${BUILD_REFLECT_FILES}   PROPERTIES HEADER_FILE_ONLY ON  )
set_source_files_properties( "${BUILD_REFLECT_UNITY}" PROPERTIES HEADER_FILE_ONLY OFF )

add_executable( build_reflect ${BUILD_REFLECT_FILES} )
set_target_properties( build_reflect PROPERTIES FOLDER "10_Tools" )
source_group( TREE "${BUILD_REFLECT_ROOT}" FILES ${BUILD_REFLECT_FILES} )

# ------------------------------------------------------------------------------
# 00 ROOT
# ------------------------------------------------------------------------------

set( ROOT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/source/orb.h"
)
set( DOC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/project.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/TODO.txt"
)

add_custom_target( root SOURCES ${ROOT_SOURCES} ${DOC_SOURCES} )
set_target_properties( root PROPERTIES FOLDER "00_Root" )

source_group( "Source" "${CMAKE_CURRENT_SOURCE_DIR}/source" FILES ${ROOT_SOURCES} )
source_group( "Docs"   "${CMAKE_CURRENT_SOURCE_DIR}/docs"   FILES ${DOC_SOURCES}  )

# ------------------------------------------------------------------------------
# 01 BASE
# ------------------------------------------------------------------------------

add_unity_library( base STATIC "source/base" "01_Base" "base.c" )

# ------------------------------------------------------------------------------
# 02 ENGINE
# ------------------------------------------------------------------------------

add_unity_library( engine_sys  STATIC "source/engine/sys"  "02_Engine" "sys.c"  )
add_unity_library( engine_mod  STATIC "source/engine/mod"  "02_Engine" "mod.c"  )
add_unity_library( engine_app  STATIC "source/engine/app"  "02_Engine" "app.c"  )
add_unity_library( engine_core STATIC "source/engine/core" "02_Engine" "core.c" )
add_unity_library( engine_rs   STATIC "source/engine/rs"   "02_Engine" "rs.c"   )

# engine_

target_compile_definitions( engine_sys  PUBLIC SYS_STATIC  )
target_compile_definitions( engine_mod  PUBLIC MOD_STATIC  )
target_compile_definitions( engine_app  PUBLIC APP_STATIC  )
target_compile_definitions( engine_core PUBLIC CORE_STATIC )
target_compile_definitions( engine_rs   PUBLIC RS_STATIC   )

# target_link_libraries( engine_sys PUBLIC base )
target_link_libraries( engine_mod  PUBLIC engine_sys )
target_link_libraries( engine_app  PUBLIC engine_sys )
target_link_libraries( engine_core PUBLIC engine_sys engine_rs )

target_add_reflect_gen( engine_core "source/engine/core" "engine_core" )

# ------------------------------------------------------------------------------
# 03 RUNTIME
# ------------------------------------------------------------------------------

add_unity_library( runtime_host STATIC "source/runtime" "03_Runtime" "runtime.c" )
target_compile_definitions( runtime_host PUBLIC RUN_STATIC )
target_link_libraries( runtime_host PUBLIC engine_mod engine_sys engine_core engine_app )

# ------------------------------------------------------------------------------
# 03 RUNTIME SERVICE
# ------------------------------------------------------------------------------

# RHI — Vulkan backend, static. Windowed hosts link this directly; DLL modules
# access it through the registry via MOD_FETCH_API. Headless hosts omit it.
add_unity_library( rhi  STATIC "source/runtime_service/rhi"  "03_Runtime_Service" "rhi.c"  )
add_unity_library( jobs STATIC "source/runtime_service/jobs" "03_Runtime_Service" "jobs.c" )

target_compile_definitions( rhi PUBLIC RHI_STATIC )
target_link_libraries( rhi PUBLIC engine_app )

# jobs: placeholder, not yet implemented

# ------------------------------------------------------------------------------
# 03 RUNTIME MODULES
# ------------------------------------------------------------------------------

add_module( example         "source/runtime_modules/example"         "03_Runtime_Modules" "example.c"         )
add_module( render          "source/runtime_modules/render"          "03_Runtime_Modules" "render.c"          )
add_module( audio           "source/runtime_modules/audio"           "03_Runtime_Modules" "audio.c"           )
add_module( physics         "source/runtime_modules/physics"         "03_Runtime_Modules" "physics.c"         )
add_module( example_reflect "source/runtime_modules/example_reflect" "03_Runtime_Modules" "example_reflect.c" )
add_module( example_gen     "source/runtime_modules/example_gen"     "03_Runtime_Modules" "example_gen.c" )

target_add_reflect_gen( example_reflect "source/runtime_modules/example_reflect" "example_reflect" )
target_add_reflect_gen( example_gen     "source/runtime_modules/example_gen"     "example_gen" )

# target_link_libraries( example PUBLIC base )
# target_link_libraries( render  PUBLIC base )
# target_link_libraries( audio   PUBLIC base )
# target_link_libraries( physics PUBLIC base )

# ------------------------------------------------------------------------------
# 04 GAME
# ------------------------------------------------------------------------------

add_unity_library( game ${MODULE_TYPE} "source/game" "04_Game" "game.c" )
target_link_libraries( game PUBLIC base )

# ------------------------------------------------------------------------------
# 04 GAME SERVICE
# ------------------------------------------------------------------------------

add_unity_library( nav STATIC "source/game_service/nav" "04_Game_Service" "nav.c" )
target_link_libraries( nav PUBLIC base )

# ------------------------------------------------------------------------------
# 04 GAME MODULES
# ------------------------------------------------------------------------------

add_module( game_example "source/game_modules/game_example" "04_Game_Modules" "game_example.c" )
target_link_libraries( game_example PUBLIC base )

# ------------------------------------------------------------------------------
# 05 EDITOR
# ------------------------------------------------------------------------------

add_unity_library( editor_host STATIC "source/editor" "05_Editor" "editor.c" )
target_compile_definitions( editor_host PUBLIC EDITOR_STATIC )
target_link_libraries( editor_host PUBLIC engine_mod engine_sys engine_core engine_app )

# ------------------------------------------------------------------------------
# 05 EDITOR SERVICE
# ------------------------------------------------------------------------------

add_unity_library( viewport STATIC "source/editor_service/viewport" "05_Editor_Service" "viewport.c" )
target_link_libraries( viewport PUBLIC base )

# ------------------------------------------------------------------------------
# 05 EDITOR MODULES
# ------------------------------------------------------------------------------

add_module( editor_example "source/editor_modules/editor_example" "05_Editor_Modules" "editor_example.c" )
target_link_libraries( editor_example PUBLIC base )

# ------------------------------------------------------------------------------
# 06 PROJECT
# ------------------------------------------------------------------------------

add_unity_library( sample_game ${MODULE_TYPE} "source/project/sample_game" "06_Project" "sample_game.c" )

# ------------------------------------------------------------------------------
# 07 DEVELOPER
# ------------------------------------------------------------------------------

add_unity_library( dev_build STATIC "source/developer/dev_build" "07_Developer" "dev_build.c" )
target_link_libraries( dev_build PUBLIC engine_sys )

add_unity_library( dev_hot STATIC "source/developer/dev_hot" "07_Developer" "dev_hot.c" )
target_link_libraries( dev_hot PUBLIC engine_sys dev_build )

# ------------------------------------------------------------------------------
# 08 HOST
# ------------------------------------------------------------------------------

add_unity_library( host_common STATIC "source/host/common" "08_Host" "host_common.c" )

add_unity_executable( host_game    "source/host/game"    "08_Host" "host_game_main.c"   )
add_unity_executable( host_editor  "source/host/editor"  "08_Host" "host_editor_main.c" )
add_unity_executable( host_tool    "source/host/tool"    "08_Host" "host_tool_main.c"   )
add_unity_executable( host_sandbox "source/host/sandbox" "08_Host" "host_sandbox.c"     )

target_link_libraries( host_game    PRIVATE base engine_sys engine_mod engine_core engine_app rhi )
target_link_libraries( host_editor  PRIVATE base engine_sys engine_mod engine_core rhi )
target_link_libraries( host_tool    PRIVATE base engine_sys engine_mod engine_core )
target_link_libraries( host_sandbox PRIVATE base engine_sys engine_mod engine_core )

if( ENGINE_MONOLITHIC )
    target_link_libraries( host_game   PUBLIC render audio physics )
    target_link_libraries( host_editor PUBLIC render )
endif()

# --- The Magic IDE Config ---
# Change "sandbox_imgui" here, run your build.bat, and VS will automatically
# pass this argument when you press F5.
# set_target_properties( host_sandbox PROPERTIES
#     VS_DEBUGGER_COMMAND_ARGUMENTS "-module sandbox_imgui"
# )

# ------------------------------------------------------------------------------
# 09 SANDBOX
# ------------------------------------------------------------------------------

add_unity_executable( sb_base            "source/sandbox/base/"                   "09_Sandbox/Base"   "sb_base.c" )
add_unity_executable( sb_engine_mod      "source/sandbox/engine/engine_mod"       "09_Sandbox/Engine" "sb_engine_mod.c" )
add_unity_executable( sb_engine_sys      "source/sandbox/engine/engine_sys"       "09_Sandbox/Engine" "sb_engine_sys.c" )
add_unity_executable( sb_engine_app      "source/sandbox/engine/engine_app"       "09_Sandbox/Engine" "sb_engine_app.c")
add_unity_executable( sb_engine_core     "source/sandbox/engine/engine_core"      "09_Sandbox/Engine" "sb_engine_core.c" )
add_unity_executable( sb_engine_reflect  "source/sandbox/engine/engine_reflect"   "09_Sandbox/Engine" "sb_engine_reflect.c" )

add_unity_executable( sb_runtime_reflect "source/sandbox/runtime/runtime_reflect" "09_Sandbox/Runtime" "sb_runtime_reflect.c" )
add_unity_executable( sb_runtime_gen     "source/sandbox/runtime/runtime_gen"     "09_Sandbox/Runtime" "sb_runtime_gen.c" )

target_link_libraries( sb_base            PRIVATE base )
target_link_libraries( sb_engine_mod      PRIVATE engine_sys engine_mod dev_build dev_hot )
target_link_libraries( sb_engine_sys      PRIVATE engine_sys )
target_link_libraries( sb_engine_app      PRIVATE engine_sys engine_mod engine_app )
target_link_libraries( sb_engine_core     PRIVATE engine_core )
target_link_libraries( sb_engine_reflect  PRIVATE engine_core engine_rs )

target_link_libraries( sb_runtime_reflect PRIVATE engine_sys engine_mod engine_core engine_rs )
target_link_libraries( sb_runtime_gen     PRIVATE engine_sys engine_mod engine_core engine_rs )

# rs_test.c is not a unity entry; compile it as a separate TU for the reflect sandbox
set_source_files_properties( "${CMAKE_SOURCE_DIR}/source/engine/rs/rs_test.c" PROPERTIES HEADER_FILE_ONLY OFF )
target_sources( sb_engine_reflect PRIVATE "${CMAKE_SOURCE_DIR}/source/engine/rs/rs_test.c" )

add_dependencies( sb_runtime_reflect example_reflect )

# ------------------------------------------------------------------------------
# 09 SANDBOX EXAMPLE
#
#   Minimal host setup examples for various use cases, all linking runtime_host.
# ------------------------------------------------------------------------------

add_unity_executable( sb_example_tool    "source/sandbox_example/example_tool"    "09_Sandbox_Example" "example_tool.c"    )
add_unity_executable( sb_example_server  "source/sandbox_example/example_server"  "09_Sandbox_Example" "example_server.c"  )
add_unity_executable( sb_example_editor  "source/sandbox_example/example_editor"  "09_Sandbox_Example" "example_editor.c"  )
add_unity_executable( sb_example_game    "source/sandbox_example/example_game"    "09_Sandbox_Example" "example_game.c"    )
add_unity_executable( sb_example_runtime "source/sandbox_example/example_runtime" "09_Sandbox_Example" "example_runtime.c" )

target_link_libraries( sb_example_tool    PRIVATE runtime_host )
target_link_libraries( sb_example_server  PRIVATE runtime_host )
target_link_libraries( sb_example_editor  PRIVATE runtime_host rhi )
target_link_libraries( sb_example_game    PRIVATE runtime_host rhi )
target_link_libraries( sb_example_runtime PRIVATE runtime_host rhi )

# add_unity_executable( sb_tool_modinfo "source/sandbox/tool_modinfo" "09_Sandbox" "sb_tool_modinfo.c" )
# target_link_libraries( sb_tool_modinfo PRIVATE base engine_sys host_common )

# ------------------------------------------------------------------------------
# MONOLITHIC
# ------------------------------------------------------------------------------

# TODO: wire monolithic static module links into host targets
if( ENGINE_MONOLITHIC )
endif()

# ------------------------------------------------------------------------------
# VS SETTINGS
# ------------------------------------------------------------------------------

set_property( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT sb_example_runtime )

if( CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR MATCHES "Xcode" )
    set_property( GLOBAL PROPERTY USE_FOLDERS ON )
    set_property( GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Auto" )
endif()

# ------------------------------------------------------------------------------