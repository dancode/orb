/*==============================================================================================

    sandbox/engine/engine_net/sb_engine_net.c -- Test sandbox for sys sockets + the net module.

==============================================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "engine/net/net_host.h"

/*==============================================================================================
    Check helper shared by all test suites
==============================================================================================*/

static int s_checks = 0;
static int s_fails  = 0;

static void
sb_check( bool ok, const char* what )
{
    s_checks++;
    if ( !ok )
    {
        s_fails++;
        printf( "    FAIL: %s\n", what );
    }
}

/*==============================================================================================
    Unity includes for the separated test suites
==============================================================================================*/

#include "sb_engine_net_socket.c"
#include "sb_engine_net_bit.c"
#include "sb_engine_net_conn.c"
#include "sb_engine_net_reliable.c"

/*==============================================================================================
   main -- Executable entry point.
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    printf( "========================================\n" );
    printf( " sys socket layer\n" );
    printf( "========================================\n" );
    net_test_socket();

    printf( "\n========================================\n" );
    printf( " bit packing\n" );
    printf( "========================================\n" );
    net_test_bit();

    printf( "\n========================================\n" );
    printf( " connections + channels\n" );
    printf( "========================================\n" );
    sys_tick_init();
    sys_net_init();
    net_test_conn();

    printf( "\n========================================\n" );
    printf( " reliability under simulated loss\n" );
    printf( "========================================\n" );
    net_test_reliable();

    sys_net_shutdown();
    sys_tick_exit();

    printf( "\n%d checks, %d failures\n", s_checks, s_fails );
    printf( s_fails == 0 ? "ALL PASS\n" : "FAILED\n" );
    return s_fails == 0 ? 0 : 1;
}

/*============================================================================================*/
