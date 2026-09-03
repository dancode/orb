/*==============================================================================================

    build_tool_01_prim.c -- Low-level primitives used by every other module.

    Four stateless utility families:
      - Command field append    : space-joined append into a fixed char field.
      - cmd_buf_t management    : safe append + response-file overflow spill.
      - Filesystem helpers      : mtime probe, ensure_dir.
      - Per-target named mutex  : cross-process serialization of same-target builds.

    No dependencies on any other build_tool module.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Command Field Append ---

    Append one whitespace-separated token to a fixed-size command field, inserting the
    separating space when the field is already non-empty. Every compile and link field
    that is built up entry-by-entry (flags, includes, defines, libs) goes through here.

    Overflow halts the process. A truncated compiler or linker field produces a command
    that runs and silently does the wrong thing -- a short lib list surfaces as an
    unresolved-symbol dump with no hint of its real cause -- so there is no useful way to
    continue. link_cmd_t.libs (1024 bytes) is the field that fills first: roughly thirty
    dependency paths plus the system libs.
==============================================================================================*/

void
str_append_tok( char* buf, size_t size, const char* fmt, ... )
{
    size_t used = strlen( buf );
    if ( used + 1 >= size )
    {
        printf( ORB_INDENT "[orb error] command field full (capacity %zu)"
                " -- raise the field size\n", size );
        exit( 1 );
    }

    if ( used )
        buf[ used++ ] = ' ';

    size_t remaining = size - used;

    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( buf + used, remaining, fmt, args );
    va_end( args );

    if ( written < 0 || ( size_t )written >= remaining )
    {
        printf( ORB_INDENT "[orb error] command field truncated (needed %d, had %zu)"
                " -- raise the field size\n", written, remaining );
        exit( 1 );
    }
}

/*==============================================================================================
    --- Command Field Overflow Probe ---

    Report a command field that ends exactly at its capacity and halt.

    str_append_tok() and cc_field() abort on their own truncation, but the whole-field
    snprintf writes (exe names, output paths, obj globs) do not: snprintf always
    NUL-terminates, so a lost tail leaves no trace except that the field is now exactly
    full. That is the only detectable signature, and a field content that happens to land
    on the capacity boundary intact is a near miss worth stopping for anyway.

    owner/name identify the field in the message so the fix is one struct edit away.
==============================================================================================*/

void
cmd_field_check_full( const char* owner, const char* name, const char* buf, size_t size )
{
    if ( strlen( buf ) != size - 1 )
        return;

    printf( ORB_INDENT "[orb error] %s.%s filled its %zu-byte capacity -- content was"
            " truncated; raise the field size\n", owner, name, size );
    exit( 1 );
}

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

    size_t remaining = CMD_BUF_WORK_MAX - b->size;

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
    --- Command Response File Spill ---

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
    --- Path Helpers ---
==============================================================================================*/

/*  Resolve in to an absolute path, falling back to a verbatim copy when resolution fails.

    The fallback is not decoration. platform_fullpath is _fullpath on Windows, which is
    pure string work and succeeds for a path that does not exist yet; on POSIX it is
    realpath, which fails outright unless every component already exists. Build paths are
    routinely named before they are created (an obj dir, a generated .c, a cooked asset),
    so on POSIX this branch is the common case, and keeping the unresolved spelling is
    what lets the caller go on to create the thing. */

void
path_abs( char* out, const char* in, size_t size )
{
    if ( !platform_fullpath( out, in, size ) )
        snprintf( out, size, "%s", in );
}

/*  Convert backslashes to forward slashes in place.

    Everything that leaves the build tool as text -- JSON, vcxproj, VS filter paths, the
    generated orb.targets -- wants forward slashes: cl.exe and MSBuild accept both, and
    JSON would otherwise need every separator double-escaped. Paths coming back from
    platform_fullpath on Windows are backslashed, so this normalizes at the boundary.

    There is no in-place inverse here: normalize_slashes() in build_tool_12_gen_nmake.c
    goes the other way for NMake recipe text and has no second caller to share with. */

void
path_to_fwd( char* s )
{
    for ( ; *s; ++s )
        if ( *s == '\\' ) *s = '/';
}

/*  True when two paths name the same location, comparing the spellings only -- no
    filesystem access, no symlink resolution, no relative-to-absolute step. Callers that
    need those must resolve through path_abs() first.

    Separator kind is ignored ('\' == '/') and trailing separators are ignored, so
    "C:\proj\game", "C:/proj/game" and "C:/proj/game/" all compare equal. Case is folded
    on Windows only: on POSIX two paths differing in case are two different files.

    Streams both sides rather than normalizing into buffers, so an over-long path cannot
    be silently truncated into a false match. */

static bool
path_eq( const char* a, const char* b )
{
    for ( ;; )
    {
        bool sep_a = ( *a == '/' || *a == '\\' );
        bool sep_b = ( *b == '/' || *b == '\\' );

        if ( sep_a || sep_b )
        {
            /* Collapse each run of separators to one. */
            while ( *a == '/' || *a == '\\' ) ++a;
            while ( *b == '/' || *b == '\\' ) ++b;

            /* A run that ends the string is a trailing separator: ignore it on
               either side. Otherwise both sides must have had one here. */
            if ( !*a && !*b )         return true;
            if ( !sep_a || !sep_b )   return false;
            continue;
        }

        if ( !*a || !*b ) return *a == *b;

        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
#if defined( _WIN32 )
        if ( ca >= 'A' && ca <= 'Z' ) ca = (unsigned char)( ca + 32 );
        if ( cb >= 'A' && cb <= 'Z' ) cb = (unsigned char)( cb + 32 );
#endif
        if ( ca != cb ) return false;
    }
}

/*==============================================================================================
    --- Filesystem Helpers ---
==============================================================================================*/

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

// Advance *p by one line from the mapped view [*p, end), stripping trailing CR.
// Writes the line into buf (clamped to buf_size-1) and NUL-terminates. Returns false
// when the view is exhausted. Handles NULL *p or NULL end (empty/missing file).

static bool
mmap_next_line( const char** p, const char* end, char* buf, size_t buf_size )
{
    if ( !*p || *p >= end ) return false;
    const char* nl  = (const char*)memchr( *p, '\n', (size_t)( end - *p ) );
    size_t      len = nl ? (size_t)( nl - *p ) : (size_t)( end - *p );
    if ( len > 0 && (*p)[ len - 1 ] == '\r' ) len--;
    if ( len >= buf_size ) len = buf_size - 1;
    memcpy( buf, *p, len );
    buf[ len ] = '\0';
    *p = nl ? nl + 1 : end;
    return true;
}

/*==============================================================================================
    --- Named Mutex ---

    Serializes concurrent build_tool.exe invocations that write the same artifact.
    The mutex lives in the unprivileged local-session namespace so any number of
    processes in the same logon session share the same mutex object.
    Failure is non-fatal: the caller proceeds unlocked rather than refusing to build.

    Usually keyed by target name, but the key is really the OUTPUT.  Two targets that share a
    source tree can declare the same generated file, and those must serialize against each other
    rather than against everything else their target builds -- see build_cook_content.
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
