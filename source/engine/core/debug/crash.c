/*==============================================================================================

    engine/core/debug/crash.c

    Crash report policy: filenames, report text, output routing. The OS mechanism
    (SEH filter, minidump, backtrace, symbols) lives in engine/sys/win/win_crash.c.

    Included by core_debug.c as part of the core unity build. Runs on the faulting
    thread: all working memory is static because the crashed stack may be nearly
    exhausted (EXCEPTION_STACK_OVERFLOW) and the heap cannot be trusted.

==============================================================================================*/

#include "engine/sys/sys_host.h" /* sys_crash_*, sys_exe_dir, sys_datetime_local, ... */

/*==============================================================================================
    State
==============================================================================================*/

#define CRASH_MAX_FRAMES 64

static char         s_crash_dir[ 512 ];       // report output directory, no trailing slash
static char         s_crash_report[ 8192 ];   // formatted report text
static void*        s_crash_frames[ CRASH_MAX_FRAMES ];
static volatile i32 s_crash_lock;             // serializes snapshot calls against each other

/*==============================================================================================
    Report assembly
==============================================================================================*/

/* Bounded append into the report buffer; returns the new write cursor, saturating at end. */
static char*
crash_append( char* p, char* end, const char* fmt, ... )
{
    if ( p >= end - 1 )
        return p;
    va_list args;
    va_start( args, fmt );
    int n = vsnprintf( p, ( size_t )( end - p ), fmt, args );
    va_end( args );
    if ( n < 0 )
        return p;
    return ( n < end - p ) ? p + n : end - 1;
}

/*  Write <dir>/crash_<stamp>_<pid>.dmp + .txt and route the report to stderr and the
    debugger. `info` is non-NULL on the fatal path, NULL for live snapshots. */
static void
crash_capture( const sys_crash_info_t* info, const char* reason )
{
    if ( !s_crash_dir[ 0 ] )
        sys_exe_dir( s_crash_dir, sizeof( s_crash_dir ) );

    SysDateTime dt;
    sys_datetime_local( &dt );

    char base[ 600 ];
    snprintf( base, sizeof( base ), "%s\\crash_%04u%02u%02u_%02u%02u%02u_%u", s_crash_dir, dt.year,
              dt.month, dt.day, dt.hour, dt.minute, dt.second, sys_process_id() );

    char dmp_path[ 640 ];
    char txt_path[ 640 ];
    snprintf( dmp_path, sizeof( dmp_path ), "%s.dmp", base );
    snprintf( txt_path, sizeof( txt_path ), "%s.txt", base );

    bool dumped = sys_crash_minidump( dmp_path, info );

    char* p   = s_crash_report;
    char* end = s_crash_report + sizeof( s_crash_report );

    p = crash_append( p, end, "\n=============================== ORB CRASH ===============================\n" );
    if ( info )
        p = crash_append( p, end, "%s (0x%08X) at 0x%p\n", sys_crash_code_str( info->code ),
                          info->code, info->address );
    else
        p = crash_append( p, end, "SNAPSHOT: %s\n", reason );
    p = crash_append( p, end, "minidump: %s%s\n\n", dmp_path, dumped ? "" : "  (WRITE FAILED)" );

    int count = sys_crash_backtrace( info, s_crash_frames, CRASH_MAX_FRAMES );
    for ( int i = 0; i < count; ++i )
    {
        char sym[ 512 ];
        sys_crash_symbolize( s_crash_frames[ i ], sym, sizeof( sym ) );
        p = crash_append( p, end, "  [%2d] %s\n", i, sym );
    }
    p = crash_append( p, end, "==========================================================================\n" );

    sys_file_write_entire( txt_path, s_crash_report, ( u32 )( p - s_crash_report ) );

    fputs( s_crash_report, stderr );
    OutputDebugStringA( s_crash_report );
}

/*==============================================================================================
    Public entry points
==============================================================================================*/

static void
crash_handler( const sys_crash_info_t* info, void* user )
{
    UNUSED( user );
    /* Fatal path: take the lock (waiting out any in-flight snapshot) and never release --
       the process dies when this returns. */
    while ( sys_atomic_compare_exchange( &s_crash_lock, 1, 0 ) != 0 )
        thread_yield();
    crash_capture( info, NULL );
}

void /* public */
core_crash_install( const char* report_dir )
{
    if ( report_dir && report_dir[ 0 ] )
        snprintf( s_crash_dir, sizeof( s_crash_dir ), "%s", report_dir );
    else
        sys_exe_dir( s_crash_dir, sizeof( s_crash_dir ) );

    sys_crash_install( crash_handler, NULL );
}

void /* public */
core_crash_report_now( const char* reason )
{
    while ( sys_atomic_compare_exchange( &s_crash_lock, 1, 0 ) != 0 )
        thread_yield();
    crash_capture( NULL, ( reason && reason[ 0 ] ) ? reason : "unspecified" );
    sys_atomic_write( &s_crash_lock, 0 );
}

/*============================================================================================*/
