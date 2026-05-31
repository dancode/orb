/*==============================================================================================

    build_tool_test.c -- Debug frontend for stepping through build_tool.exe in VS.

    Pass -custom_args in the VS debugger command-line settings to activate.
    When active, argc/argv are replaced with the args hardcoded below before
    main() parses flags.  The rest of main() runs identically to a real invocation.

    To change the debug args: edit DEBUG_CUSTOM_ARGS below and rebuild.
    To disable without recompiling: remove -custom_args from VS debug settings.

    Compiled out entirely when BUILD_TOOL_NO_DEBUG_INJECT is defined.

==============================================================================================*/
// clang-format off

#if !defined( BUILD_TOOL_NO_DEBUG_INJECT )

// --- Edit these to set the args used when -custom_args is passed. ---
static const char* DEBUG_CUSTOM_ARGS[] = {
    "-config", "Debug",
    "-target",  "core",
};
// ---

#define DEBUG_ARGS_MAX  ( sizeof( DEBUG_CUSTOM_ARGS ) / sizeof( DEBUG_CUSTOM_ARGS[ 0 ] ) )

static char* s_debug_argv[ DEBUG_ARGS_MAX + 2 ];   // +1 for argv[0], +1 for NULL sentinel
static int   s_debug_argc = 0;

static void
build_tool_debug_inject( int* argc, char*** argv )
{
    // Scan for -custom_args anywhere in the command line.
    int found = 0;
    for ( int i = 1; i < *argc; i++ )
    {
        if ( strcmp( ( *argv )[ i ], "-custom_args" ) == 0 )
        {
            found = 1;
            break;
        }
    }
    if ( !found ) return;

    // Replace argc/argv with the hardcoded args above.
    s_debug_argv[ 0 ] = ( *argv )[ 0 ];   // keep argv[0] (exe path)
    s_debug_argc      = 1;

    for ( int i = 0; i < (int)DEBUG_ARGS_MAX; i++ )
        s_debug_argv[ s_debug_argc++ ] = (char*)DEBUG_CUSTOM_ARGS[ i ];

    s_debug_argv[ s_debug_argc ] = NULL;

    printf( ORB_INDENT "[orb debug] -custom_args active: %d hardcoded arg(s)\n",
            s_debug_argc - 1 );

    *argc = s_debug_argc;
    *argv = s_debug_argv;
}

#else   // BUILD_TOOL_NO_DEBUG_INJECT

static void build_tool_debug_inject( int* argc, char*** argv ) { (void)argc; (void)argv; }

#endif  // BUILD_TOOL_NO_DEBUG_INJECT

// clang-format on
/*============================================================================================*/
