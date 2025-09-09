#pragma once
#include <stddef.h>

struct registry_api_t;

/*============================================================================================*/

// Foundation function table (the single authoritative implementation)
// Minimal example: alloc/free/log
struct base_api_t
{
    void* ( *alloc )( size_t size );
    void ( *free )( void* ptr );
    void ( *log )( const char* msg );
};

/*============================================================================================*/

// Called by loader (once) to initialize foundation and internal registry.
void base_init( void );

// Register the foundation API into the provided registry
void base_register_api( struct registry_api_t* reg );

// Convenience thin wrapper
struct base_api_t* base_get_api( void );

/*============================================================================================*/
