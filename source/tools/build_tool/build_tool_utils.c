/*==============================================================================================

    build_tool_utils.c -- Common helper utilities for the build orchestrator.

    Three independent helper families live here:
      - cmd_buf_t management   (safe append + response-file overflow spill)
      - filesystem mtime probe (drives the incremental rebuild decision)
      - per-target named mutex (cross-process serialization of same-target builds)

    All three are stateless modules: no globals, no init/shutdown, can be
    used from any thread or from any time in the build lifecycle.

==============================================================================================*/

// --- Command Buffer Management ---

/**
 * cmd_append()
 *
 * Appends a formatted string to a command buffer.
 *
 * vsnprintf() returns the length it WOULD have written, not the truncated
 * length. The old implementation added that return value to b->size directly,
 * which silently pushed b->size past CMD_BUF_MAX. The next append would then
 * compute (CMD_BUF_MAX - b->size) as a huge size_t underflow and write
 * arbitrarily far past the buffer.
 *
 * This version clamps to the remaining space and surfaces truncation via
 * the `truncated` flag so callers can spill to a response file.
 */
void
cmd_append( cmd_buf_t* b, const char* fmt, ... )
{
    // Early-out if a prior append already filled the buffer; flagging
    // truncated again is harmless and lets the caller bail without
    // continuing to format unused arguments.
    if ( b->size >= CMD_BUF_MAX ) { b->truncated = true; return; }

    size_t remaining = CMD_BUF_MAX - b->size;

    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( b->buf + b->size, remaining, fmt, args );
    va_end( args );

    // vsnprintf returns < 0 on encoding errors. We can't tell how much (if
    // any) was written, so the safe move is to treat the buffer as toast
    // and force a response-file spill on the next caller check.
    if ( written < 0 ) { b->truncated = true; return; }

    if ( ( size_t )written >= remaining )
    {
        // Did not fit. Mark truncated and pin size at the null-terminator
        // so subsequent reads see a valid C string and so the "remaining"
        // computation above can never underflow on the next append.
        b->size                  = CMD_BUF_MAX - 1;
        b->buf[ CMD_BUF_MAX - 1 ] = '\0';
        b->truncated             = true;
    }
    else
    {
        b->size += ( size_t )written;
    }
}

/**
 * cmd_spill_to_response_file()
 *
 * cmd.exe caps argument strings near 8191 chars, and CreateProcessW caps at
 * 32767. As targets gain dependencies the link line in particular grows
 * without bound. When the buffer crosses CMD_RSP_THRESHOLD (or has been
 * marked truncated), spill the argument tail to a response file so the
 * actual invocation stays short.
 *
 * cl/link/lib all consume "@file" arguments where the file contains the
 * remaining tokens separated by whitespace. Quoting and backslashes pass
 * through literally — no escaping needed for our paths.
 */
bool
cmd_spill_to_response_file( cmd_buf_t* b, const char* rsp_path )
{
    // Nothing to do if the buffer is comfortably under the shell limit and
    // hasn't been flagged truncated. The common case for small targets.
    if ( !b->truncated && b->size < CMD_RSP_THRESHOLD ) return false;

    // Find the end of the first token (the tool exe). Whatever follows that
    // whitespace boundary is the argument list we'll write to the rsp file.
    const char* args = b->buf;
    while ( *args && *args != ' ' && *args != '\t' ) ++args;
    if ( !*args ) return false;   // No args present; nothing useful to spill.

    size_t exe_len = ( size_t )( args - b->buf );
    if ( exe_len == 0 || exe_len >= 64 ) return false;   // Exe name implausibly large.

    char exe[ 64 ];
    memcpy( exe, b->buf, exe_len );
    exe[ exe_len ] = '\0';

    // Skip whitespace between exe and first arg so the rsp file starts
    // cleanly on the first real argument.
    while ( *args == ' ' || *args == '\t' ) ++args;

    FILE* f = fopen( rsp_path, "w" );
    if ( !f )
    {
        printf( "Warning: could not open response file %s\n", rsp_path );
        return false;
    }
    fputs( args, f );
    fclose( f );

    // Rewrite the buffer to the short "<exe> @<rsp_path>" form. Clear
    // truncated so the (now short) buffer is treated as valid by downstream
    // callers; cmd_append below has the room to rebuild it safely.
    b->size      = 0;
    b->truncated = false;
    cmd_append( b, "%s @%s", exe, rsp_path );
    return true;
}

/*============================================================================================*/
// --- File System Helpers ---

/**
 * build_get_mtime()
 *
 * Returns the last modification time of `path`, or 0 if the file is missing
 * or unreadable. 0 is a deliberately unambiguous "never modified" sentinel
 * — using `(out_mtime != 0)` as the "artifact exists" predicate lets the
 * up-to-date check in build_target collapse missing-output and stale-output
 * into a single rebuild branch.
 *
 * Resolution is 1 second (NTFS) or coarser; back-to-back rebuilds within
 * the same second can therefore falsely look "up to date". Not addressed
 * here; production projects tend to be slow enough that this never bites.
 */
__time64_t
build_get_mtime( const char* path )
{
    struct __stat64 s;
    if ( _stat64( path, &s ) == 0 ) return s.st_mtime;
    return 0;
}

/**
 * strip_eol()
 *
 * Trim trailing CR/LF (and any combination thereof) in place. fgets() preserves
 * the newline it consumed; callers reading paths or config lines back out need
 * a clean string for strcmp / _stat64 / etc.
 */
static void
strip_eol( char* s )
{
    size_t l = strlen( s );
    while ( l > 0 && ( s[ l - 1 ] == '\n' || s[ l - 1 ] == '\r' ) ) s[ --l ] = '\0';
}

/**
 * ensure_dir()
 *
 * Idempotent "mkdir if missing" used by every code path that needs to write
 * into bin/, the build dir, an obj dir, or the generated dir. _access() probes
 * are cheap, so the common (already-exists) case skips the system() spawn.
 */
static void
ensure_dir( const char* dir )
{
#if defined( _WIN32 )
    if ( _access( dir, 0 ) == 0 ) return;
    char cmd[ BT_PATH_MAX ];
    snprintf( cmd, sizeof( cmd ), "mkdir %s >nul 2>nul", dir );
    system( cmd );
#else
#error "build_tool only supports Windows (MSVC)"
#endif
}

/*============================================================================================*/
// --- Cross-Process Target Locking ---

/**
 * build_lock_target()
 *
 * Returns a kernel-mutex handle scoped to the named target. Blocks until the
 * mutex is granted. The name lives in the unprivileged local-session namespace
 * so any number of build_tool.exe processes in the same logon session share
 * the same mutex object for a given target.
 *
 * Failure to create the mutex is non-fatal: we return NULL and the caller
 * proceeds unlocked. Better to risk a rare collision than to refuse to build.
 */
void*
build_lock_target( const char* target_name )
{
#if defined( _WIN32 )
    char name[ 256 ];
    snprintf( name, sizeof( name ), "orb_build_tool_%s", target_name );

    HANDLE h = CreateMutexA( NULL, FALSE, name );
    if ( !h ) return NULL;

    WaitForSingleObject( h, INFINITE );
    return ( void* )h;
#else
    ( void )target_name;
    return NULL;
#endif
}

/**
 * build_unlock_target()
 *
 * Releases and closes a target lock previously returned by build_lock_target.
 */
void
build_unlock_target( void* lock )
{
#if defined( _WIN32 )
    if ( !lock ) return;
    ReleaseMutex( ( HANDLE )lock );
    CloseHandle( ( HANDLE )lock );
#else
    ( void )lock;
#endif
}

/*============================================================================================*/
// --- Target Registry Helpers ---

// Linear search through the global target pool by exact name.
// Used for internal dep resolution where names are always lowercase literals from g_targets[].
static target_info_t*
find_target( const char* name )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( strcmp( g_targets[ i ].name, name ) == 0 ) return &g_targets[ i ];
    return NULL;
}

// Case-insensitive variant for user-facing CLI lookups (target_name from argv).
static target_info_t*
find_target_icase( const char* name )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( _stricmp( g_targets[ i ].name, name ) == 0 ) return &g_targets[ i ];
    return NULL;
}

// Find the singleton target marked is_reflect_tool.
static target_info_t*
find_reflect_tool( void )
{
    for ( int i = 0; i < g_target_count; ++i )
        if ( g_targets[ i ].is_reflect_tool ) return &g_targets[ i ];
    return NULL;
}
