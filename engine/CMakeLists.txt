# ==========================================================
# engine/CMakeLists.txt
# ==========================================================

add_subdirectory(core)
add_subdirectory(platform)

# add_library(engine_systems STATIC
#     systems/render/renderer.c
#     systems/audio/mixer.c
#     systems/physics/physics.c
# )
# 
# add_executable(engine_runtime runtime/engine_main.c)
# target_link_libraries(engine_runtime
#     PRIVATE engine_core engine_platform engine_systems
# )

# add_executable(engine_runtime runtime/engine_main.c)
# target_link_libraries(engine_runtime
#     PRIVATE engine_core engine_platform engine_systems
# )

# ==========================================================
# engine/CMakeLists.txt
# ==========================================================

# Collect source files in one place
set(ENGINE_SOURCES
	orb.h
	
    engine_main.c
	engine.h
	engine.c
		
	base/base.h	
	base/base.c
	
	base/standard.h
	base/standard.c
	base/standard_string.c
	
	base/module.h
	base/module.c
)

add_executable(engine
    ${ENGINE_SOURCES}
)

target_include_directories(engine PUBLIC
	${CMAKE_SOURCE_DIR}/engine
)

# Add loader object files to engine
# target_sources(engine PRIVATE $<TARGET_OBJECTS:loader>)

# engine executable is minimal: links loader + base
target_link_libraries(engine PUBLIC core)

# Put in a clear project folder
set_target_properties(engine PROPERTIES FOLDER "Exec/")
set_target_properties(engine PROPERTIES LINKER_LANGUAGE C )

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES} )

# ==========================================================

# ==========================================================
# game/CMakeLists.txt
# ==========================================================

# add_library(game SHARED
#     ecs/entity.c
#     world/world.c
#     gameplay/navmesh.c
# )
# 
# target_link_libraries(game PRIVATE
#     engine_core engine_platform engine_systems
# )

# ==========================================================
# editor/CMakeLists.txt
# ==========================================================

# add_executable(editor
#     editor_main.c
#     ui/editor_ui.c
#     tools/importer.c
# )
# 
# target_link_libraries(editor PRIVATE
#     game
#     engine_core engine_platform engine_systems
# )

# ==========================================================
# tools/CMakeLists.txt
# ==========================================================

# # Shared scaffolding for tools (optional)
# add_library(tools_core STATIC tools_common/tool_util.c)
# set_target_properties(tools_core PROPERTIES FOLDER "Tools/Common")
#
# # Build tool
# add_executable(build_tool build_tool/build_tool_main.c)
# target_link_libraries(build_tool PRIVATE tools_core engine_core engine_platform)
# set_target_properties(reflect_tool PROPERTIES FOLDER "Tools/Reflect")
#
# # Reflect tool
# add_executable(reflect_tool reflect_tool/reflect_main.c)
# target_link_libraries(reflect_tool PRIVATE tools_core engine_core engine_platform)
# 
# # Asset tool
# add_executable(asset_tool asset_tool/asset_main.c)
# target_link_libraries(asset_tool PRIVATE tools_core engine_core engine_platform engine_systems)
# 
# # Shader tool
# add_executable(shader_tool shader_tool/shader_main.c)
# target_link_libraries(shader_tool PRIVATE tools_core engine_core engine_platform # engine_systems)


