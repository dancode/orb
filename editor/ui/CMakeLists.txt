add_library(editor_ui SHARED
    source/ui.c
)

# core does NOT link to base; it expects loader to register base API
target_include_directories(editor_ui PUBLIC
	${CMAKE_SOURCE_DIR}/editor/ui/include
	${CMAKE_SOURCE_DIR}/engine/base/include
)

set_target_properties(editor_ui PROPERTIES FOLDER "Editor/UI")

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    source/ui.c
)


