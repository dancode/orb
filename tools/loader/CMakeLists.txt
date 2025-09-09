add_library(loader OBJECT 
	source/loader.c
	include/loader.h
)

# set_source_files_properties(
# 	source/registry.c
#     PROPERTIES HEADER_FILE_ONLY ON
# )

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    source/loader.c
    include/loader.h
)

target_include_directories(loader PUBLIC
    ${CMAKE_SOURCE_DIR}/tools/loader/include
    ${CMAKE_SOURCE_DIR}/engine/base/include
)

target_link_libraries(loader PRIVATE common)

# message(STATUS "CMAKE_SOURCE_DIR = ${CMAKE_SOURCE_DIR}")

# Put in a clear project folder
set_target_properties(loader PROPERTIES FOLDER "Tools/")
