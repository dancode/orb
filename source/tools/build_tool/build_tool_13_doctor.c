/*==============================================================================================

    build_tool_13_doctor.c -- -doctor command: read-only build-setup diagnosis.

    Runs every check and reports [ ok ] / [warn] / [FAIL] lines, each failure with a
    one-line fix hint, instead of the fail-fast behavior of a normal build. Aggregates
    checks that already exist scattered across the tool (validate_targets, vcvars
    discovery) and adds environment, filesystem, and child-project wiring checks that
    nothing else runs. The goal: anyone can self-serve "why doesn't my setup build".

    Check groups:
        environment    -- cl.exe reachability, BUILD_SAFE_MODE / vcvars state, msbuild
        registry       -- orb.targets parse, validate_targets, dep cycles, run lines,
                          target pool headroom
        module descs   -- each mod_desc_t's .deps/.dep_count vs the target's orb.targets
                          'dep' lines (runtime init-order metadata vs link metadata drift)
        filesystem     -- include_dir existence, generated-solution freshness,
                          run-line exe presence, bin writability, reflect_tool
        child wiring   -- engine root + engine build_tool.exe, .orb_engine agreement,
                          bin\build_tool.bat forwarder (only when 'engine' is declared)

    Dispatched after registry_load + init_builtin_targets but BEFORE the registry
    bail-out in main() -- a broken orb.targets is exactly what doctor diagnoses.

    Read-only: never imports vcvars, never creates directories, never compiles.
    (Exception: the bin writability probe writes one temp file and deletes it.)

    Exit code: 0 when no [FAIL] checks fired; 1 otherwise. Warnings do not fail.

    Included after 13_query.c: reuses deps_topo_t / deps_visit for cycle detection.

==============================================================================================*/
// clang-format off

#include <stdarg.h>

/*==============================================================================================
    --- Result counters + line printers ---

    Every check ends in exactly one doc_ok / doc_warn / doc_fail call; doc_fix adds an
    indented hint line under the preceding result. The counters drive the summary and
    the process exit code.
==============================================================================================*/

static int s_doc_ok_count;
static int s_doc_warn_count;
static int s_doc_fail_count;

static void
doc_emit( const char* tag, const char* clr, const char* fmt, va_list ap )
{
    printf( ORB_INDENT "%s%s%s  ", clr, tag, g_clr_reset );
    vprintf( fmt, ap );
    printf( "\n" );
}

static void
doc_ok( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    doc_emit( "[ ok ]", g_clr_green, fmt, ap );
    va_end( ap );
    ++s_doc_ok_count;
}

static void
doc_warn( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    doc_emit( "[warn]", g_clr_yellow, fmt, ap );
    va_end( ap );
    ++s_doc_warn_count;
}

static void
doc_fail( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    doc_emit( "[FAIL]", g_clr_red, fmt, ap );
    va_end( ap );
    ++s_doc_fail_count;
}

/* Hint line under the preceding result, aligned past the tag column. */
static void
doc_fix( const char* fmt, ... )
{
    printf( ORB_INDENT "        %sfix:%s ", g_clr_dim, g_clr_reset );
    va_list ap;
    va_start( ap, fmt );
    vprintf( fmt, ap );
    va_end( ap );
    printf( "\n" );
}

static void
doc_section( const char* name )
{
    printf( "\n" ORB_INDENT "%s--- %s ---%s\n", g_clr_dim, name, g_clr_reset );
}

/*==============================================================================================
    --- Helpers ---
==============================================================================================*/

/* True when exe is reachable through PATH; writes the resolved path to out when found. */
static bool
doctor_exe_on_path( const char* exe, char* out, size_t out_size )
{
#if defined( _WIN32 )
    char found[ PATH_MAX ];
    if ( SearchPathA( NULL, exe, NULL, sizeof( found ), found, NULL ) == 0 )
        return false;
    if ( out ) snprintf( out, out_size, "%s", found );
    return true;
#else
    const char* path_env = getenv( "PATH" );
    if ( !path_env ) return false;
    char paths[ 4096 ];
    snprintf( paths, sizeof( paths ), "%s", path_env );
    for ( char* tok = strtok( paths, ":" ); tok; tok = strtok( NULL, ":" ) )
    {
        char cand[ PATH_MAX ];
        snprintf( cand, sizeof( cand ), "%s/%s", tok, exe );
        if ( access( cand, X_OK ) == 0 )
        {
            if ( out ) snprintf( out, out_size, "%s", cand );
            return true;
        }
    }
    return false;
#endif
}

/* Copy of a path normalized for comparison: forward slashes, trailing separator
   stripped. Windows path compares below pair this with str_icmp. */
static void
doctor_norm_path( const char* in, char* out, size_t out_size )
{
    size_t n = 0;
    for ( ; in[ n ] && n < out_size - 1; ++n )
        out[ n ] = ( in[ n ] == '\\' ) ? '/' : in[ n ];
    while ( n > 0 && out[ n - 1 ] == '/' )
        --n;
    out[ n ] = '\0';
}

/* First whitespace-delimited token of a target's 'run' line (the exe target name). */
static void
doctor_run_exe_token( const target_info_t* t, char* exe, size_t exe_size )
{
    size_t n = 0;
    const char* p = t->run_cmd;
    while ( p[ n ] && p[ n ] != ' ' && p[ n ] != '\t' && n < exe_size - 1 )
    {
        exe[ n ] = p[ n ];
        ++n;
    }
    exe[ n ] = '\0';
}

/*==============================================================================================
    --- Check group: environment ---
==============================================================================================*/

static void
doctor_check_environment( void )
{
    doc_section( "environment" );

#if defined( _WIN32 )

#if defined( BUILD_SAFE_MODE )
    const bool safe_mode = true;
#else
    const bool safe_mode = false;
#endif

    /* cl.exe reachability mirrors the fast paths in build_setup_vc_env: on PATH under
       an \x64\ directory means builds work as-is; otherwise the vcvars CACHE (works in
       every mode) or the vcvars import (blocked by BUILD_SAFE_MODE) must cover it. */
    char cl_path[ PATH_MAX ];
    bool cl_found = doctor_exe_on_path( "cl.exe", cl_path, sizeof( cl_path ) );
    bool has_cache = ( platform_get_mtime( VCVARS_CACHE_PATH ) > 0 );
    if ( cl_found && strstr( cl_path, "\\x64\\" ) )
        doc_ok( "cl.exe on PATH (x64): %s", cl_path );
    else if ( cl_found )
    {
        doc_warn( "cl.exe on PATH is not the x64 compiler: %s", cl_path );
        doc_fix( "run vcvarsall.bat with the x64 argument -- an x86 cl produces 32-bit output" );
    }
    else if ( has_cache )
        doc_ok( "cl.exe not on PATH; vcvars cache available: %s", VCVARS_CACHE_PATH );
    else if ( safe_mode )
    {
        doc_fail( "cl.exe not on PATH; BUILD_SAFE_MODE blocks auto-import and no vcvars cache exists" );
        doc_fix( "run any build_tool command once from a Developer Command Prompt to seed %s",
                 VCVARS_CACHE_PATH );
    }
    else
    {
        char vcvars[ 512 ];
        if ( locate_vcvarsall( vcvars, sizeof( vcvars ) ) )
            doc_ok( "cl.exe not on PATH; vcvars auto-import available: %s", vcvars );
        else
        {
            doc_fail( "cl.exe not on PATH and vcvarsall.bat was not found" );
            doc_fix( "install Visual Studio 2019+ with the Desktop C++ workload" );
        }
    }

    if ( safe_mode )
        doc_warn( "BUILD_SAFE_MODE compiled in (build_tool.h): vcvars import + vswhere disabled"
                  " (cache still works)" );
    else
        doc_ok( "vcvars auto-import enabled (BUILD_SAFE_MODE off)" );

    /* msbuild only matters for building *_ms solutions outside the IDE. */
    char ms_path[ PATH_MAX ];
    if ( doctor_exe_on_path( "msbuild.exe", ms_path, sizeof( ms_path ) ) )
        doc_ok( "msbuild.exe on PATH: %s", ms_path );
    else
        doc_warn( "msbuild.exe not on PATH -- *_ms solutions build only inside Visual Studio" );

#else

    char cc_path[ PATH_MAX ];
    if ( doctor_exe_on_path( "cc", cc_path, sizeof( cc_path ) ) )
        doc_ok( "cc on PATH: %s", cc_path );
    else
    {
        doc_fail( "no C compiler (cc) on PATH" );
        doc_fix( "install gcc or clang" );
    }

#endif
}

/*==============================================================================================
    --- Check group: registry ---
==============================================================================================*/

static void
doctor_check_registry( bool registry_ok )
{
    doc_section( "registry" );

    int local_t = 0, ext_t = 0, local_s = 0;
    for ( int i = 0; i < g_target_count; ++i )
        ( g_targets[ i ].is_external ) ? ++ext_t : ++local_t;
    for ( int i = 0; i < g_solution_count; ++i )
        if ( !g_solutions[ i ].is_external ) ++local_s;

    if ( registry_ok )
        doc_ok( "orb.targets parsed: %d local + %d external targets, %d solutions",
                local_t, ext_t, local_s );
    else
    {
        doc_fail( "orb.targets failed to parse (see errors above)" );
        doc_fix( "fix the reported line -- builds and generation are blocked until it parses" );
    }

    /* Full structural validation. Its own [orb error] lines print inline above this result. */
    if ( validate_targets() )
        doc_ok( "target/solution validation passed" );
    else
        doc_fail( "target/solution validation failed (see errors above)" );

    /* Dependency cycles over every local target's closure (deps + tool deps + reflect). */
    {
        static deps_topo_t topo;
        memset( &topo, 0, sizeof( topo ) );
        for ( int i = 0; i < g_target_count; ++i )
            if ( !g_targets[ i ].is_external )
                deps_visit( &topo, &g_targets[ i ] );
        if ( topo.has_cycle )
        {
            doc_fail( "dependency %s", topo.cycle_msg );
            doc_fix( "break the cycle -- a lower layer must never dep on a higher one" );
        }
        else
            doc_ok( "no dependency cycles (%d targets in the local build closure)", topo.count );
    }

    /* Every 'run' line's first token must name a known exe target (F5 contract). */
    int run_lines = 0, run_bad = 0;
    for ( int i = 0; i < g_target_count; ++i )
    {
        const target_info_t* t = &g_targets[ i ];
        if ( !t->run_cmd ) continue;
        ++run_lines;

        char exe[ 128 ];
        doctor_run_exe_token( t, exe, sizeof( exe ) );
        const target_info_t* run_t = find_target( exe );
        if ( !run_t )
        {
            doc_fail( "target '%s': run references unknown target '%s'", t->name, exe );
            doc_fix( "point 'run' at an exe target (e.g. host_editor / host_game)" );
            ++run_bad;
        }
        else if ( run_t->type != TARGET_EXECUTABLE )
        {
            doc_warn( "target '%s': run target '%s' is not an exe -- F5 cannot launch it",
                      t->name, exe );
            ++run_bad;
        }
    }
    if ( run_lines && !run_bad )
        doc_ok( "%d 'run' line(s) reference valid exe targets", run_lines );

    /* Pool headroom: overflow crashes at startup (built-ins append unguarded), so warn early. */
    if ( g_target_count >= MAX_TARGETS - 8 )
    {
        doc_warn( "target pool nearly full: %d/%d", g_target_count, MAX_TARGETS );
        doc_fix( "raise MAX_TARGETS in build_tool.h and rebuild (bootstrap_build_tool.bat)" );
    }
    else
        doc_ok( "target pool headroom: %d/%d", g_target_count, MAX_TARGETS );
}

/*==============================================================================================
    --- Check group: module descriptors ---

    A module's runtime dependencies live in two hand-maintained places that nothing
    cross-checks: mod_desc_t.deps[] in <name>_get_mod_desc() drives load/init order, and
    the target's 'dep' lines in orb.targets drive linking and build scheduling. They
    drift silently -- a dep present in one but not the other changes init order or link
    inputs with no build error. This group finds each module's descriptor in source,
    parses .deps / .dep_count with a plain text scan, and reports the drift.

    Descriptor discovery relies on two firm conventions: the descriptor function is
    named <target>_get_mod_desc, and it lives in a .c file under the target's root
    (usually <target>_api.c or <target>.c -- those are probed first, then the tree is
    walked, each file mapped at most once). Exe targets never carry descriptors.

    Static modules are checked in both directions. Dynamic modules never link their
    deps (host-only rule), so only their descriptor itself is validated.
==============================================================================================*/

#define DOC_DESC_MAX_DEPS   12    // parse cap; > MOD_MAX_DEPS (8) so overflow is visible
#define DOC_DESC_NAME_MAX   40

typedef struct
{
    const target_info_t* target;
    char                 file[ PATH_MAX ];                                // defining .c file
    char                 deps[ DOC_DESC_MAX_DEPS ][ DOC_DESC_NAME_MAX ];  // quoted names in .deps
    int                  name_count;                                      // entries in deps[]
    int                  dep_count;                                       // parsed .dep_count (0 when absent)

} doctor_desc_t;

static doctor_desc_t s_doc_descs[ MAX_TARGETS ];
static int           s_doc_desc_count;

/* Bounded substring search over a mapped (non-NUL-terminated) buffer. */
static const char*
doc_memfind( const char* hay, size_t hay_len, const char* needle, size_t needle_len )
{
    if ( needle_len == 0 || hay_len < needle_len ) return NULL;
    for ( size_t i = 0; i + needle_len <= hay_len; ++i )
        if ( hay[ i ] == needle[ 0 ] && memcmp( hay + i, needle, needle_len ) == 0 )
            return hay + i;
    return NULL;
}

static bool
doc_is_ident_char( char c )
{
    return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

static size_t
doc_skip_ws( const char* data, size_t size, size_t p )
{
    while ( p < size && ( data[ p ] == ' ' || data[ p ] == '\t' || data[ p ] == '\r' || data[ p ] == '\n' ) )
        ++p;
    return p;
}

/* True when the buffer contains the DEFINITION "<token>( void ) ... {".
   Call sites have no "( void )"; header-style declarations end in ';' not '{'. */
static bool
doc_defines_desc_fn( const char* data, size_t size, const char* token )
{
    size_t tlen = strlen( token );
    size_t off  = 0;
    while ( off + tlen <= size )
    {
        const char* hit = doc_memfind( data + off, size - off, token, tlen );
        if ( !hit ) return false;

        size_t pos = ( size_t )( hit - data );
        off        = pos + 1;
        if ( pos > 0 && doc_is_ident_char( data[ pos - 1 ] ) ) continue;    // longer identifier

        size_t p = pos + tlen;
        if ( p + 8 > size || memcmp( data + p, "( void )", 8 ) != 0 ) continue;
        p = doc_skip_ws( data, size, p + 8 );
        if ( p < size && data[ p ] == '{' ) return true;
    }
    return false;
}

/* Parse the first `.deps = { "a", "b", ... }` and `.dep_count = N` in the file.
   The initializer may sit at file scope (gui) or inside the function (most modules),
   so the whole file is the parse scope. `{ 0 }` / `{ NULL }` / `{}` all yield zero names. */
static void
doc_parse_desc_fields( const char* data, size_t size, doctor_desc_t* d )
{
    d->name_count = 0;
    d->dep_count  = 0;

    size_t off = 0;
    while ( off < size )
    {
        const char* hit = doc_memfind( data + off, size - off, ".deps", 5 );
        if ( !hit ) break;
        size_t p = ( size_t )( hit - data ) + 5;
        off      = ( size_t )( hit - data ) + 1;

        if ( p < size && doc_is_ident_char( data[ p ] ) ) continue;    // .deps_something
        p = doc_skip_ws( data, size, p );
        if ( p >= size || data[ p ] != '=' ) continue;
        p = doc_skip_ws( data, size, p + 1 );
        if ( p >= size || data[ p ] != '{' ) continue;

        for ( ++p; p < size && data[ p ] != '}'; ++p )
        {
            if ( data[ p ] != '"' ) continue;
            char   name[ DOC_DESC_NAME_MAX ];
            size_t n = 0;
            for ( ++p; p < size && data[ p ] != '"'; ++p )
                if ( n < DOC_DESC_NAME_MAX - 1 ) name[ n++ ] = data[ p ];
            name[ n ] = '\0';
            if ( d->name_count < DOC_DESC_MAX_DEPS )
                snprintf( d->deps[ d->name_count++ ], DOC_DESC_NAME_MAX, "%s", name );
        }
        break;    // first initializer wins
    }

    off = 0;
    while ( off < size )
    {
        const char* hit = doc_memfind( data + off, size - off, ".dep_count", 10 );
        if ( !hit ) break;
        size_t p = ( size_t )( hit - data ) + 10;
        off      = ( size_t )( hit - data ) + 1;

        p = doc_skip_ws( data, size, p );
        if ( p >= size || data[ p ] != '=' ) continue;
        p = doc_skip_ws( data, size, p + 1 );

        int  v   = 0;
        bool any = false;
        for ( ; p < size && data[ p ] >= '0' && data[ p ] <= '9'; ++p, any = true )
            v = v * 10 + ( data[ p ] - '0' );
        if ( !any ) continue;
        d->dep_count = v;
        break;
    }
}

/* Map one file and, when it defines the descriptor function, parse it into d. */
static bool
doctor_scan_desc_file( const char* path, const char* token, doctor_desc_t* d )
{
    platform_mapped_file_t mf = { 0 };
    if ( !platform_map_file( path, &mf ) ) return false;

    bool found = false;
    if ( mf.data && doc_defines_desc_fn( mf.data, mf.size, token ) )
    {
        doc_parse_desc_fields( mf.data, mf.size, d );
        snprintf( d->file, sizeof( d->file ), "%s", path );
        found = true;
    }
    platform_unmap_file( &mf );
    return found;
}

/* Depth-first walk for the descriptor; skip_a/skip_b are the top-level fast-path
   candidates already probed, so no file is mapped twice. Stops at the first hit. */
static bool
doctor_scan_desc_dir( const char* dir, const char* token, bool top,
                      const char* skip_a, const char* skip_b, doctor_desc_t* d )
{
    char pattern[ PATH_MAX ];
    snprintf( pattern, sizeof( pattern ), "%s" PATH_SEP "*", dir );

    platform_find_data_t fd;
    platform_find_t      h = platform_find_first( pattern, &fd );
    if ( h == PLATFORM_FIND_INVALID ) return false;

    bool found = false;
    do
    {
        if ( strcmp( fd.name, "." ) == 0 || strcmp( fd.name, ".." ) == 0 ) continue;

        char path[ PATH_MAX ];
        snprintf( path, sizeof( path ), "%s" PATH_SEP "%s", dir, fd.name );

        if ( fd.is_dir )
            found = doctor_scan_desc_dir( path, token, false, NULL, NULL, d );
        else
        {
            const char* ext = strrchr( fd.name, '.' );
            if ( !ext || str_icmp( ext, ".c" ) != 0 ) continue;
            if ( top && ( ( skip_a && str_icmp( fd.name, skip_a ) == 0 ) ||
                          ( skip_b && str_icmp( fd.name, skip_b ) == 0 ) ) )
                continue;
            found = doctor_scan_desc_file( path, token, d );
        }
    } while ( !found && platform_find_next( h, &fd ) );

    platform_find_close( h );
    return found;
}

/* Locate + parse a target's descriptor. Probes <root>/<name>_api.c and <root>/<name>.c
   before falling back to the tree walk -- the convention hits for nearly every module. */
static bool
doctor_scan_desc_target( const target_info_t* t, doctor_desc_t* d )
{
    char token[ 160 ], fast_a[ 160 ], fast_b[ 160 ], path[ PATH_MAX + 176 ];
    snprintf( token, sizeof( token ), "%s_get_mod_desc", t->name );
    snprintf( fast_a, sizeof( fast_a ), "%s_api.c", t->name );
    snprintf( fast_b, sizeof( fast_b ), "%s.c", t->name );

    snprintf( path, sizeof( path ), "%s" PATH_SEP "%s", t->root_dir, fast_a );
    if ( platform_file_exists( path ) && doctor_scan_desc_file( path, token, d ) ) return true;

    snprintf( path, sizeof( path ), "%s" PATH_SEP "%s", t->root_dir, fast_b );
    if ( platform_file_exists( path ) && doctor_scan_desc_file( path, token, d ) ) return true;

    return doctor_scan_desc_dir( t->root_dir, token, true, fast_a, fast_b, d );
}

static bool
doc_desc_declares( const doctor_desc_t* d, const char* name )
{
    for ( int i = 0; i < d->name_count; ++i )
        if ( strcmp( d->deps[ i ], name ) == 0 ) return true;
    return false;
}

static bool
doc_target_links( const target_info_t* t, const char* name )
{
    for ( int i = 0; i < TARGET_MAX_SLOTS && t->deps[ i ]; ++i )
        if ( strcmp( t->deps[ i ], name ) == 0 ) return true;
    return false;
}

/* Descriptor lookup by target name -- membership means "this target is a module". */
static const doctor_desc_t*
doc_find_desc( const char* name )
{
    for ( int i = 0; i < s_doc_desc_count; ++i )
        if ( strcmp( s_doc_descs[ i ].target->name, name ) == 0 ) return &s_doc_descs[ i ];
    return NULL;
}

static void
doc_name_append( char* buf, size_t size, const char* name )
{
    size_t len = strlen( buf );
    snprintf( buf + len, size - len, " %s", name );
}

static void
doctor_check_module_descs( bool registry_ok )
{
    doc_section( "module descriptors" );

    if ( !registry_ok )
    {
        doc_warn( "skipped -- orb.targets did not parse" );
        return;
    }

    /* Pass 1: collect every local static/dynamic target that defines a descriptor. */
    s_doc_desc_count = 0;
    for ( int i = 0; i < g_target_count && s_doc_desc_count < MAX_TARGETS; ++i )
    {
        const target_info_t* t = &g_targets[ i ];
        if ( t->is_external || t->is_tool || t->is_build_tool || t->is_reflect_tool ) continue;
        if ( !t->root_dir || t->type == TARGET_EXECUTABLE ) continue;

        doctor_desc_t* d = &s_doc_descs[ s_doc_desc_count ];
        memset( d, 0, sizeof( *d ) );
        if ( doctor_scan_desc_target( t, d ) )
        {
            d->target = t;
            ++s_doc_desc_count;
        }
    }

    if ( !s_doc_desc_count )
    {
        doc_warn( "no module descriptors found under any static/dynamic target root" );
        return;
    }

    /* Pass 2: validate each descriptor and cross-check it against orb.targets. */
    int issues = 0;
    for ( int i = 0; i < s_doc_desc_count; ++i )
    {
        const doctor_desc_t* d = &s_doc_descs[ i ];
        const target_info_t* t = d->target;

        /* Internal consistency: the loader reads exactly dep_count entries of deps[],
           so a count that disagrees with the initializer silently drops or invents deps. */
        if ( d->dep_count != d->name_count )
        {
            doc_fail( "target '%s': descriptor lists %d dep name(s) but dep_count = %d (%s)",
                      t->name, d->name_count, d->dep_count, d->file );
            doc_fix( "make .dep_count match the .deps initializer" );
            ++issues;
        }

        /* Typo catch: module names equal target names by convention. */
        for ( int j = 0; j < d->name_count; ++j )
        {
            if ( !find_target( d->deps[ j ] ) )
            {
                doc_warn( "target '%s': descriptor dep '%s' is not a known target",
                          t->name, d->deps[ j ] );
                doc_fix( "fix the .deps entry in %s", d->file );
                ++issues;
            }
        }

        /* Link cross-check, static modules only: dynamic modules never link their deps
           (host-only rule), so their .deps have no orb.targets echo to compare against. */
        if ( t->type != TARGET_STATIC_LIB )
            continue;

        char names[ 256 ];

        /* Only static dep targets expect a 'dep' echo: a descriptor dep on a dynamic
           module is load-order/gateway-only (link happens via host mono_dep, if ever). */
        names[ 0 ] = '\0';
        for ( int j = 0; j < d->name_count; ++j )
        {
            const target_info_t* dep_t = find_target( d->deps[ j ] );
            if ( dep_t && dep_t->type == TARGET_STATIC_LIB && !doc_target_links( t, d->deps[ j ] ) )
                doc_name_append( names, sizeof( names ), d->deps[ j ] );
        }
        if ( names[ 0 ] )
        {
            doc_warn( "target '%s': descriptor declares dep(s) orb.targets does not link:%s",
                      t->name, names );
            doc_fix( "add 'dep%s' to target '%s' in orb.targets, or drop from .deps", names, t->name );
            ++issues;
        }

        names[ 0 ] = '\0';
        for ( int j = 0; j < TARGET_MAX_SLOTS && t->deps[ j ]; ++j )
            if ( doc_find_desc( t->deps[ j ] ) && !doc_desc_declares( d, t->deps[ j ] ) )
                doc_name_append( names, sizeof( names ), t->deps[ j ] );
        if ( names[ 0 ] )
        {
            doc_warn( "target '%s': links module lib(s) its descriptor does not declare:%s",
                      t->name, names );
            doc_fix( "declare in .deps (%s) if init order matters; link-only use is fine", d->file );
            ++issues;
        }
    }

    if ( !issues )
        doc_ok( "%d module descriptor(s) agree with orb.targets", s_doc_desc_count );
}

/*==============================================================================================
    --- Check group: filesystem ---
==============================================================================================*/

static void
doctor_check_filesystem( void )
{
    doc_section( "filesystem" );

    /* include_dir paths: a '%' means platform_expand_env left an unset variable in
       place (e.g. %VULKAN_SDK% without the SDK installed) -- that is a hard compile
       break; a missing directory is only a warning (may be optional headers). */
    int inc_total = 0, inc_bad = 0;
    for ( int i = 0; i < g_target_count; ++i )
    {
        const target_info_t* t = &g_targets[ i ];
        for ( int j = 0; j < MAX_EXTRA_INCLUDE_DIRS && t->extra_include_dirs[ j ]; ++j )
        {
            const char* dir = t->extra_include_dirs[ j ];
            ++inc_total;
            if ( strchr( dir, '%' ) )
            {
                doc_fail( "target '%s': include_dir has an unexpanded env var: %s", t->name, dir );
                doc_fix( "set the environment variable (e.g. VULKAN_SDK) and re-run" );
                ++inc_bad;
            }
            else if ( !platform_file_exists( dir ) )
            {
                doc_warn( "target '%s': include_dir does not exist: %s", t->name, dir );
                ++inc_bad;
            }
        }
    }
    if ( inc_total && !inc_bad )
        doc_ok( "%d include_dir path(s) exist", inc_total );

    /* Generated solutions vs orb.targets: stale project files are the most common
       "VS is building the wrong thing" report. */
    platform_mtime_t reg_mtime = platform_get_mtime( "orb.targets" );
    int sln_total = 0, sln_stale = 0;
    for ( int i = 0; i < g_solution_count; ++i )
    {
        const solution_info_t* sln = &g_solutions[ i ];
        if ( sln->is_external || !sln->out_dir || !sln->name ) continue;

        const char* suffixes[ 2 ][ 2 ] = { { "_nm", "" }, { "_ms", "_ms" } };
        for ( int fl = 0; fl < 2; ++fl )
        {
            char path[ PATH_MAX ];
            snprintf( path, sizeof( path ), "%s%s" PATH_SEP "%s%s.sln",
                      sln->out_dir, suffixes[ fl ][ 1 ], sln->name, suffixes[ fl ][ 0 ] );
            ++sln_total;

            platform_mtime_t m = platform_get_mtime( path );
            if ( m == 0 )
            {
                doc_warn( "solution '%s': %s not generated", sln->name, path );
                doc_fix( "run: build_tool -gen" );
                ++sln_stale;
            }
            else if ( m < reg_mtime )
            {
                doc_warn( "solution '%s': %s is older than orb.targets", sln->name, path );
                doc_fix( "run: build_tool -gen" );
                ++sln_stale;
            }
        }
    }
    if ( sln_total && !sln_stale )
        doc_ok( "%d generated solution file(s) up to date", sln_total );

    /* Run-line exes on disk: F5 launches these. External targets resolve to the engine
       bin, local ones to the local bin (mirrors gen_run_debug_cmd). Missing is only a
       warning -- a build produces them. */
    int run_exes = 0, run_missing = 0;
    for ( int i = 0; i < g_target_count; ++i )
    {
        const target_info_t* t = &g_targets[ i ];
        if ( !t->run_cmd ) continue;

        char exe[ 128 ];
        doctor_run_exe_token( t, exe, sizeof( exe ) );
        const target_info_t* run_t = find_target( exe );
        if ( !run_t || run_t->type != TARGET_EXECUTABLE ) continue;    // reported in registry group

        char path[ PATH_MAX + 32 ];
        if ( run_t->is_external && g_engine_root[ 0 ] )
            snprintf( path, sizeof( path ), "%s" PATH_SEP "bin" PATH_SEP "%s.exe", g_engine_root, exe );
        else
            snprintf( path, sizeof( path ), "bin" PATH_SEP "%s.exe", exe );

        ++run_exes;
        if ( !platform_file_exists( path ) )
        {
            doc_warn( "F5 host not built yet: %s", path );
            doc_fix( run_t->is_external ? "build '%s' at the engine root" : "run: build_tool -target %s", exe );
            ++run_missing;
        }
    }
    if ( run_exes && !run_missing )
        doc_ok( "%d F5 host exe(s) present", run_exes );

    /* bin writability: locked outputs (running host, stuck debugger) break every build. */
    if ( platform_file_exists( "bin" ) )
    {
        const char* probe_path = "bin" PATH_SEP ".orb_doctor_probe";
        FILE* probe = fopen( probe_path, "w" );
        if ( probe )
        {
            fclose( probe );
            remove( probe_path );
            doc_ok( "bin" PATH_SEP " is writable" );
        }
        else
        {
            doc_fail( "bin" PATH_SEP " exists but is not writable" );
            doc_fix( "check permissions, or close a host/debugger holding files open" );
        }
    }
    else
        doc_ok( "bin" PATH_SEP " not created yet (the first build creates it)" );

    /* Reflection: has_reflect targets need a registered reflect tool. */
    int refl_targets = 0;
    for ( int i = 0; i < g_target_count; ++i )
        if ( !g_targets[ i ].is_external && g_targets[ i ].has_reflect ) ++refl_targets;
    if ( refl_targets )
    {
        if ( find_reflect_tool() )
            doc_ok( "reflect_tool registered (%d local reflect target(s))", refl_targets );
        else
        {
            doc_fail( "%d target(s) use reflection but no reflect_tool is registered", refl_targets );
            doc_fix( "built-ins register it automatically -- check for 'is_reflect_tool' flag misuse" );
        }
    }

    /* Resource tables: every dynamic module and every exe linking res needs res_tool. */
    int res_targets = 0;
    for ( int i = 0; i < g_target_count; ++i )
        if ( !g_targets[ i ].is_external && target_wants_res_table( &g_targets[ i ] ) ) ++res_targets;
    if ( res_targets )
    {
        if ( find_res_tool() )
            doc_ok( "res_tool registered (%d local target(s) carry a resource table)", res_targets );
        else
        {
            doc_fail( "%d target(s) carry a resource table but no res_tool is registered", res_targets );
            doc_fix( "built-ins register it automatically -- check for 'is_res_tool' flag misuse" );
        }
    }
}

/*==============================================================================================
    --- Check group: child project wiring ---

    Only when 'engine <path>' is declared. The generated project files, the bat
    forwarder, and clean_build.bat all embed paths back to the engine root; this
    group verifies those links agree and the engine side actually has its tool.
==============================================================================================*/

static void
doctor_check_child_wiring( void )
{
    if ( !g_engine_root[ 0 ] )
        return;

    doc_section( "child project wiring" );

    if ( !platform_file_exists( g_engine_root ) )
    {
        doc_fail( "engine root does not exist: %s", g_engine_root );
        doc_fix( "fix the 'engine' path in orb.targets" );
        return;    // everything below keys off this path
    }
    doc_ok( "engine root: %s", g_engine_root );

    char p[ PATH_MAX + 32 ];
    snprintf( p, sizeof( p ), "%s" PATH_SEP "bin" PATH_SEP "build_tool.exe", g_engine_root );
    if ( platform_file_exists( p ) )
        doc_ok( "engine build_tool.exe present" );
    else
    {
        doc_fail( "engine build_tool.exe missing: %s", p );
        doc_fix( "run bootstrap_build_tool.bat at the engine root (generated projects shell out to it)" );
    }

    /* .orb_engine must agree with the 'engine' directive: clean_build.bat reads it to
       restore the bat forwarder, so a stale path silently re-wires builds elsewhere. */
    {
        FILE* f = fopen( ".orb_engine", "r" );
        if ( !f )
        {
            doc_warn( "no .orb_engine file (clean_build.bat reads it)" );
            doc_fix( "re-run the scaffold, or write the engine root path into .orb_engine" );
        }
        else
        {
            char line[ PATH_MAX ] = { 0 };
            if ( fgets( line, sizeof( line ), f ) )
            {
                size_t l = strlen( line );
                while ( l > 0 && ( line[ l - 1 ] == '\n' || line[ l - 1 ] == '\r' ) ) line[ --l ] = '\0';
            }
            fclose( f );

            char a[ PATH_MAX ], b[ PATH_MAX ];
            doctor_norm_path( line, a, sizeof( a ) );
            doctor_norm_path( g_engine_root, b, sizeof( b ) );
            if ( str_icmp( a, b ) == 0 )
                doc_ok( ".orb_engine agrees with the 'engine' directive" );
            else
            {
                doc_warn( ".orb_engine disagrees with orb.targets: '%s' vs '%s'", line, g_engine_root );
                doc_fix( "clean_build.bat would re-wire to the wrong engine -- update .orb_engine" );
            }
        }
    }

    /* bin\build_tool.bat forwarder: CLI convenience only (VS projects use the absolute
       exe path), but a stale one sends hand-typed builds to a dead engine path. */
    {
        const char* bat_path = "bin" PATH_SEP "build_tool.bat";
        FILE* f = fopen( bat_path, "r" );
        if ( !f )
        {
            doc_warn( "no %s forwarder (CLI builds need the full engine exe path)", bat_path );
            doc_fix( "re-run bootstrap_project.bat from this directory, or clean_build.bat" );
        }
        else
        {
            char line[ PATH_MAX + 64 ] = { 0 };
            if ( !fgets( line, sizeof( line ), f ) ) line[ 0 ] = '\0';
            fclose( f );

            /* Expected shape: @"<engine>\bin\build_tool.exe" %* -- pull the quoted path. */
            char* q1 = strchr( line, '"' );
            char* q2 = q1 ? strchr( q1 + 1, '"' ) : NULL;
            if ( q1 && q2 )
            {
                *q2 = '\0';
                if ( platform_file_exists( q1 + 1 ) )
                    doc_ok( "%s forwarder points at an existing exe", bat_path );
                else
                {
                    doc_warn( "%s points at a missing exe: %s", bat_path, q1 + 1 );
                    doc_fix( "engine moved? re-run bootstrap_project.bat or clean_build.bat" );
                }
            }
            else
                doc_warn( "%s has an unexpected format (hand-edited?)", bat_path );
        }
    }
}

/*==============================================================================================
    --- cmd_doctor (-doctor) ---
==============================================================================================*/

static int
cmd_doctor( bool registry_ok )
{
    char cwd[ PATH_MAX ] = { 0 };
    platform_get_cwd( cwd, sizeof( cwd ) );

    printf( ORB_BANNER "----------------------------------------------------------------\n" );
    printf( ORB_BANNER "[orb doctor]  [ %s | %s ]\n",
            g_engine_root[ 0 ] ? "child project" : "engine root", cwd );

    s_doc_ok_count = s_doc_warn_count = s_doc_fail_count = 0;

    doctor_check_environment();
    doctor_check_registry( registry_ok );
    doctor_check_module_descs( registry_ok );
    doctor_check_filesystem();
    doctor_check_child_wiring();

    const char* clr = s_doc_fail_count ? g_clr_red
                    : s_doc_warn_count ? g_clr_yellow : g_clr_green;
    printf( "\n" ORB_BANNER "%s[ DOCTOR: %d ok | %d warn | %d fail ]%s\n\n",
            clr, s_doc_ok_count, s_doc_warn_count, s_doc_fail_count, g_clr_reset );

    return s_doc_fail_count ? 1 : 0;
}

// clang-format on
/*============================================================================================*/
