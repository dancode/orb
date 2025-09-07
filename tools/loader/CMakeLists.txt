add_library(loader OBJECT 
	source/loader.c
	# source/registry.c
	include/loader.h
	include/api_registry.h
)

# set_source_files_properties(
# 	source/registry.c
#     PROPERTIES HEADER_FILE_ONLY ON
# )

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    source/loader.c
	# source/registry.c
    include/loader.h
	include/api_registry.h
)

target_include_directories(loader PUBLIC
    ${CMAKE_SOURCE_DIR}/tools/loader/include
    ${CMAKE_SOURCE_DIR}/engine/base/include
)

# message(STATUS "CMAKE_SOURCE_DIR = ${CMAKE_SOURCE_DIR}")

# Put in a clear project folder
set_target_properties(loader PROPERTIES FOLDER "Tools/Loader")

# add_library(loader STATIC
#     loader.c
# 	registry.c
# 	loader.h
# 	api_registry.h
# )
# 
# target_include_directories(loader PUBLIC include)
# target_include_directories(base PRIVATE include)
# 
# set_source_files_properties(
#     registry.c
#     PROPERTIES HEADER_FILE_ONLY ON
# )
# 
# set_target_properties(loader PROPERTIES FOLDER "Loader")
# 
# # Remove the default "Header Files" / "Source Files" filters in Visual Studio
# source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
#     loader.c
# 	registry.c
#     loader.h
# 	api_registry.h
# )

