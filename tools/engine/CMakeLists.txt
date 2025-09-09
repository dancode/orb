add_executable(engine
    engine_main.c
)

target_include_directories(engine PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/loader/include
    ${CMAKE_SOURCE_DIR}/engine/base/include
)

# message(STATUS "CMAKE_SOURCE_DIR = ${CMAKE_SOURCE_DIR}")

# Add loader object files to engine
# target_sources(engine PRIVATE $<TARGET_OBJECTS:loader>)

# engine executable is minimal: links loader + base
target_link_libraries(engine PRIVATE common loader base)

# Put in a clear project folder
set_target_properties(engine PROPERTIES FOLDER "Tools/")

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    engine_main.c
)
