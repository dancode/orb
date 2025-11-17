# ==========================================================
# Engine : Core Library
# ==========================================================

# Collect all .c/.h/.natvis etc for IDE visibility
file(GLOB_RECURSE ALL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.natvis"
)

# Select which .c files should ACTUALLY compile
# Everything else is HEADER_FILE_ONLY and treated as "unity fragments"
set(COMPILED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core_api.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sid_system.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/module_system.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cvar_system.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/reflect_system.c"    
)

# Compute list of all other .c files (excluding compiled sources)
set(HEADER_SOURCES ${ALL_FILES})
list(REMOVE_ITEM HEADER_SOURCES ${COMPILED_SOURCES})

# Mark all other .c files as header-only (not compiled)
# Explicitly allow compiled files to compile normally
set_source_files_properties( ${HEADER_SOURCES} PROPERTIES HEADER_FILE_ONLY ON )
set_source_files_properties( ${COMPILED_SOURCES} PROPERTIES HEADER_FILE_ONLY OFF )

# ----------------------------------------------------------
# 3. Build target
# ----------------------------------------------------------

add_library(core STATIC
    ${ALL_FILES}          # everything shows in IDE
    ${COMPILED_SOURCES}   # these actually compile
)

set_target_properties(core PROPERTIES FOLDER "Engine")

# IMPORTANT: Explicitly mark as C language for Clang-CL
set_target_properties(core PROPERTIES LINKER_LANGUAGE C )

# Mirror physical directory structure inside Visual Studio
source_group(
    TREE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ALL_FILES}
)

target_include_directories(core PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)
