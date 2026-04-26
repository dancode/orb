# ==========================================================
# tools/asset_tool/CMakeLists.txt
# ==========================================================

set(ALL_SOURCES
	asset_tool.c
	asset_tool.h
	asset_tool_vs.c
)

add_executable(asset_tool
    ${ALL_SOURCES}
)

target_include_directories(engine PUBLIC
	${CMAKE_SOURCE_DIR}/engine
)

target_include_directories(asset_tool PUBLIC
	${CMAKE_SOURCE_DIR}/tool/asset_tool
)

target_link_libraries(asset_tool PRIVATE platform_sys)

# engine executable is minimal: links loader + base
# target_link_libraries(engine PUBLIC core)

# Put in a clear project folder
set_target_properties(asset_tool PROPERTIES FOLDER "Tool/Asset")
set_target_properties(asset_tool PROPERTIES LINKER_LANGUAGE C )

# Set the output name to avoid prefix 'lib' on Unix-like systems
# set_target_properties(asset_tool PROPERTIES OUTPUT_NAME "asset_tool")

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ALL_SOURCES} )

# target_link_libraries(demo_game PRIVATE game)