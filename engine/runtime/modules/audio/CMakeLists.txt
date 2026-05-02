# ==========================================================
# Engine : Audio Module Library
# ==========================================================

file(GLOB_RECURSE ALL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.natvis"
)
set(COMPILED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/audio.c"
)

# ----------------------------------------------------------
# Set As Unity Build For Compiled Sources Only
# ----------------------------------------------------------

set(HEADER_SOURCES ${ALL_FILES})
list(REMOVE_ITEM HEADER_SOURCES ${COMPILED_SOURCES})
set_source_files_properties( ${HEADER_SOURCES} PROPERTIES HEADER_FILE_ONLY ON )
set_source_files_properties( ${COMPILED_SOURCES} PROPERTIES HEADER_FILE_ONLY OFF )

# ----------------------------------------------------------
# Create Build Target For Static or Dynamic (default) Module
# ----------------------------------------------------------

if(ENGINE_MONOLITHIC)
    set(MODULE_TYPE STATIC)
else()
    set(MODULE_TYPE SHARED)
endif()

add_library(audio ${MODULE_TYPE}
    ${ALL_FILES}          # everything that shows in IDE
    ${COMPILED_SOURCES}   # these actually compile
)

# ----------------------------------------------------------
# Set Automatic Include Directories
# ----------------------------------------------------------

set_target_properties(audio PROPERTIES FOLDER "Engine/System")
set_target_properties(audio PROPERTIES LINKER_LANGUAGE C )

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

target_include_directories(audio PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}
)
