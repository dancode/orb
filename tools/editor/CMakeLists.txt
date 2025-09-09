add_executable(editor
    editor_main.c
)

target_include_directories(editor PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/loader/include
    ${CMAKE_SOURCE_DIR}/engine/base/include
)

# Add loader object files to editor
# target_sources(editor PRIVATE $<TARGET_OBJECTS:loader>)

# editor executable is minimal: links loader + base
target_link_libraries(editor PRIVATE common loader base)

# Put in a clear project folder
set_target_properties(editor PROPERTIES FOLDER "Tools/")

# Remove the default "Header Files" / "Source Files" filters in Visual Studio
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES
    editor_main.c
)
