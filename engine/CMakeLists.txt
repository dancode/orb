# ==========================================================
# engine/CMakeLists.txt
# ==========================================================

add_subdirectory(core)
add_subdirectory(platform)

add_subdirectory(systems/render)

# add_library( render )
# add_library(engine_systems STATIC
#     systems/render/renderer.c
#     systems/audio/mixer.c
#     systems/physics/physics.c
# )

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
# set(ENGINE_SOURCES
# 	orb.h
# 	
#   engine_main.c
# 	engine.h
# 	engine.c
# 		
# 	engine_api.h
# 	engine_api.c
# )

# ==========================================================
# Engine : Core Library
# ==========================================================

# Collect all .c/.h/.natvis etc for IDE visibility
file(GLOB ALL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.natvis"
	"${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.h"
)

# Select which .c files should ACTUALLY compile
# Everything else is HEADER_FILE_ONLY and treated as "unity fragments"
set(COMPILED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/engine_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine_api.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/runtime/runtime.c"
)


# ----------------------------------------------------------
# Unity Module
# ----------------------------------------------------------

# Compute list of all other .c files (excluding compiled sources)
set(HEADER_SOURCES ${ALL_FILES})
list(REMOVE_ITEM HEADER_SOURCES ${COMPILED_SOURCES})

# Mark all other .c files as header-only (not compiled)
# Explicitly allow compiled files to compile normally
set_source_files_properties( ${HEADER_SOURCES} PROPERTIES HEADER_FILE_ONLY ON )
set_source_files_properties( ${COMPILED_SOURCES} PROPERTIES HEADER_FILE_ONLY OFF )

# ----------------------------------------------------------
# Executbale
# ----------------------------------------------------------

#add_executable(engine
#    ${ENGINE_SOURCES}
#)

add_executable(engine
    ${ALL_FILES}
)

target_include_directories(engine PUBLIC
	${CMAKE_SOURCE_DIR}/engine
)

# Add loader object files to engine
# target_sources(engine PRIVATE $<TARGET_OBJECTS:loader>)

# engine executable is minimal: links loader + base
target_link_libraries(engine PUBLIC core)
target_link_libraries(engine PUBLIC platform)

# Put in a clear project folder
set_target_properties(engine PROPERTIES FOLDER "Exec/")
set_target_properties(engine PROPERTIES LINKER_LANGUAGE C )

if(UNIX)
    set_target_properties(engine PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
    )
endif()

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
# source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ENGINE_SOURCES} )

# Mirror physical directory structure inside Visual Studio
source_group(
    TREE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${ALL_FILES}
)


# ==========================================================
# Create Base folder for Unity include standard library
# ==========================================================

# 1. Define the directory and files
set(VISUAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/base")
file(GLOB_RECURSE VISUAL_FILES "${VISUAL_DIR}/*")

# 2. Create the visual hierarchy in Visual Studio Solution Explorer
source_group(TREE "${VISUAL_DIR}" PREFIX "Base" FILES ${VISUAL_FILES})

# 3. Add to your target and mark as non-compiling
# Note: 'my_project' is your existing add_executable or add_library target
target_sources(engine PRIVATE ${VISUAL_FILES})

# 4. Force Visual Studio to treat these as non-buildable items
set_source_files_properties(${VISUAL_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)


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
# set_target_properties(build_tool PROPERTIES FOLDER "Tools/Build")
#
# # Reflect tool
# add_executable(reflect_tool reflect_tool/reflect_main.c)
# target_link_libraries(reflect_tool PRIVATE tools_core engine_core engine_platform)
# set_target_properties(reflect_tool PROPERTIES FOLDER "Tools/Reflect")
# 
# # Asset tool
# add_executable(asset_tool asset_tool/asset_main.c)
# target_link_libraries(asset_tool PRIVATE tools_core engine_core engine_platform engine_systems)
# 
# # Shader tool
# add_executable(shader_tool shader_tool/shader_main.c)
# target_link_libraries(shader_tool PRIVATE tools_core engine_core engine_platform # engine_systems)


