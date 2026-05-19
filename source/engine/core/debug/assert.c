/*==============================================================================================

    engine/core/debug/assert.c

    Full-featured assertion handler for the core module.

    Included by core_debug.c as part of the core unity build. All functions are
    static within that translation unit except core_assert_set_skip, which must
    be callable from test code linked to the host binary.

==============================================================================================*/

/*  Forward-declare OutputDebugStringA without pulling in windows.h.
    On x64 Windows, __stdcall is a no-op but we keep it for correctness. */

#if OS_WINDOWS
    extern __declspec( dllimport ) void __stdcall OutputDebugStringA( const char* lpOutputString );
#else
    #error "assert.c: OutputDebugString not implemented for this platform"
#endif

/*==============================================================================================
    State
==============================================================================================*/

static bool g_assert_skip = false;

/*==============================================================================================
    Skip mode
==============================================================================================*/

void
core_assert_set_skip( bool skip )
{
    g_assert_skip = skip;
}

/*==============================================================================================
    Report handler
==============================================================================================*/

/*  Called by ORB_ASSERT / ORB_ASSERT_MSG on failure.
    Returns true to skip the ORB_TRAP() (skip mode), false to trap. */

ORB_NOINLINE static bool
assert_report( const char* cond, const char* msg, const char* func, const char* file, int line )
{
    char buf[1024];

    /* VS Output window clickable format: file(line): text */

    if ( msg && msg[0] )
        snprintf( buf, sizeof( buf ),
                  "\n%s(%d): assertion failed: %s\n"
                  "    FUNCTION: %s : %s\n\n",
                  file, line, cond, func, msg );
    else
        snprintf( buf, sizeof( buf ),
                  "\n%s(%d): assertion failed: %s\n"
                  "    FNUCTION: %s\n\n",
                  file, line, cond, func );

    fputs( buf, stderr );
    OutputDebugStringA( buf );

    return g_assert_skip;
}

/*============================================================================================*/
