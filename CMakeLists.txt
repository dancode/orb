cmake_minimum_required(VERSION 3.12)
project(hybrid C)

# Set C standard to C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ==========================================================
# Set output directories
# ==========================================================

# Options
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Handle multi-config generators (Visual Studio, Xcode)
foreach(OUTPUTCONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_SOURCE_DIR}/bin")
endforeach()

# message(STATUS "Current Source Directory: ${CMAKE_SOURCE_DIR}")

# ==========================================================
# Configure different build types
# ==========================================================

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build" FORCE)
endif()

# ==========================================================
# Compile flags
# ==========================================================

if(MSVC)

    # Common flags for all build types	
    add_compile_options(/W4 /WX)  # Enable all warnings and treat them as errors
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

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
# add_subdirectory(tools)

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


