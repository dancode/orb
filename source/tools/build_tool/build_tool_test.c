/*==============================================================================================

    build_tool_test.c -- Debug frontend for stepping through build_tool.exe in VS.

    Reads args from "build_tool_debug.args" in the working directory at startup.
    If the file exists, its contents replace argc/argv before main() parses flags.
    The rest of main() runs identically to a real invocation -- same parsing path,
    same dispatch -- so you can set a breakpoint anywhere and step through normally.

    Usage:
        1. Create "build_tool_debug.args" next to build_tool.exe (see template below).
        2. Put one argument token per line. Lines starting with '#' are comments.
        3. Run build_tool.exe under the debugger (F5). No command-line setup needed.
        4. Edit the file freely between runs -- no recompile required.
        5. Delete the file (or rename it) to restore normal behavior.

    File format example (build_tool_debug.args):
        # target + config
        -config
        Debug
        -target
        core
        -compile-only

    Compiled out entirely in Release builds (_DEBUG not defined).

==============================================================================================*/
// clang-format off

#if defined( _DEBUG ) && !defined( BUILD_TOOL_NO_DEBUG_INJECT )

#define DEBUG_ARGS_FILE  "build_tool_debug.args"
#define DEBUG_ARGS_MAX   32
#define DEBUG_ARG_LEN    512

static char* s_debug_argv[ DEBUG_ARGS_MAX + 2 ];   // +1 for argv[0], +1 for NULL sentinel
static int   s_debug_argc = 0;
static char  s_debug_arg_storage[ DEBUG_ARGS_MAX ][ DEBUG_ARG_LEN ];

static void
build_tool_debug_inject( int* argc, char*** argv )
{
    // Only inject when launched with no arguments (F5 from VS without a command line
    // configured). Real NMake invocations supply their own args -- don't clobber them.
    if ( *argc > 1 ) return;

    FILE* f = fopen( DEBUG_ARGS_FILE, "r" );
    if ( !f ) return;

    // argv[0] is always the exe name.
    s_debug_argv[ 0 ] = (char*)"build_tool.exe";
    s_debug_argc      = 1;

    char line[ DEBUG_ARG_LEN ];
    int  slot = 0;
    while ( fgets( line, sizeof( line ), f ) && slot < DEBUG_ARGS_MAX )
    {
        // Strip CR/LF.
        char* nl = strpbrk( line, "\r\n" );
        if ( nl ) *nl = '\0';

        // Skip blank lines and comments.
        if ( line[ 0 ] == '\0' || line[ 0 ] == '#' ) continue;

        strncpy( s_debug_arg_storage[ slot ], line, DEBUG_ARG_LEN - 1 );
        s_debug_arg_storage[ slot ][ DEBUG_ARG_LEN - 1 ] = '\0';
        s_debug_argv[ s_debug_argc++ ] = s_debug_arg_storage[ slot++ ];
    }

    s_debug_argv[ s_debug_argc ] = NULL;
    fclose( f );

    printf( ORB_INDENT "[orb debug] override: %d arg(s) from " DEBUG_ARGS_FILE "\n",
            s_debug_argc - 1 );

    *argc = s_debug_argc;
    *argv = s_debug_argv;
}

#else   // !_DEBUG

static void build_tool_debug_inject( int* argc, char*** argv ) { (void)argc; (void)argv; }

#endif  // _DEBUG

// clang-format on
/*============================================================================================*/
