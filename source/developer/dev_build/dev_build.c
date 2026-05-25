/*==============================================================================================

    dev_build.c : Implementation. Spawns build_tool.exe via sys_process_run_capture.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "orb.h"
#include "engine/sys/sys_host.h"
#include "developer/dev_build/dev_build.h"

#ifndef MAX_PATH
    #define MAX_PATH 260
#endif

/*==============================================================================================
    State
==============================================================================================*/

static struct
{
    char               build_dir[ MAX_PATH ];        /* full absolute path to the repo root */
    char               build_tool_path[ MAX_PATH ];  /* full path to build_tool.exe */
    dev_build_config_t config;                        /* DEV_BUILD_DEBUG or DEV_BUILD_RELEASE */
    bool               capture_output;                /* if true, fills result->log on each build */
    bool               initialized;                   /* whether dev_build_init() has been called successfully */

} g_rt;

static char g_rt_error[ 512 ]; /* description of the last error that occurred in this module */

static void
set_error( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( g_rt_error, sizeof( g_rt_error ), fmt, ap );
    va_end( ap );
}

const char*
dev_build_last_error( void )
{
    return g_rt_error;
}

/*==============================================================================================
    PDB lock workaround
    -------------------
    When this process is being debugged and has loaded a module's shadow DLL, the
    debugger holds an open handle on the module's PDB file. The next link will fail
    with LNK1201 because it can't overwrite the PDB.

    Fix: just before invoking build_tool, rename <bin>/<target>.pdb to a unique name. The
    debugger's handle stays valid (Windows lets you rename open files), and the
    linker writes a fresh PDB at the original name. Old renamed files accumulate
    until process exit; we sweep them up on dev_build_init().
==============================================================================================*/

#if OS_WINDOWS

    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define WIN32_EXTRA_LEAN
    #define VC_EXTRALEAN
    #include <windows.h>

static unsigned g_pdb_lock_counter = 0;

static void
unlock_target_pdb( const char* target )
{
    char src[ MAX_PATH ];
    char dst[ MAX_PATH ];
    snprintf( src, sizeof( src ), "%s\\bin\\%s.pdb", g_rt.build_dir, target );
    snprintf( dst, sizeof( dst ), "%s\\bin\\%s.pdb.locked.%u", g_rt.build_dir, target, g_pdb_lock_counter++ );

    /* MoveFileA fails silently if the source doesn't exist (first build, or already
       moved by a previous attempt) -- both cases are fine, the linker will just create
       the PDB fresh. We don't need to know whether it succeeded. */
    MoveFileA( src, dst );
}

static void
purge_locked_pdbs( void )
{
    /* Best-effort cleanup of leftover .pdb.locked.* files from previous sessions.
       Files still held by an active debugger will refuse to delete; we ignore those
       and let the OS clean them up when the holding process exits. */
    char pattern[ MAX_PATH ];
    snprintf( pattern, sizeof( pattern ), "%s\\bin\\*.pdb.locked.*", g_rt.build_dir );

    WIN32_FIND_DATAA fd;
    HANDLE           find = FindFirstFileA( pattern, &fd );
    if ( find == INVALID_HANDLE_VALUE )
        return;

    do {
        char full[ MAX_PATH ];
        snprintf( full, sizeof( full ), "%s\\bin\\%s", g_rt.build_dir, fd.cFileName );
        DeleteFileA( full );
    }
    while ( FindNextFileA( find, &fd ) );

    FindClose( find );
}

#endif

/*==============================================================================================
    Build-dir auto-detection
    The host exe lives at <build_dir>/bin/<exe>.exe, so build_dir is the parent of
    the exe's directory.
==============================================================================================*/

static void
auto_detect_build_dir( char* out, int size )
{
    char exe_dir[ MAX_PATH ];
    sys_exe_dir( exe_dir, sizeof( exe_dir ) );

    /* trim trailing slashes */
    int len = ( int )strlen( exe_dir );
    while ( len > 0 && ( exe_dir[ len - 1 ] == '\\' || exe_dir[ len - 1 ] == '/' ) ) exe_dir[ --len ] = '\0';

    /* drop the final path component ("bin") */
    char* slash = strrchr( exe_dir, '\\' );
    if ( !slash )
        slash = strrchr( exe_dir, '/' );
    if ( slash )
        *slash = '\0';

    snprintf( out, ( size_t )size, "%s", exe_dir );
}

/*==============================================================================================
    Init
==============================================================================================*/

bool
dev_build_init( const dev_build_settings_t* settings )
{
    memset( &g_rt, 0, sizeof( g_rt ) );

    if ( settings && settings->build_dir && *settings->build_dir )
        snprintf( g_rt.build_dir, sizeof( g_rt.build_dir ), "%s", settings->build_dir );
    else
        auto_detect_build_dir( g_rt.build_dir, sizeof( g_rt.build_dir ) );

    /* build_tool.exe lives in bin/ under the build root; allow an override for edge cases */
    if ( settings && settings->build_tool_path && *settings->build_tool_path )
        snprintf( g_rt.build_tool_path, sizeof( g_rt.build_tool_path ), "%s", settings->build_tool_path );
    else
        snprintf( g_rt.build_tool_path, sizeof( g_rt.build_tool_path ), "%s\\bin\\build_tool.exe",
                  g_rt.build_dir );

    g_rt.config         = settings ? settings->config : DEV_BUILD_DEBUG;
    g_rt.capture_output = settings ? settings->capture_output : true;
    g_rt.initialized    = true;

    purge_locked_pdbs();

    printf( "[dev_build] init  build=%s  tool=%s  config=%s\n", g_rt.build_dir, g_rt.build_tool_path,
            g_rt.config == DEV_BUILD_RELEASE ? "Release" : "Debug" );
    return true;
}

/*==============================================================================================
    Internal: invoke build_tool.exe, populate result
==============================================================================================*/

static const char*
config_str( dev_build_config_t c )
{
    return c == DEV_BUILD_RELEASE ? "Release" : "Debug";
}

static bool
run_build_tool( const char* target_or_null, dev_build_result_t* result )
{
    if ( !g_rt.initialized )
    {
        set_error( "dev_build_init() not called" );
        return false;
    }
    if ( !result )
    {
        set_error( "result pointer is required" );
        return false;
    }
    memset( result, 0, sizeof( *result ) );

    char cmd[ 1024 ];
    if ( target_or_null )
    {
        snprintf( cmd, sizeof( cmd ), "\"%s\" -config %s -target %s", g_rt.build_tool_path,
                  config_str( g_rt.config ), target_or_null );
    }
    else
    {
        snprintf( cmd, sizeof( cmd ), "\"%s\" -config %s", g_rt.build_tool_path, config_str( g_rt.config ) );
    }

    printf( "[dev_build] %s\n", cmd );

    sys_process_result_t pr;
    bool                 launched;

    if ( g_rt.capture_output )
    {
        launched = sys_process_run_capture( cmd, NULL, result->log, ( int )sizeof( result->log ),
                                            &result->log_len, &pr );
    }
    else
    {
        launched = sys_process_run( cmd, NULL, &pr );
    }

    if ( !launched )
    {
        set_error( "failed to launch build_tool (path: '%s')", g_rt.build_tool_path );
        return false;
    }

    result->exit_code       = pr.exit_code;
    result->elapsed_seconds = pr.elapsed_seconds;
    result->success         = ( pr.exit_code == 0 );

    printf( "[dev_build] %s in %.2fs (exit %d)\n", result->success ? "OK" : "FAILED", pr.elapsed_seconds,
            pr.exit_code );
    return true;
}

/*==============================================================================================
    Public
==============================================================================================*/

bool
dev_build_module( const char* target, dev_build_result_t* result )
{
    if ( !target || !*target )
    {
        set_error( "target name is required" );
        return false;
    }

    unlock_target_pdb( target );

    return run_build_tool( target, result );
}

bool
dev_build_all( dev_build_result_t* result )
{
    return run_build_tool( NULL, result );
}

/*============================================================================================*/
