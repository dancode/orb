# ==========================================================
# Engine : Core Library
# ==========================================================

set(CORE_SOURCES
	core.h
	core.c
	
	reflect/str_intern.h
	reflect/str_intern.c
	reflect/reflection.h	
	reflect/registry.c
	reflect/reflection.c
	
	module_system.c	
	
	bits.c
	
	# cvar / command system
	
	cvar/cmd_buffer.c
	cvar/cmd_parse.c
	cvar/string_pool.h
	cvar/string_pool.c	
	cvar/cvar.h
	cvar/cvar.c
	cvar/cvar_cmd.c
	cvar/cvar_config.c
	cvar/cvar.natvis
	cvar/cmd.c
	cvar/cmd.h
	
	debug.c 
		
	core_api.c
	
	test/test_core_cvar.c
)

add_library(core STATIC ${CORE_SOURCES} )
set_target_properties(core PROPERTIES FOLDER "Engine/")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CORE_SOURCES} )
target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR}/engine )

