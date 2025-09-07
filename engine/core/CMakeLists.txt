add_library(core SHARED
    source/core_plugin.c
	core.h
	include/plugin.h
)

# core does NOT link to base; it expects loader to register base API
target_include_directories(core PUBLIC
	${CMAKE_SOURCE_DIR}/engine/core/include
	${CMAKE_SOURCE_DIR}/engine/base/include
)

set_target_properties(core PROPERTIES FOLDER "Engine/Core")

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    source/core_plugin.c
	core.h
	include/plugin.h
)


