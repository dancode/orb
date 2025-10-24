# ==========================================================
# Engine : Core Library
# ==========================================================

set(CORE_SOURCES
	core.h
	core.c
	
	reflection.h		
	reflection.c
	
	module_system.c	
	
	bits.c
	
	string_pool.h
	string_pool.c
	
	cvar.h
	cvar.c
	cvar_cmd.c
	cvar_config.c
	cvar.natvis
	
	debug.c 
		
	core_api.c
	
	
)

add_library(core STATIC ${CORE_SOURCES} )
set_target_properties(core PROPERTIES FOLDER "Engine/")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CORE_SOURCES} )
target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR}/engine )

