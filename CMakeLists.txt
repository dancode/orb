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
# Create libray function (for Unity builds)
# ------------------------------------------------------------------------------

function(add_unity_library TARGET_NAME TARGET_TYPE SOURCE_DIR VS_FOLDER)
    
    # 1. Resolve the absolute path to the module directory.
    # Anchoring to CMAKE_SOURCE_DIR guarantees this works flawlessly even 
    # if you call this function from a nested CMakeLists.txt file.
    set(MODULE_ROOT "${CMAKE_SOURCE_DIR}/${SOURCE_DIR}")

    # 2. Collect all raw files for IDE visibility
    file(GLOB_RECURSE TARGET_FILES
        "${MODULE_ROOT}/*.c"
        "${MODULE_ROOT}/*.h"
        "${MODULE_ROOT}/*.natvis"
    )

    # 3. Process ALL trailing arguments (ARGN) as files to compile.
    # We automatically prepend the MODULE_ROOT here so you only have to 
    # pass the local filenames (e.g., "sys.c").
    set(TARGET_COMPILED "")
    foreach(SRC_FILE IN LISTS ARGN)
        list(APPEND TARGET_COMPILED "${MODULE_ROOT}/${SRC_FILE}")
    endforeach()

    # 4. Create the Headers list by cloning the full list and removing the compiled files
    #    CMake's REMOVE_ITEM can handle a list, so we can remove them all at once
    set(TARGET_HEADERS ${TARGET_FILES})        
    if(TARGET_COMPILED)
        list(REMOVE_ITEM TARGET_HEADERS ${TARGET_COMPILED})
    endif()

    # 5. Apply properties
    set_source_files_properties(${TARGET_HEADERS} PROPERTIES HEADER_FILE_ONLY ON)
    set_source_files_properties(${TARGET_COMPILED} PROPERTIES HEADER_FILE_ONLY OFF)

    # 6. Build the library (Static or Shared based on TARGET_TYPE)
    add_library(${TARGET_NAME} ${TARGET_TYPE}
        ${TARGET_HEADERS}
        ${TARGET_COMPILED}
    )

    # 7. Organize inside Visual Studio Solution Explorer
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${VS_FOLDER})

    # 8. Mirror physical directory structure inside Visual Studio
    source_group(TREE "${MODULE_ROOT}" FILES ${TARGET_FILES})

    # 9. Tuck the mandatory CMake script out of sight
    source_group("CMake Rules" FILES "CMakeLists.txt")

endfunction()

# ------------------------------------------------------------------------------
# Create Executable function (for Unity builds)
# ------------------------------------------------------------------------------

function(add_unity_executable TARGET_NAME SOURCE_DIR VS_FOLDER)
    
    # 1. Resolve the absolute path to the host directory.
    set(MODULE_ROOT "${CMAKE_SOURCE_DIR}/${SOURCE_DIR}")

    # 2. Collect all raw files for IDE visibility
    file(GLOB_RECURSE TARGET_FILES
        "${MODULE_ROOT}/*.c"
        "${MODULE_ROOT}/*.h"
        "${MODULE_ROOT}/*.natvis"
    )

    # 3. Process ALL trailing arguments (ARGN) as files to compile.
    set(TARGET_COMPILED "")
    foreach(SRC_FILE IN LISTS ARGN)
        list(APPEND TARGET_COMPILED "${MODULE_ROOT}/${SRC_FILE}")
    endforeach()

    # 4. Create the Headers list by cloning and removing the compiled files
    set(TARGET_HEADERS ${TARGET_FILES})            
    if(TARGET_COMPILED)
        list(REMOVE_ITEM TARGET_HEADERS ${TARGET_COMPILED})
    endif()

    # 5. Apply properties (tell VS not to compile the individual .c files)
    set_source_files_properties(${TARGET_HEADERS} PROPERTIES HEADER_FILE_ONLY ON)
    set_source_files_properties(${TARGET_COMPILED} PROPERTIES HEADER_FILE_ONLY OFF)

    # 6. Build the Executable (Notice: No TARGET_TYPE here)
    add_executable(${TARGET_NAME}
        ${TARGET_HEADERS}
        ${TARGET_COMPILED}
    )

    # 7. Organize inside Visual Studio Solution Explorer
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${VS_FOLDER})

    # 8. Mirror physical directory structure inside Visual Studio
    source_group(TREE "${MODULE_ROOT}" FILES ${TARGET_FILES})

    # 9. Tuck the mandatory CMake script out of sight
    source_group("CMake Rules" FILES "CMakeLists.txt")

endfunction()

# ------------------------------------------------------------------------------
# PDB unlock helper (hot-reload under debugger, MSVC dynamic builds only)
# ------------------------------------------------------------------------------
# Call once after each add_unity_library that produces a hot-reloadable DLL.
# In monolithic or non-MSVC builds this expands to nothing.

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

# ------------------------------------------------------------------------------
# add_module: wrapper for hot-reloadable runtime modules.
#
#   - Calls add_unity_library with MODULE_TYPE (SHARED or STATIC per build mode)
#   - Adds PRIVATE <NAME>_STATIC so the module can call its own API through
#     the standard gateway (example_api()->func()) safely in dynamic builds
#   - Registers the PRE_LINK PDB unlock for debugger-safe rebuilds
#
# Usage (identical to add_unity_library, just swaps the function name):
#   add_module( example "source/runtime_modules/example" "05_Runtime_Modules" "example.c" )
# ------------------------------------------------------------------------------

function(add_module TARGET_NAME SOURCE_DIR VS_FOLDER)
    add_unity_library( ${TARGET_NAME} ${MODULE_TYPE} ${SOURCE_DIR} ${VS_FOLDER} ${ARGN} )

    string(TOUPPER "${TARGET_NAME}" UPPER)
    target_compile_definitions( ${TARGET_NAME} PRIVATE ${UPPER}_STATIC )

    target_unlock_pdb_on_rebuild( ${TARGET_NAME} )
endfunction()

# ------------------------------------------------------------------------------
# 00 Project Level Files (not compiled - organization and visibility only)
# ------------------------------------------------------------------------------
# Create a dummy target to hold root-level files in Visual Studio

set( ROOT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/source/orb.h"
)
set( DOC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/project.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/TODO.txt"
)

add_custom_target(root SOURCES ${ROOT_SOURCES} ${DOC_SOURCES} )
set_target_properties(root PROPERTIES FOLDER "00_Root")

source_group( "Source" "${CMAKE_CURRENT_SOURCE_DIR}/source" FILES ${ROOT_SOURCES})
source_group( "Docs" "${CMAKE_CURRENT_SOURCE_DIR}/docs" FILES ${DOC_SOURCES})

# ------------------------------------------------------------------------------
# 01 BASE (Stateless Standard Library)
# ------------------------------------------------------------------------------

add_unity_library(base STATIC "source/base" "01_Base" "base.c" )

# ------------------------------------------------------------------------------
# 02 ENGINE (Stateful Engine Framework)
# ------------------------------------------------------------------------------

# SYS : shared OS dependency, the only module dependency in engine allowed.
add_unity_library( engine_sys STATIC "source/engine/sys" "02_Engine" "sys.c" )
target_compile_definitions( engine_sys PUBLIC SYS_STATIC )

# ------------------------------------------------------------------------------

# MOD
add_unity_library( engine_mod STATIC "source/engine/mod" "02_Engine" "mod.c" )
target_compile_definitions( engine_mod PUBLIC MOD_STATIC )

# APP
add_unity_library( engine_app STATIC "source/engine/app" "02_Engine" "app.c" )
target_compile_definitions( engine_app PUBLIC APP_STATIC )

# CORE
add_unity_library( engine_core STATIC "source/engine/core" "02_Engine" "core.c" )
target_compile_definitions( engine_core PUBLIC CORE_STATIC )

# ENGINE LINK ORDER: SYS -> CORE -> MOD -> APP

# target_link_libraries( engine_sys  PUBLIC base )
target_link_libraries( engine_core PUBLIC engine_sys )
target_link_libraries( engine_mod  PUBLIC engine_sys )
target_link_libraries( engine_app  PUBLIC engine_sys )

# ------------------------------------------------------------------------------
# 03 RUNTIME: (EXE HOST + SERVICES)
# ------------------------------------------------------------------------------

# RUNTIME HOST (The "Host" side of the runtime, which is linked by the host executables)
# SOURCE_DIR covers all of source/runtime so run_api.c and host/host.c are both visible.
add_unity_library( runtime_host STATIC "source/runtime" "03_Runtime" "runtime.c" )
target_compile_definitions( runtime_host PUBLIC RUN_STATIC )
target_link_libraries( runtime_host PUBLIC engine_mod engine_sys engine_core engine_app )

# RUNTIME HOST (The "Host" side of the runtime, which is linked by the host executables)
#add_unity_library(runtime_host STATIC "source/runtime/host" "03_Runtime/Host"
#    "runtime_host.c"
#)
#target_link_libraries( runtime_host PUBLIC base engine_mod engine_sys engine_core engine_app )

#RUNTIEM APP (The "App" side of the runtime, which is linked by the runtime modules and the sandbox runtime)
# add_unity_library( runtime_app STATIC "source/runtime/app" "03_Runtime/App"
#     "runtime_app.c"
# )
# target_link_libraries( runtime_app PUBLIC runtime_host )


# RUNTIME SERVICES
add_unity_library( jobs STATIC "source/runtime_service/jobs" "03_Runtime_Service" "jobs.c" )

# Note: The runtime_host library doesn't actually link the services directly 
#       The final executable (host_game) links them all together!

# ------------------------------------------------------------------------------
# 03 RUNTIME: MODULES
# ------------------------------------------------------------------------------

# DYNAMIC MODULES (Built as separate DLLs for hot-reloading, but statically linked in monolithic mode)
add_module( example "source/runtime_modules/example" "03_Runtime_Modules" "example.c" )
add_module( render  "source/runtime_modules/render"  "03_Runtime_Modules" "render.c"  )
add_module( audio   "source/runtime_modules/audio"   "03_Runtime_Modules" "audio.c"   )
add_module( physics "source/runtime_modules/physics" "03_Runtime_Modules" "physics.c" )

# LINK
target_link_libraries( example PUBLIC base )
target_link_libraries( render PUBLIC base )
target_link_libraries( audio PUBLIC base )
target_link_libraries( physics PUBLIC base )

# ------------------------------------------------------------------------------
# 04 DEVELOPER MODULES (dev_* services — link only in non-shipping hosts)
# ------------------------------------------------------------------------------

add_unity_library( dev_build STATIC "source/developer/dev_build" "04_Developer" "dev_build.c" )
target_link_libraries( dev_build PUBLIC engine_sys )

add_unity_library( dev_hot STATIC "source/developer/dev_hot" "04_Developer" "dev_hot.c")
target_link_libraries( dev_hot PUBLIC engine_sys dev_build )

# ------------------------------------------------------------------------------
# 04 GAME (FRAMEWORK)
# ------------------------------------------------------------------------------

# GAME
add_unity_library(game ${MODULE_TYPE} "source/game" "04_Game"
    "game.c"
)
target_link_libraries( game PUBLIC base )

# ------------------------------------------------------------------------------
# 05 PROJECTS (THE ACTUAL GAME IMPLEMENTATION PROJECT)
# ------------------------------------------------------------------------------

# 05 PROJECT
add_unity_library(sample_game ${MODULE_TYPE} "source/project/sample_game" "05_Project"
    "sample_game.c"
)

# ------------------------------------------------------------------------------
# 06. HOST SHARED LIBRARY
# ------------------------------------------------------------------------------

add_unity_library( host_common STATIC "source/host/common" "06_Host" 
    "host_common.c"
)

# ------------------------------------------------------------------------------
# 06 HOSTS (Executables)
# ------------------------------------------------------------------------------

# HOST Game
add_unity_executable( host_game "source/host/game" "06_Host" "host_game_main.c" )
target_link_libraries( host_game PRIVATE base engine_sys engine_mod engine_core engine_app )
if ( ENGINE_MONOLITHIC ) 
    target_link_libraries( host_game PUBLIC render audio physics )
endif()

# HOST Editor
add_unity_executable( host_editor "source/host/editor" "06_Host" "host_editor_main.c" )
target_link_libraries( host_editor PRIVATE base engine_sys engine_mod engine_core )
if ( ENGINE_MONOLITHIC ) 
    target_link_libraries( host_editor PUBLIC render )
endif()

# HOST Tool
add_unity_executable( host_tool "source/host/tool" "06_Host" "host_tool_main.c" )
target_link_libraries( host_tool PRIVATE base engine_mod engine_sys engine_core )

# HOST Sandbox
add_unity_executable( host_sandbox "source/host/sandbox" "06_Host" "host_sandbox.c" )
target_link_libraries( host_sandbox PRIVATE base engine_sys engine_mod engine_core )
if ( ENGINE_MONOLITHIC ) 
endif()

# --- The Magic IDE Config ---
# Change "sandbox_imgui" here, run your build.bat, and VS will automatically 
# pass this argument when you press F5.
# set_target_properties( host_sandbox PROPERTIES 
#     VS_DEBUGGER_COMMAND_ARGUMENTS "-module sandbox_imgui"
# )

# ------------------------------------------------------------------------------
# 07 SANDBOX (Executables)
# ------------------------------------------------------------------------------

# SANDBOX ENGINE : LIBRARY TESTS MODULES FOR ENGINE

add_unity_executable( sb_engine_mod "source/sandbox/engine_mod" "07_Sandbox" "sb_engine_mod.c" )
target_link_libraries( sb_engine_mod PRIVATE engine_sys engine_mod dev_build dev_hot )

add_unity_executable( sb_engine_sys "source/sandbox/engine_sys" "07_Sandbox" "sb_engine_sys.c" )
target_link_libraries( sb_engine_sys PRIVATE engine_sys )

add_unity_executable( sb_engine_app "source/sandbox/engine_app" "07_Sandbox" "sb_engine_app.c" )
target_link_libraries( sb_engine_app PRIVATE engine_mod engine_sys engine_app )

add_unity_executable( sb_engine_core "source/sandbox/engine_core" "07_Sandbox" "sb_engine_core.c" )
target_link_libraries( sb_engine_core PRIVATE engine_core )

# SANDBOX EXAMPLES : Simple host setup examples for various use cases, 
# all linking runtime_host to show how it provides access to the engine.

# runtime_host includes engine static libraries already, so we don't
# need to link them again here. Just link runtime_host and it works.

# TODO: Use host_common for shared code for parsing args, etc. 

add_unity_executable( sb_example_tool "source/sandbox_example/example_tool" "07_Sandbox_Example" "example_tool.c" )
target_link_libraries( sb_example_tool PRIVATE runtime_host )

add_unity_executable( sb_example_server "source/sandbox_example/example_server" "07_Sandbox_Example" "example_server.c" )
target_link_libraries( sb_example_server PRIVATE runtime_host )

add_unity_executable( sb_example_editor "source/sandbox_example/example_editor" "07_Sandbox_Example" "example_editor.c" )
target_link_libraries( sb_example_editor PRIVATE runtime_host )

add_unity_executable( sb_example_game "source/sandbox_example/example_game" "07_Sandbox_Example" "example_game.c" )
target_link_libraries( sb_example_game PRIVATE runtime_host )

add_unity_executable( sb_example_runtime "source/sandbox_example/example_runtime" "07_Sandbox_Example" "example_runtime.c" )
target_link_libraries( sb_example_runtime PRIVATE runtime_host )


# add_unity_executable( sb_tool_modinfo "source/sandbox/tool_modinfo" "07_Sandbox" "sb_tool_modinfo.c" )
# target_link_libraries( sb_tool_modinfo PRIVATE base engine_sys host_common )

# ------------------------------------------------------------------------------
# MONOLITHIC BUILD COMBINES ALL LIBRARIES INTO HOST TARGET
# ------------------------------------------------------------------------------

# TODO: fixup dependencies later
if(ENGINE_MONOLITHIC)
# In monolithic mode, the engine must statically link the modules.
# target_link_libraries(engine PUBLIC example)
# target_link_libraries(engine PUBLIC render)
# target_link_libraries(engine PUBLIC audio)
# target_link_libraries(engine PUBLIC game)
# target_link_libraries(engine PUBLIC sample_game)
#else()
#    # In dynamic mode, do NOT link the render target (load at runtime)
endif()

# ------------------------------------------------------------------------------
# Set default startup project
# ------------------------------------------------------------------------------

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT sb_example_runtime )

# ------------------------------------------------------------------------------
# Hide CMake generated builds in Visual Studio project.
# ------------------------------------------------------------------------------

if(CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR MATCHES "Xcode")
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Auto")  
endif()

# ------------------------------------------------------------------------------