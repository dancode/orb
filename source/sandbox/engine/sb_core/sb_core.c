/*==============================================================================================

    sandbox/engine/sb_core/sb_core.c - For testing engine core library features.     
    
    Not a real host; just a place to call core APIs and verify they work.

==============================================================================================*/

#include <stdio.h>    // printf, fprintf
#include <string.h>   // strlen, memcmp

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/core/core_host.h"    // core_crash_install / core_crash_report_now
#include "engine/sys/sys_host.h"      // sys_file_* helpers used by the console/cvar round trip

int  intern_test( void );                        // ... temporary code ...
void test_core_cvar( int argc, char** argv );    // ... temporary code ...

/*==============================================================================================
    Check helper (matches sb_net / sb_prof / sb_fs house style)
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

    Per-channel log filtering -- registry, `log` command, console intake round trip.

    Headless: ambient log entries land in the console scrollback through con_log_sink
    (con_init registers it), so scrollback line-total deltas prove exactly what passed
    the logger's filter.  The console intake floor is dropped to TRACE first so only the
    logger's own global-floor/override decision is under test.

==============================================================================================*/

/* Write one ambient log line; true if it reached the console scrollback. */

static bool
log_probe( int level, const char* channel )
{
    u32 before = con_line_total();
    core_log_fn( level, channel, "probe" );
    return con_line_total() != before;
}

static void
log_channel_test( void )
{
    printf( "\n=== log channels ===\n\n" );

    cvar_system_init();
    cmd_system_init();
    con_init();
    cvar_register_commands();
    log_register_commands();
    con_set_log_filter( LOG_LEVEL_TRACE );

    /* Unconfigured channels inherit the global floor (log_level default: info). */
    sb_check(  log_probe( ORB_LOG_INFO,  "boat" ), "info passes at global floor" );
    sb_check( !log_probe( ORB_LOG_DEBUG, "boat" ), "debug filtered at global floor" );

    /* Opening one channel below the floor leaves the others untouched. */
    con_exec( "log boat debug" );
    sb_check(  log_probe( ORB_LOG_DEBUG, "boat" ), "override opens boat to debug" );
    sb_check( !log_probe( ORB_LOG_DEBUG, "gull" ), "gull still at global floor" );

    /* off mutes everything below FATAL (FATAL itself terminates -- not probed). */
    con_exec( "log boat off" );
    sb_check( !log_probe( ORB_LOG_ERROR, "boat" ), "off mutes errors" );

    /* Per-channel reset returns to the inherited floor. */
    con_exec( "log boat reset" );
    sb_check(  log_probe( ORB_LOG_INFO,  "boat" ), "reset restores inherit (info)" );
    sb_check( !log_probe( ORB_LOG_DEBUG, "boat" ), "reset drops the debug override" );

    /* Channel names are case-insensitive, matching cvar/cmd console conventions. */
    con_exec( "log BOAT error" );
    sb_check( !log_probe( ORB_LOG_WARN,  "boat" ), "case-insensitive set (warn muted)" );
    sb_check(  log_probe( ORB_LOG_ERROR, "boat" ), "case-insensitive set (error passes)" );

    /* Setting an unseen channel pre-registers it (autoexec before first write). */
    con_exec( "log heron trace" );
    sb_check(  log_probe( ORB_LOG_TRACE, "heron" ), "pre-registered channel at trace" );

    /* Global reset clears every override at once. */
    con_exec( "log reset" );
    sb_check( !log_probe( ORB_LOG_TRACE, "heron" ), "log reset clears heron override" );
    sb_check(  log_probe( ORB_LOG_WARN,  "boat" ),  "log reset clears boat override" );

    /* View + error paths render through con_printf (echoed to stdout above). */
    con_exec( "log list" );
    con_exec( "log boat" );
    con_exec( "log nosuch" );
    con_exec( "log boat banana" );
    con_exec( "log" );

    printf( "\nlog channels: %d checks, %d failed\n", s_checks, s_fails );

    con_exit();
    cmd_system_exit();
    cvar_system_exit();
}

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

        /* Aliases: name -> command string, expanded through the buffer at dispatch time. */

        con_exec( "alias hi \"echo hello ; echo world\"" );
        con_exec( "alias" );                 // list: expect "hi"
        con_exec( "hi" );                    // queues the body, doesn't run inline
        cmd_pump();                          // now "echo hello" / "echo world" fire

        con_exec( "alias set banana" );      // must refuse: "set" is a real command
        con_exec( "alias s_volume 1" );      // must refuse: "s_volume" is a cvar
        con_exec( "unalias hi" );
        con_exec( "hi" );                    // expect "Unknown command"
        con_exec( "unalias hi" );            // expect "not aliased"

        /* Self-referential exec: must trip the per-pump budget with one clear message,
           not spend the frame re-opening the file hundreds of times. */
        con_exec( "exec test_selfref.cfg" );
        cmd_pump();

        printf( "scrollback: %u lines, history: %u entries\n",
                con_line_count(), con_history_count() );

        con_exit();
        cmd_system_exit();
        cvar_system_exit();
    }

    log_channel_test();
}

/*============================================================================================*/
/* main entry point */

int
main( int argc, char** argv )
{
    core_crash_install( NULL );

    /* Manual crash-pipeline probes: -crash faults and dies (expect .dmp/.txt next to the
       exe + report on stderr); -crash-report writes a live snapshot and keeps running. */
    if ( argc > 1 && strcmp( argv[ 1 ], "-crash" ) == 0 )
    {
        volatile int* die = NULL;
        *die              = 42;
    }
    if ( argc > 1 && strcmp( argv[ 1 ], "-crash-report" ) == 0 )
    {
        core_crash_report_now( "sb_core -crash-report probe" );
        return 0;
    }

    core_test();
    return 0;
}

/*============================================================================================*/