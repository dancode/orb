# ==========================================================
# projects/demo_game/CMakeLists.txt
# ==========================================================

set(SAMPLE_GAME_SOURCES
	sample_game.c
	sample_game.h
)

add_library(sample_game SHARED
    ${SAMPLE_GAME_SOURCES}
)
 
target_include_directories(sample_game PUBLIC
	${CMAKE_SOURCE_DIR}/engine
)

# Put in a clear project folder
set_target_properties(sample_game PROPERTIES FOLDER "Proj/Sample_Game")

# IMPORTANT: Explicitly mark as C language for Clang-CL
set_target_properties(sample_game PROPERTIES LINKER_LANGUAGE C )

# Set the output name to avoid prefix 'lib' on Unix-like systems
set_target_properties(sample_game PROPERTIES OUTPUT_NAME "sample_game")

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SAMPLE_GAME_SOURCES} )

# target_link_libraries(demo_game PRIVATE game)