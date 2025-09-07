/*============================================================================================*/

#include "api_registry.h"

#include "base.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*============================================================================================*/

static void*
f_alloc( size_t s )
{
    return malloc( s );
}

static void
f_free( void* p )
{
    free( p );
}

static void
f_log( const char* m )
{
    if ( !m )
        return;

    printf( "[base] %s\n", m );
}

/*============================================================================================*/

static struct base_api_t base_api = {
    .alloc = f_alloc,
    .free  = f_free,
    .log   = f_log,
};

/*============================================================================================*/

// Accessors to the internal registry (foundation owns the storage)
void
base_init( void )
{
    // empty
}

void
base_register_api( struct api_registry* reg )
{
    if ( !reg || !reg->add )
        return;

    reg->add( "base_api", &base_api );
}

// Convenience thin wrapper
struct base_api_t*
base_get_api( void )
{
    return &base_api;
}

/*============================================================================================*/
