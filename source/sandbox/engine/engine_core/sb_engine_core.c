/*==============================================================================================

    sandbox/sb_engine_core.c - For testing engine core library features.     
    
    Not a real host; just a place to call core APIs and verify they work.

==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/core/core.h"

int  intern_test( void );                        // ... temporary code ...
void test_core_cvar( int argc, char** argv );    // ... temporary code ...

/*============================================================================================*/

void
core_test( void )
{    
    // intern_test();    // <-- test string interning system
   
    // sid_init();
    // sid_t a = sid_intern_cstr( "Hello, World!" );
    // sid_exit();
    // UNUSED( a );

    if ( 1 )
    {
        /// mem_test();           // <-- test memory system
        
    }

    if ( 0 )
    {
        /// int argc = 0; char** argv = NULL;
        /// cvar_system_init();
        /// test_core_cvar( argc, argv );    // <-- test cvar system
        /// cvar_system_exit();
    }
    
}

/*============================================================================================*/
/* main entry point */

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );
    core_test();
    return 0;
}

/*============================================================================================*/