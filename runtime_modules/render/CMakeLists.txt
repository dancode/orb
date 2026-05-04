# ==========================================================
# Engine : Render Library
# ==========================================================

file(GLOB_RECURSE ALL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.natvis"
)
set(COMPILED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/render.c"
)

# ----------------------------------------------------------
# Unity Module
# ----------------------------------------------------------

set(HEADER_SOURCES ${ALL_FILES})
list(REMOVE_ITEM HEADER_SOURCES ${COMPILED_SOURCES})
set_source_files_properties( ${HEADER_SOURCES} PROPERTIES HEADER_FILE_ONLY ON )
set_source_files_properties( ${COMPILED_SOURCES} PROPERTIES HEADER_FILE_ONLY OFF )

# ----------------------------------------------------------
# Build target
# ----------------------------------------------------------

if(ENGINE_MONOLITHIC)
    set(MODULE_TYPE STATIC)
else()
    set(MODULE_TYPE SHARED)
endif()

add_library(render ${MODULE_TYPE}
    ${ALL_FILES}          # everything shows in IDE
    ${COMPILED_SOURCES}   # these actually compile
)

set_target_properties(render PROPERTIES FOLDER "Runtime_Modules")
set_target_properties(render PROPERTIES LINKER_LANGUAGE C )

# ----------------------------------------------------------
# Mirror physical directory structure inside Visual Studio
# ----------------------------------------------------------

source_group(
    TREE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ALL_FILES}
)

# ----------------------------------------------------------
# Searched include directories at base directory
# ----------------------------------------------------------

target_include_directories(render PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}
)
