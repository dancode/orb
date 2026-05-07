/*==============================================================================================

    core.h

==============================================================================================*/
#pragma once
#include "orb.h"
#include "cvar/cvar.h"
#include "sid/sid.h"

typedef struct core_api_s core_api_t;
/*==============================================================================================

    core.c

==============================================================================================*/

void core_init_startup( void );
void core_exit_shutdown( void );

/*==============================================================================================

    memory.c

==============================================================================================*/

enum    // Built-in system tags (fixed, global)
{
    MEMTAG_UNKNOWN = 0,    // unknown / untagged
    MEMTAG_CORE,           // core engine systems
    MEMTAG_ENGINE,         // engine systems
    MEMTAG_COUNT           // total number of built-in tags
};

typedef uint16_t memtag_t;    // runtime-generated tag ID

void             mem_tag_init( void );
void             mem_tag_exit( void );

memtag_t         mem_tag_create( const char* name );
void*            mem_tag_alloc( int32_t size, memtag_t tag );
void             mem_tag_free( void* ptr, int32_t size, memtag_t tag );

void             mem_tag_dump( void );          // diagnostics
void             mem_tag_dump_leaks( void );    // diagnostics

void             mem_test( void );

/*============================================================================================*/
