
# create hreader only library that is viewable in solution

file(GLOB_RECURSE COMMON_FILES "*.h")
add_custom_target( com SOURCES ${COMMON_FILES})
set_target_properties(com PROPERTIES  FOLDER "Common" PROJECT_LABEL "common")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES	
 	${COMMON_FILES}	
)
