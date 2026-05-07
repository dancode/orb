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

# Handle multi-config generators (Visual Studio, Xcode)
# foreach(OUTPUTCONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
#     string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
#     set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_SOURCE_DIR}/bin")
# endforeach()

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

# This allows `#include "orb.h"` and `#include "engine/core/core_api.h"` 
# to work from anywhere in the codebase.

include_directories(${CMAKE_SOURCE_DIR}/source)

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
    # pass the local filenames (e.g., "sys_ub.c").
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
    # source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/${SOURCE_DIR}" FILES ${TARGET_FILES})

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
# 0. Solution-Level Files (Organization)
# ------------------------------------------------------------------------------
# Create a dummy target to hold root-level files in Visual Studio

set(ROOT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/source/orb.h"
)

# ${CMAKE_SOURCE_DIR}/CMakeLists.txt
# ${CMAKE_SOURCE_DIR}/generate_vs.bat 
# You can add your .bat files or README.md here later so they show up in VS too!

add_custom_target(root SOURCES ${ROOT_SOURCES})
set_target_properties(root PROPERTIES FOLDER "0_Root")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/source" FILES ${ROOT_SOURCES})

# ------------------------------------------------------------------------------
# 2. BASE (Stateless Standard Library)
# ------------------------------------------------------------------------------

# Single Unity File (The standard way)
add_unity_library(base STATIC "source/base" "02_Base"
    "base_ub.c"
)

# ------------------------------------------------------------------------------
# 3. ENGINE
# ------------------------------------------------------------------------------

# 3. ENGINE : SUS
add_unity_library(engine_sys STATIC "source/engine/sys" "03_Engine"
    "sys_ub.c"
)

# # 3. ENGINE : CORE
add_unity_library(engine_core STATIC "source/engine/core" "03_Engine"
	"core_api.c"
	"core_sid.c"
	"core_cvar.c"
	"core_debug.c"
	"core_log.c"
	"core_reflect.c"
    "core_memory.c"    
)

# 3. ENGINE : MOD
add_unity_library(engine_mod STATIC "source/engine/mod" "03_Engine"
    "mod.c"
)

# 3. ENGINE : App
add_unity_library(engine_app STATIC "source/engine/app" "03_Engine"
    "app.c"
)

# Module Engine Dependencies
target_link_libraries(engine_sys  PUBLIC base)
target_link_libraries(engine_core PUBLIC base engine_sys )
target_link_libraries(engine_mod  PUBLIC base engine_sys)
target_link_libraries(engine_app  PUBLIC base engine_sys engine_core)

# ------------------------------------------------------------------------------
# 4. RUNTIME
# ------------------------------------------------------------------------------

# 4. RUNTIME
add_unity_library(runtime STATIC "source/runtime" "04_Runtime"
    "runtime.c"
)

# ------------------------------------------------------------------------------
# 5. RUNTIME MODULES (Dynamic or Static in Monolithic build)
# ------------------------------------------------------------------------------

add_unity_library(example ${MODULE_TYPE} "source/runtime_modules/example" "05_RuntimeModules"
    "example.c"
)
add_unity_library(render ${MODULE_TYPE} "source/runtime_modules/render" "05_RuntimeModules"
    "render.c"
)
add_unity_library(audio ${MODULE_TYPE} "source/runtime_modules/audio" "05_RuntimeModules"
    "audio.c"
)

target_link_libraries( example PUBLIC base )
target_link_libraries( render PUBLIC base )
target_link_libraries( audio PUBLIC base )

# ------------------------------------------------------------------------------
# 6. GAME (FRAMEWORK)
# ------------------------------------------------------------------------------

# 6. GAME (FRAMEWORK)
add_unity_library(game ${MODULE_TYPE} "source/game" "06_Game"
    "game.c"
)

target_link_libraries( game PUBLIC base )

# ------------------------------------------------------------------------------
# 7. USER PROJECTS (Additional Game DLL)
# ------------------------------------------------------------------------------

# 7. GAME (FRAMEWORK)
add_unity_library(sample_game ${MODULE_TYPE} "source/projects/sample_game" "07_Project"
    "sample_game.c"
)

# target_link_libraries(engine_core PUBLIC base engine_sys engine_core )
# add_engine_dll(amberfall "7_Projects" PROJECTS/amberfall/source/af_unity.c "base;engine_core;game_framework")

# ------------------------------------------------------------------------------
# 8. HOSTS (Executables)
# ------------------------------------------------------------------------------

#Engine Host (Used for testing core engine plumbing)
# add_executable( host_engine "source/hosts/engine/engine_main.c" )
# target_link_libraries( host_engine PRIVATE base engine_sys engine_mod engine_core )
# set_target_properties( host_engine PROPERTIES FOLDER "0_Hosts" )

add_unity_executable(host_engine "source/hosts/engine" "01_Hosts"
    "engine_main.c"
    "engine_test.c"
)
target_link_libraries( host_engine PRIVATE 
    base 
    engine_sys 
    engine_mod 
    engine_core 
)

# Runtime Host (Minimal, for testing runtime modules without the full game)
add_unity_executable( host_runtime "source/hosts/runtime" "01_Hosts"
    "runtime_main.c"
)
target_link_libraries( host_runtime PRIVATE 
    base
    engine_sys 
    engine_mod
    engine_core
)

if(ENGINE_MONOLITHIC)
target_link_libraries(host_runtime PUBLIC example)
endif()

# Game Host
#add_executable(host_game HOST/game/main.c)
#target_link_libraries(host_game PRIVATE runtime_host game_framework)
#set_target_properties(host_game PROPERTIES FOLDER "0_Hosts")

# Tool Host (Headless! Notice it links sys and mod, but NOT app)
# add_executable(host_tool HOST/tool/main.c)
# target_link_libraries(host_tool PRIVATE base engine_core engine_sys engine_mod)
# set_target_properties(host_tool PROPERTIES FOLDER "0_Hosts")

# Editor Host
# add_executable(host_editor HOST/editor/main.c EDITOR/host/editor_host.c)
# target_link_libraries(host_editor PRIVATE runtime_host)
# set_target_properties(host_editor PROPERTIES FOLDER "0_Hosts")

# ------------------------------------------------------------------------------
# 3. MONOLITHIC Statically Links All DLL's
# ------------------------------------------------------------------------------

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
# Create our main executable (engine.exe)
# ------------------------------------------------------------------------------

# add_subdirectory(base)
### add_subdirectory(engine)

### add_subdirectory(runtime)
### add_subdirectory(runtime_modules/example)
### add_subdirectory(runtime_modules/render)
### add_subdirectory(runtime_modules/audio)
### 
### add_subdirectory(hosts)

# add_subdirectory(runtime/modules/example)
# add_subdirectory(runtime/modules/render)
# add_subdirectory(runtime/modules/audio)


# Editor
# add_subdirectory(editor)

# Tools
# add_subdirectory(tools/build_tool)
# add_subdirectory(tools/asset_tool)

# Game Projects
# add_subdirectory(projects/sample_game)

# ------------------------------------------------------------------------------
# Set default startup project
# ------------------------------------------------------------------------------

### set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT engine)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT host_engine)

# ------------------------------------------------------------------------------
# Hide CMake generated builds in Visual Studio project.
# ------------------------------------------------------------------------------

if(CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR MATCHES "Xcode")
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Auto")  
endif()

# ------------------------------------------------------------------------------