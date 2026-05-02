cmake_minimum_required(VERSION 3.12)
project(hybrid LANGUAGES C)

# ==========================================================
# BUILD MODE TOGGLE
# ==========================================================

# This determines if modules like 'render' are built as DLLs or static libs
option(ENGINE_MONOLITHIC "Build as a single static executable" OFF)

if(ENGINE_MONOLITHIC)
    # This makes -DMODULE_LINK_STATIC visible to EVERY target in the project
    add_compile_definitions(MODULE_LINK_STATIC)
    message(STATUS "Global Flag: MODULE_LINK_STATIC is active.")
endif()

if(ENGINE_MONOLITHIC)
    message(STATUS "Build Mode: Monolithic (Statically Linked)")
else()
    message(STATUS "Build Mode: Dynamic (Hot-Reload Enabled)")
endif()

# ==========================================================
# LANGUAGE STANDARD
# ==========================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ==========================================================
# Solution Folders
# ==========================================================

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ==========================================================
# Output Directories
# ==========================================================

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Handle multi-config generators (Visual Studio, Xcode)
# foreach(OUTPUTCONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
#     string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
#     set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_SOURCE_DIR}/bin")
# endforeach()

# ==========================================================
# Build Type (for single-config generators)
# ==========================================================

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build" FORCE)
endif()

# message(STATUS "Current Source Directory: ${CMAKE_SOURCE_DIR}")

# ==========================================================
# Compiler-specific settings (Recommended Revision)
# ==========================================================

# Check if using the Clang compiler (pure Clang or Clang-CL)
if (CMAKE_C_COMPILER_ID MATCHES "Clang")
    
    # Check if Clang is running in MSVC compatibility mode (Clang-CL)
    if (CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(STATUS "Compiler: Clang-CL (MSVC Frontend)")
        
        # Apply MSVC-style flags, which Clang-CL understands
        add_compile_options(/W4 /WX /wd4100 /wd4189)
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

		# Release-only suppressions for Clang-CL
        add_compile_options("$<$<CONFIG:Release>:/wd4101;/wd4189;/wd4100>")
        add_compile_options("$<$<CONFIG:Release>:-Wno-unused-variable;-Wno-unused-parameter>")
		   
    else()
        message(STATUS "Compiler: Pure Clang (Unix/macOS)")
        
        # Apply standard Unix-style Clang flags
        add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-function)
    endif()

# Check if using native MSVC (Not Clang)
elseif (MSVC)

    message(STATUS "Compiler: Native MSVC")
    
    # Apply standard MSVC flags
    add_compile_options(/W4 /WX /wd4100 /wd4189)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

	# Disable unreferenced variable warnings ONLY in Release
    # C4101: unreferenced local variable
    # C4189: local variable is initialized but not referenced
    # C4100: unreferenced formal parameter
    add_compile_options("$<$<CONFIG:Release>:/wd4101;/wd4189;/wd4100>")
	
else()
    # Catch-all for GCC, etc.
    message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID}")
    
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-function)

endif()


# ==========================================================
# Debug Info for Release Builds
# ==========================================================

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


# ==========================================================
# Create our main executable (engine.exe)
# ==========================================================

# Engine (Contains the 'engine' executable and 'render' library)
add_subdirectory(engine)

# Editor
# add_subdirectory(editor)

# Tools
# add_subdirectory(tools/build_tool)
# add_subdirectory(tools/asset_tool)

# Game Projects
# add_subdirectory(projects/sample_game)

# ==========================================================
# Set default startup project
# ==========================================================

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT engine)

# ==========================================================
# Hide CMake generated builds in Visual Studio project.
# ==========================================================

if(CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR MATCHES "Xcode")
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Auto")  
endif()

