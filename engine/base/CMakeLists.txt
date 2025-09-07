add_library(base STATIC
    source/base.c
	include/base.h	
)

# PUBLIC: Any other target that links to base Will inherit that directory.
target_include_directories(base PUBLIC include
	${CMAKE_SOURCE_DIR}/engine/base/include
	${CMAKE_SOURCE_DIR}/tools/loader/include
)

set_target_properties(base PROPERTIES FOLDER "Engine/Base")

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    source/base.c
	include/base.h
)



