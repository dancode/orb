/*==============================================================================================

    build_tool_01_prim.c -- Low-level primitives used by every other module.

    Three stateless utility families:
      - cmd_buf_t management   : safe append + response-file overflow spill.
      - Filesystem helpers     : mtime probe, ensure_dir.
      - Per-target named mutex : cross-process serialization of same-target builds.

    No dependencies on any other build_tool module.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Command Buffer ---

    Safe append into a fixed-size command-line buffer. When the buffer approaches
    CMD_BUF_WORK_MAX the truncated flag is set so the caller can spill to a
    response file rather than silently passing a truncated command to the shell.
==============================================================================================*/

void
cmd_append( cmd_buf_t* b, const char* fmt, ... )
{
    if ( b->size >= CMD_BUF_WORK_MAX )
    {
        b->truncated = true;
        return;
    }

    size_t  remaining = CMD_BUF_WORK_MAX - b->size;

    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( b->buf + b->size, remaining, fmt, args );
    va_end( args );

    if ( written < 0 )
    {
        b->truncated = true;
        return;
    }

    if ( ( size_t )written >= remaining )
    {
        b->size                        = CMD_BUF_WORK_MAX - 1;
        b->buf[ CMD_BUF_WORK_MAX - 1 ] = '\0';
        b->truncated                   = true;
    }
    else
    {
        b->size += ( size_t )written;
    }
}

/*==============================================================================================
    --- Response File Spill ---

    When the assembled command line would exceed the shell limit (~8191 chars),
    spill everything after the exe token to a response file and rewrite the
    buffer to "<exe> @<rsp_path>". cl/link/lib all accept "@file" arguments.
==============================================================================================*/

bool
cmd_spill_to_response_file( cmd_buf_t* b, const char* rsp_path )
{
    if ( !g_use_rsp ) return false;
    if ( !b->truncated && b->size < CMD_RSP_THRESHOLD )
        return false;

    // Split the buffer into "<exe>" + "<everything else>" at the first space.
    const char* args = b->buf;
    while ( *args && *args != ' ' && *args != '\t' ) ++args;
    if ( !*args )
        return false;

    size_t exe_len = ( size_t )( args - b->buf );
    if ( exe_len == 0 || exe_len >= 64 )
        return false;

    char exe[ 64 ];
    memcpy( exe, b->buf, exe_len );
    exe[ exe_len ] = '\0';

    while ( *args == ' ' || *args == '\t' ) ++args;

    FILE* f = fopen( rsp_path, "w" );
    if ( !f )
    {
        printf( "Warning: could not open response file %s\n", rsp_path );
        return false;
    }
    fputs( args, f );
    fclose( f );

    b->size      = 0;
    b->truncated = false;
    cmd_append( b, "%s @%s", exe, rsp_path );
    return true;
}

/*==============================================================================================
    --- Filesystem Helpers ---
==============================================================================================*/

/* Returns the last-modified time of a file, or 0 if missing/unreadable.
   Used as the "artifact exists" predicate: mtime 0 == needs rebuild. */

platform_mtime_t
build_get_mtime( const char* path )
{
    return platform_get_mtime( path );
}

/* Create a directory and all missing parent components (mkdir -p semantics).
   platform_mkdir is a no-op on existing directories so redundant calls are safe. */

static void
ensure_dir( const char* dir )
{
    char tmp[ PATH_MAX ];
    snprintf( tmp, sizeof( tmp ), "%s", dir );

    for ( char* p = tmp + 1; *p; ++p )
    {
        if ( *p == '/' || *p == '\\' )
        {
            char saved = *p;
            *p = '\0';
            platform_mkdir( tmp );
            *p = saved;
        }
    }
    platform_mkdir( tmp );
}

/*==============================================================================================
    --- Per-Target Named Mutex ---

    Serializes concurrent build_tool.exe invocations that target the same artifact.
    The mutex lives in the unprivileged local-session namespace so any number of
    processes in the same logon session share the same mutex object.
    Failure is non-fatal: the caller proceeds unlocked rather than refusing to build.
==============================================================================================*/

void*
build_lock_target( const char* target_name )
{
    char name[ 256 ];
    snprintf( name, sizeof( name ), "orb_build_tool_%s", target_name );
    return platform_lock_create( name );
}

void
build_unlock_target( void* lock )
{
    platform_lock_release( lock );
}

// clang-format on
/*============================================================================================*/
