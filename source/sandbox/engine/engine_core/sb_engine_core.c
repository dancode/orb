/*==============================================================================================

    sandbox/sb_engine_core.c - For testing engine core library features.     
    
    Not a real host; just a place to call core APIs and verify they work.

==============================================================================================*/

#include <stdio.h>    // printf, fprintf

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/core/core_api.h"

int  intern_test( void );                        // ... temporary code ...
void test_core_cvar( int argc, char** argv );    // ... temporary code ...

/*============================================================================================*/
/* '+action' handlers for the bind round trip: down queues "+test <key>", up "-test <key>". */

static void
cmd_test_down( int argc, char** argv )
{
    con_printf( "+test down (key %s)\n", ( argc > 1 ) ? argv[ 1 ] : "?" );
}

static void
cmd_test_up( int argc, char** argv )
{
    con_printf( "-test up   (key %s)\n", ( argc > 1 ) ? argv[ 1 ] : "?" );
}

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

    if ( 1 )
    {
        /* Console + cvar round trip, fully headless: every con_print echoes to stdout and the
           scrollback ring fills inside core -- no gui anywhere.  con_exec is the same front-end
           submit path sb_gui_console feeds; dispatch happens in the cmd backend. */

        cvar_system_init();
        cmd_system_init();
        con_init();
        cvar_register_commands();

        cvar_register_f( "s_volume", "Sound volume", 0.8f, 0.0f, 1.0f, CVAR_ARCHIVE );
        cvar_register_b( "cl_showfps", "Show FPS counter", false, 0 );
        cvar_register_i( "com_maxfps", "Max FPS", 60, 30, 300, 0 );
        cvar_register_r( "version", "Engine version", "ORB 0.1.0", CVAR_ROM );

        con_exec( "help" );
        con_exec( "s_volume" );          // bare cvar name prints its value
        con_exec( "s_volume 0.25" );     // name + value sets
        con_exec( "s_volume banana" );   // parse failure must report, not claim success
        con_exec( "version 2.0" );       // ROM set must be rejected
        con_exec( "toggle cl_showfps" ); // bool toggle
        con_exec( "toggle com_maxfps" ); // int toggle (nonzero -> 0)
        con_exec( "set user_var 42" );   // creates a user cvar
        con_exec( "cvarlist" );
        con_exec( "echo hello console" );
        con_exec( "no_such_thing" );     // unknown command path

        /* Deferred buffer: any service can queue text; the host loop pumps once per frame. */

        cmd_queue( "echo frame one; echo still frame one // trailing comment" );
        cmd_queue( "wait" );
        cmd_queue( "echo frame two" );
        cmd_queue_front( "echo runs first" );

        cmd_pump();    // frame 1: runs first / frame one x2, stops at wait
        cmd_pump();    // frame 2: wait elapses
        cmd_pump();    // frame 3: frame two

        /* exec round trip: archive current values, change one, exec the file to restore it.
           The file text runs through the buffer, so "seta" works like any command. */

        con_exec( "writeconfig test_exec.cfg" );
        con_exec( "s_volume 0.9" );
        con_exec( "exec test_exec.cfg" );    // queues the file at the buffer front
        cmd_pump();
        con_exec( "s_volume" );              // expect 0.25 restored from the file

        /* Command-line translation: "+cmd arg..." groups become queued statements. */

        char* fake_argv[] = { "exe", "-launcher-noise", "+set", "user_var", "99", "+echo", "cmdline", "done" };
        cmd_queue_args( 8, fake_argv );
        cmd_pump();
        con_exec( "user_var" );              // expect 99

        /* Binds: key -> command string, queued through the buffer.  A tiny name table
           stands in for app_key_names() (this sandbox loads no app module). */

        static const char* key_names[] = { "none", "j", "k" };
        cmd_bind_wire_names( key_names, 3 );
        cmd_register( "+test", cmd_test_down, "Bind test action (press)" );
        cmd_register( "-test", cmd_test_up,   "Bind test action (release)" );

        con_exec( "bind j \"echo J pressed\"" );
        con_exec( "bind k +test" );
        con_exec( "bindlist" );

        cmd_bind_event( 1, true );           // j down  -> queues the echo
        cmd_bind_event( 2, true );           // k down  -> queues "+test 2"
        cmd_bind_event( 2, false );          // k up    -> queues "-test 2"
        cmd_pump();

        con_exec( "bind j" );                // query form
        con_exec( "writeconfig test_bind.cfg" );
        con_exec( "unbindall" );
        con_exec( "bindlist" );              // expect 0 binds
        con_exec( "exec test_bind.cfg" );    // restores them through the buffer
        cmd_pump();
        con_exec( "bindlist" );              // expect j + k back

        printf( "scrollback: %u lines, history: %u entries\n",
                con_line_count(), con_history_count() );

        con_exit();
        cmd_system_exit();
        cvar_system_exit();
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