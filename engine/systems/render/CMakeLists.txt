# ==========================================================
# Engine : Render Library
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
    "${CMAKE_CURRENT_SOURCE_DIR}/render.c"
)

# ----------------------------------------------------------
# Unity Module
# ----------------------------------------------------------

# Compute list of all other .c files (excluding compiled sources)
set(HEADER_SOURCES ${ALL_FILES})
list(REMOVE_ITEM HEADER_SOURCES ${COMPILED_SOURCES})

# Mark all other .c files as header-only (not compiled)
# Explicitly allow compiled files to compile normally
set_source_files_properties( ${HEADER_SOURCES} PROPERTIES HEADER_FILE_ONLY ON )
set_source_files_properties( ${COMPILED_SOURCES} PROPERTIES HEADER_FILE_ONLY OFF )

# ----------------------------------------------------------
# Build target
# ----------------------------------------------------------

add_library(render SHARED
    ${ALL_FILES}          # everything shows in IDE
    ${COMPILED_SOURCES}   # these actually compile
)

set_target_properties(render PROPERTIES FOLDER "Engine/Systems")
set_target_properties(render PROPERTIES LINKER_LANGUAGE C )

# Mirror physical directory structure inside Visual Studio
source_group(
    TREE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ALL_FILES}
)

target_include_directories(render PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}
)
