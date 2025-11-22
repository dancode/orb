cmake_minimum_required(VERSION 3.12)
project(hybrid LANGUAGES C)

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

        # Apply Clang-specific suppressions (still use -W for these)
        add_compile_options(-Wno-unused-function -Wno-unused-variable)
    
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

else()
    # Catch-all for GCC, etc.
    message(STATUS "Compiler: ${CMAKE_C_COMPILER_ID}")
    
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-function)

endif()

# ==========================================================
# Create our main executable (engine.exe)
# ==========================================================

# Engine
add_subdirectory(engine)

# Game framework
# add_subdirectory(game)

# Editor
# add_subdirectory(editor)

# Tools
add_subdirectory(tools/build_tool)

# Projects
add_subdirectory(projects/sample_game)

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

