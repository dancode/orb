# ==========================================================
# tools/bvild_tool/CMakeLists.txt
# ==========================================================

set(ALL_SOURCES
	build.c
	build.h
	build_vs.c
)

add_executable(build_tool
    ${ALL_SOURCES}
)

# add_library(build_tool SHARED
#    ${ALL_SOURCES}
# )
 
target_include_directories(engine PUBLIC
	${CMAKE_SOURCE_DIR}/engine
)

target_include_directories(build_tool PUBLIC
	${CMAKE_SOURCE_DIR}/tool/build_tool
)

target_link_libraries(build_tool PRIVATE core)

if(ENGINE_MONOLITHIC)
    # In monolithic mode, the engine must statically link the modules.
    # target_link_libraries(engine PUBLIC example)
else()
    # In dynamic mode, do NOT link the render target (load at runtime)
endif()

# engine executable is minimal: links loader + base
# target_link_libraries(engine PUBLIC core)

# Put in a clear project folder
set_target_properties(build_tool PROPERTIES FOLDER "Tool/Build")
set_target_properties(build_tool PROPERTIES LINKER_LANGUAGE C )

# Set the output name to avoid prefix 'lib' on Unix-like systems
# set_target_properties(build_tool PROPERTIES OUTPUT_NAME "build_tool")

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ALL_SOURCES} )

# target_link_libraries(demo_game PRIVATE game)