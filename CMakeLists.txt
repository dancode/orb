cmake_minimum_required(VERSION 3.12)
project(orb C)

# Set C standard to C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Enable solution folders for better organization in Visual Studio
# set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ==========================================================
# Set output directories
# ==========================================================

# Force all library/archive outputs into root/lib
# set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib")
# set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib")

# Handle multi-config generators (Visual Studio, Xcode)
foreach(OUTPUTCONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG_UPPER)
    # set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_SOURCE_DIR}/lib")
    # set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_SOURCE_DIR}/lib")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG_UPPER} "${CMAKE_SOURCE_DIR}/bin")
endforeach()

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
	# set(CMAKE_C_FLAGS_DEBUG "/DEBUG /Od /Zi /MDd") 
	# set(CMAKE_C_FLAGS_RELEASE "/O2 /Oi /GL /DNDEBUG /MD") 

endif()

# ==========================================================
# Create Shared Headers (Folder)
# ==========================================================

add_subdirectory(common)

# file(GLOB_RECURSE COMMON_FILES "common/*.h")
# add_custom_target( com SOURCES ${COMMON_FILES})
# set_target_properties(com PROPERTIES  FOLDER "Common" PROJECT_LABEL "common")
# source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES	
#  	${COMMON_FILES}	
# )

# ==========================================================
# Shared Headers
# ==========================================================

add_library(common INTERFACE)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)
# set_target_properties(common PROPERTIES FOLDER "Common/")

# Add headers as sources so Visual Studio shows them
target_sources(common INTERFACE
    ${COMMON_HEADERS}
)

# ==========================================================
# Engine modules
# ==========================================================

add_subdirectory(engine/base)
add_subdirectory(engine/core)
# add_subdirectory(engine/render)

# ==========================================================
# Editor modules
# ==========================================================

add_subdirectory(editor/ui)

# ==========================================================
# Loader + Executables
# ==========================================================

add_subdirectory(tools/loader)
add_subdirectory(tools/engine)
add_subdirectory(tools/editor)

# ==========================================================
# Samples (official demos)
# ==========================================================

# add_subdirectory(samples/minimal)
# add_subdirectory(samples/first_person)

# ==========================================================
# Projects (user games)
# ==========================================================

# You can add custom projects here or glob them dynamically.
# For now, we'll add one example project manually.

if(EXISTS "${CMAKE_SOURCE_DIR}/projects/blocker/CMakeLists.txt")

    # add_subdirectory(projects/blocker)
	
endif()

# ==========================================================
# Hide CMake generated builds in Visual Studio project.
# ==========================================================

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT engine)

if(CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR MATCHES "Xcode")

    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Auto")
	
	# set_target_properties(ZERO_CHECK PROPERTIES FOLDER "Auto")
	# set_target_properties(ALL_BUILD PROPERTIES  FOLDER "Auto")
  
endif()
  

# Hide CMake internal targets in IDE project browsers
# if(CMAKE_GENERATOR MATCHES "Visual Studio" OR CMAKE_GENERATOR MATCHES "Xcode")
#     # Function to hide CMake internal targets
#     function(hide_cmake_internal_targets)
#         # Common CMake internal targets to hide
#         set(INTERNAL_TARGETS 
# 			ZERO_CHECK 
# 			ALL_BUILD 					
# 		)        
#         foreach(target IN LISTS INTERNAL_TARGETS)
#             if(TARGET ${target})
#                 set_target_properties(${target} PROPERTIES FOLDER "CMakeInternal")
#             endif()
#         endforeach()
#     endfunction()
#     
#     # Call the function to hide internal targets
#     hide_cmake_internal_targets()
# endif()

# ==========================================================