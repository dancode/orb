# ==========================================================
# ORB Engine
# ==========================================================

add_subdirectory(base)
add_subdirectory(sys)
add_subdirectory(module)
add_subdirectory(core)
add_subdirectory(app)
add_subdirectory(runtime/modules/example)
add_subdirectory(runtime/modules/render)
add_subdirectory(runtime/modules/audio)
add_subdirectory(game)

# ==========================================================
# Engine : Host Files
# ==========================================================

# Collect all .c/.h/.natvis etc for IDE visibility
file(GLOB ALL_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.natvis"
	# "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.c"
    # "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/runtime/services/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/services/*.h"
)

# Select which .c files should ACTUALLY compile
# Everything else is HEADER_FILE_ONLY and treated as "unity fragments"
set(COMPILED_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/engine_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine_api.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/engine_test.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/services/jobs/job_system.c"
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
# Engine : Host Executbale
# ----------------------------------------------------------

add_executable(engine
    ${ALL_FILES}
)

target_include_directories(engine PUBLIC
	${CMAKE_SOURCE_DIR}/engine
)

# ----------------------------------------------------------
# Engine : Link Libarries Host Links
# ----------------------------------------------------------

# Add loader object files to engine
# target_sources(engine PRIVATE $<TARGET_OBJECTS:loader>)

# engine executable is minimal: links loader + base
target_link_libraries(engine PUBLIC sys)
target_link_libraries(engine PUBLIC module)
target_link_libraries(engine PUBLIC core)
target_link_libraries(engine PUBLIC app)

# ----------------------------------------------------------
# 2. CONDITIONAL LINKING LOGIC AT THE BOTTOM
# ----------------------------------------------------------

if(ENGINE_MONOLITHIC)
    # In monolithic mode, the engine must statically link the modules.
    target_link_libraries(engine PUBLIC example)
	target_link_libraries(engine PUBLIC render)
	target_link_libraries(engine PUBLIC audio)   	    
	target_link_libraries(engine PUBLIC game)   	    
	target_link_libraries(engine PUBLIC sample_game)   	    
else()
    # In dynamic mode, do NOT link the render target (load at runtime)
endif()


# ----------------------------------------------------------
# Engine : Custom Solution Direction In Visual Studio
# ----------------------------------------------------------

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
# Services Folder
# ==========================================================

set(SERVICES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime/services")
file(GLOB_RECURSE SERVICES_FILES "${SERVICES_DIR}/*")
source_group(TREE "${SERVICES_DIR}" PREFIX "Runtime/Services" FILES ${SERVICES_FILES})
target_sources(engine PRIVATE ${SERVICES_FILES})
# set_source_files_properties(${RUNTIME_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)

# ==========================================================
# Runtime Folder
# ==========================================================

#set(RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
#file(GLOB_RECURSE RUNTIME_FILES "${RUNTIME_DIR}/*")
#source_group(TREE "${RUNTIME_DIR}" PREFIX "Runtime" FILES ${RUNTIME_FILES})
#target_sources(engine PRIVATE ${RUNTIME_FILES})
## set_source_files_properties(${RUNTIME_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)


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
#     engine_core engine_platform engine_runtime
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
#     engine_core engine_platform engine_runtime
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
# target_link_libraries(asset_tool PRIVATE tools_core engine_core engine_platform engine_runtime)
# 
# # Shader tool
# add_executable(shader_tool shader_tool/shader_main.c)
# target_link_libraries(shader_tool PRIVATE tools_core engine_core engine_platform # engine_runtime)


