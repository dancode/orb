/*==============================================================================================

    build_tool_06_compile.c -- Compiler command construction and execution.

    Assembles the cl.exe command line for a single target and runs it.
    The command is built section-by-section into a compile_cmd_t struct so each
    logical group (flags, defines, includes, sources, output) can be printed
    independently under g_out_flags control before being joined for execution.

    Three public entry points, all called from 08_exec.c or main():
      build_target_compile()        -- full unity compile; /showIncludes + _includes.txt.
      build_target_compile_single() -- single-file compile for -file flag; no tracking.
      build_target_compile_only()   -- all units, no link step; for -compile-only flag.

    Define source of truth:
      Preprocessor defines are driven from the shared tables in 02_data.c
      (g_defines_always, g_defines_debug, g_defines_release). 11_gen.c reads
      the same tables for IntelliSense vcxproj emission -- no manual lockstep.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Compile Command Struct ---

    Holds each logical fragment of the final cl.exe command line.
    Assembled in order via cc_fill_compile_cmd(); printed selectively via
    cc_print(); joined into one string by cc_assemble() for execution.
==============================================================================================*/

typedef struct
{
    char exe      [ 64          ];  // cl.exe or clang-cl.exe
    char flags    [ 512         ];  // /c /nologo /W4 /WX /Zi /Od /MDd ...
    char includes [ 512         ];  // /I source /I gen_dir
    char defines  [ 1024        ];  // /DOS_WINDOWS /DCOMPILE_MSVC /D_DEBUG ...
    char output   [ 512         ];  // /FoobjDir/ /FdobjDir/
    char sources  [ CMD_BUF_MAX ];  // absolute .c paths

} compile_cmd_t;

/*==============================================================================================
    --- Field Append ---

    Append a formatted string to a fixed-size field inside compile_cmd_t.
    Halts the process on overflow so the caller never silently passes a
    truncated command to the compiler.
==============================================================================================*/

static void
cc_field( char* dst, size_t dst_size, const char* fmt, ... )
{
    size_t used = strlen( dst );
    if ( used >= dst_size - 1 )
    {
        printf( ORB_INDENT "[orb error] cc_field overflow (capacity %zu)"
                " -- raise field size in compile_cmd_t\n", dst_size );
        exit( 1 );
    }
    size_t remaining = dst_size - used;

    va_list args;
    va_start( args, fmt );
    int written = vsnprintf( dst + used, remaining, fmt, args );
    va_end( args );

    if ( written < 0 || ( size_t )written >= remaining )
    {
        printf( ORB_INDENT "[orb error] cc_field truncated (needed %d, had %zu)"
                " -- raise field size in compile_cmd_t\n", written, remaining );
        exit( 1 );
    }
}

// Convenience wrapper: infers dst_size from the field's declared array size.
#define CC_APPEND( field, ... ) cc_field( ( field ), sizeof( field ), __VA_ARGS__ )

/*==============================================================================================
    Verify the last byte of every compile_cmd_t field is still '\0' before
    assembly. cc_field() aborts on detected overflow, but this catches any
    silent corruption that bypassed it.
==============================================================================================*/

static void
cc_check_overflow( const compile_cmd_t* cc )
{
    if ( cc->exe     [ 64          - 1 ] || cc->flags   [ 512         - 1 ] ||
         cc->includes[ 512         - 1 ] || cc->defines [ 1024        - 1 ] ||
         cc->output  [ 512         - 1 ] || cc->sources [ CMD_BUF_MAX - 1 ] )
    {
        printf( ORB_INDENT "[orb error] compile_cmd_t sentinel overwritten -- field overflow\n" );
        exit( 1 );
    }
}

/*==============================================================================================
    get_target_upper() -- derive TARGETNAME from a target name string.
    Used for both the _STATIC define and the startup banner.
    Must match the IntelliSense defines emitted by 11_gen.c (unity build).
==============================================================================================*/

static void
get_target_upper( const char* name, char* out, size_t out_size )
{
    strncpy( out, name, out_size - 1 );
    out[ out_size - 1 ] = '\0';
    for ( char* p = out; *p; ++p ) *p = ( char )toupper( *p );
}

/*==============================================================================================
    --- Compile Section Printer ---

    Print compile_cmd_t fields to `out` according to g_out_flags.
    Called before assembly so the user sees what is about to be run.
==============================================================================================*/

static void
cc_print( FILE* out, const compile_cmd_t* cc, const target_info_t* target, const char* config )
{
    ( void )config;
    if ( g_out_flags & ORB_OUT_SUMMARY_COMPILE )
        fprintf( out, ORB_INDENT "[orb compiling] %s\n", target->name );

    if ( g_out_flags & ORB_OUT_ANY_COMPILE )
        fprintf( out, "\n" );

    bool any = false;
    if ( g_out_flags & ORB_OUT_COMPILE_SOURCES  ) any |= print_section( out, "sources:",  cc->sources,  NULL );
    if ( g_out_flags & ORB_OUT_COMPILE_FLAGS    ) any |= print_section( out, "flags:",    cc->flags,    NULL );
    if ( g_out_flags & ORB_OUT_COMPILE_DEFINES  ) any |= print_section( out, "defines:",  cc->defines,  "/D" );
    if ( g_out_flags & ORB_OUT_COMPILE_INCLUDES ) any |= print_section( out, "includes:", cc->includes, "/I" );
    if ( g_out_flags & ORB_OUT_COMPILE_OUTPUT   ) any |= print_compile_output( out, cc->output );
    if ( any ) fprintf( out, "\n" );
}

/*==============================================================================================
    --- Command Assembly ---

    Join the compile_cmd_t fields into a single string for the actual cl.exe call.
    Response-file spill happens here when the assembled string exceeds the shell
    arg limit -- operating on the joined result rather than individual fields.

    The sources field alone can be CMD_BUF_MAX bytes; joining everything through
    cmd_buf_t (also CMD_BUF_MAX) would silently truncate sources before the spill
    check ever runs. Instead we format into a dedicated oversize buffer, measure
    the total, then write the rsp file from that buffer while the data is complete.
==============================================================================================*/

static bool
cc_assemble( const compile_cmd_t* cc, cmd_buf_t* cmd, const char* rsp_path )
{
    char args[ CMD_BUF_MAX ];

    int written = snprintf( args, sizeof( args ), "%s %s %s %s %s",
                            cc->flags, cc->includes, cc->defines, cc->output, cc->sources );

    if ( written < 0 || ( size_t )written >= sizeof( args ) )
    {
        printf( ORB_INDENT "[orb error] cc_assemble args truncated (needed %d)\n", written );
        return false;
    }

    size_t total = strlen( cc->exe ) + 1 + ( size_t )( written < 0 ? 0 : written );
    if ( total >= CMD_RSP_THRESHOLD )
    {
        if ( g_use_rsp )
        {
            FILE* f = fopen( rsp_path, "w" );
            if ( f )
            {
                fputs( args, f );
                fclose( f );
            }
            else
            {
                printf( ORB_INDENT "[orb error] could not open response file %s\n", rsp_path );
                return false;
            }
            cmd->size      = 0;
            cmd->truncated = false;
            cmd_append( cmd, "%s @%s", cc->exe, rsp_path );
            return true;
        }

        printf( ORB_INDENT "[orb error] command length %zu exceeds threshold;"
                " enable -rsp to use a response file\n", total );
        return false;
    }

    cmd_append( cmd, "%s %s", cc->exe, args );
    return true;
}

/*==============================================================================================
    --- Fill Compile Command ---

    Shared setup for both compile entry points. Fills exe, flags, includes,
    defines, and output. Sources are left empty -- the caller appends them.
    /showIncludes is also left to the caller so each entry point can opt in.
==============================================================================================*/

static void
cc_fill_compile_cmd( build_context_t* ctx, target_info_t* target,
                     const char* obj_dir, const char* gen_dir, compile_cmd_t* cc )
{
    // exe
    snprintf( cc->exe, sizeof( cc->exe ), "%s",
              ctx->compiler == COMPILE_CLANG ? "clang-cl.exe" : "cl.exe" );

    // flags: standard + config-specific + active warning suppressions.
    CC_APPEND( cc->flags, "/c /nologo /W4 /WX /Zc:preprocessor /std:c11" );
    if ( ctx->config == CONFIG_DEBUG ) CC_APPEND( cc->flags, " /Zi /Od /MDd" );
    else                               CC_APPEND( cc->flags, " /O2 /MD" );

    for ( int i = 0; i < g_warn_suppression_count; ++i )
    {
        warn_suppress_t* s = &g_warn_suppressions[ i ];
        if ( ( s->config == ctx->config || s->config == CONFIG_COUNT ) &&
             ( s->compiler == ctx->compiler ) )
            CC_APPEND( cc->flags, " %s", s->flag );
    }

    // includes: generated headers are always in the search path.
    CC_APPEND( cc->includes, "/I source /I %s", gen_dir );

    // defines: consumed from shared tables in 02_data.c so the IntelliSense
    // output in 11_gen.c is guaranteed to match. Per-target _STATIC chain
    // is procedural -- it depends on runtime target state.
    for ( int i = 0; g_defines_always[ i ]; ++i )
        CC_APPEND( cc->defines, "%s/D%s", cc->defines[ 0 ] ? " " : "", g_defines_always[ i ] );

    {
        char upper[ 128 ];
        get_target_upper( target->name, upper, sizeof( upper ) );
        CC_APPEND( cc->defines, " /D%s_STATIC", upper );
    }
    for ( int i = 0; target->deps[ i ]; ++i )
    {
        target_info_t* dep = find_target( target->deps[ i ] );
        if ( !dep ) continue;
        bool dep_is_static = ( dep->type == TARGET_STATIC_LIB ) ||
                             ( dep->type == TARGET_DYNAMIC_LIB && ctx->is_monolithic );
        if ( dep_is_static )
        {
            char dep_upper[ 128 ];
            get_target_upper( dep->name, dep_upper, sizeof( dep_upper ) );
            CC_APPEND( cc->defines, " /D%s_STATIC", dep_upper );
        }
    }
    if ( ctx->is_monolithic ) CC_APPEND( cc->defines, " /DBUILD_STATIC" );
    {
        const char** cfg_defines = ( ctx->config == CONFIG_DEBUG ) ? g_defines_debug : g_defines_release;
        for ( int i = 0; cfg_defines[ i ]; ++i )
            CC_APPEND( cc->defines, " /D%s", cfg_defines[ i ] );
    }

    // output dirs: trailing slash required -- without it cl treats the path as a filename prefix.
    CC_APPEND( cc->output, "/Fo%s/ /Fd%s/", obj_dir, obj_dir );
}

/*==============================================================================================
    --- Run Compile Command ---

    Shared tail for both compile entry points: print sections, assemble, echo
    raw command if requested, then run. includes_path is forwarded to
    build_run_cmd_capture_includes; NULL means no includes file is written.
==============================================================================================*/

static bool
cc_run_compile_cmd( compile_cmd_t* cc, target_info_t* target, const char* config,
                    const char* obj_dir, const char* rsp_name, const char* includes_path )
{
    cmd_buf_t cmd = { 0 };

    FILE* log_out = cc_open_log();
    cc_print( log_out, cc, target, config );

    char rsp_path[ PATH_MAX ];
    snprintf( rsp_path, sizeof( rsp_path ), "%s\\%s", obj_dir, rsp_name );
    cc_check_overflow( cc );
    bool ok = cc_assemble( cc, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_COMPILE_CMD ) print_raw_cmd( log_out, cmd.buf );
    cc_close_log( log_out );

    if ( !ok ) return false;
    return build_run_cmd_capture_includes( cmd.buf, includes_path ) == 0;
}

/*==============================================================================================

    build_target_compile()

    Full unity compile: all target units + generated reflect file. Adds /showIncludes
    when include tracking is active so the captured output feeds _includes.txt for
    the next incremental up-to-date check.

==============================================================================================*/

bool
build_target_compile( build_context_t* ctx, target_info_t* target,
                      const char* obj_dir, const char* gen_dir )
{
    compile_cmd_t cc     = { 0 };
    const char*   config = ( ctx->config == CONFIG_DEBUG ) ? "Debug" : "Release";

    cc_fill_compile_cmd( ctx, target, obj_dir, gen_dir, &cc );

    // /showIncludes: cl.exe emits "Note: including file: <path>" for each header.
    // We capture those lines into _includes.txt so header changes trigger rebuilds.
    // Only added when include tracking is active -- the output is verbose.
    if ( g_include_track ) CC_APPEND( cc.flags, " /showIncludes" );

    // sources: absolute paths so MSVC error messages are navigable from any CWD.
    {
        char rel[ PATH_MAX ], abs_p[ PATH_MAX ];
        for ( int i = 0; target->units[ i ]; ++i )
        {
            snprintf( rel, sizeof( rel ), "%s\\%s", target->root_dir, target->units[ i ] );
            if ( !platform_fullpath( abs_p, rel, sizeof( abs_p ) ) )
                snprintf( abs_p, sizeof( abs_p ), "%s", rel );
            CC_APPEND( cc.sources, "%s%s", cc.sources[ 0 ] ? " " : "", abs_p );
        }

        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;
            snprintf( rel, sizeof( rel ), "%s\\%s.generated.c", gen_dir, rname );
            if ( !platform_fullpath( abs_p, rel, sizeof( abs_p ) ) )
                snprintf( abs_p, sizeof( abs_p ), "%s", rel );
            CC_APPEND( cc.sources, "%s%s", cc.sources[ 0 ] ? " " : "", abs_p );
        }
    }

    char  includes_path[ PATH_MAX ];
    char* includes_out = NULL;
    if ( g_include_track )
    {
        snprintf( includes_path, sizeof( includes_path ), "%s\\_includes.txt", obj_dir );
        includes_out = includes_path;
    }

    return cc_run_compile_cmd( &cc, target, config, obj_dir, "cl.rsp", includes_out );
}

/*==============================================================================================

    build_target_compile_single()

    Compiles one source file with the target's full flag/define/include set.
    Used by the -file flag. No /showIncludes, no includes file -- single-file
    compiles are not tracked incrementally.

==============================================================================================*/

bool
build_target_compile_single( build_context_t* ctx, target_info_t* target,
                              const char* obj_dir, const char* gen_dir, const char* file_path )
{
    compile_cmd_t cc     = { 0 };
    const char*   config = ( ctx->config == CONFIG_DEBUG ) ? "Debug" : "Release";

    cc_fill_compile_cmd( ctx, target, obj_dir, gen_dir, &cc );

    // Source: the single file passed via -file.
    CC_APPEND( cc.sources, "%s", file_path );

    return cc_run_compile_cmd( &cc, target, config, obj_dir, "cl_file.rsp", NULL );
}

/*==============================================================================================

    build_target_compile_only()

    Compile all units for a target without running the link/archive step.
    Called from the -compile-only dispatch path (VS Ctrl+F7 via
    NMakeCompileFileCommandLine). Ensures intermediate directories exist and
    runs reflection codegen first if the target requires it.

==============================================================================================*/

bool
build_target_compile_only( build_context_t* ctx, target_info_t* target )
{
    char obj_dir[ PATH_MAX ];
    snprintf( obj_dir, sizeof( obj_dir ), "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
    char gen_dir[ PATH_MAX ];
    snprintf( gen_dir, sizeof( gen_dir ), "%s\\%s", g_build_dir, g_gen_dir );
    char int_dir[ PATH_MAX ];
    snprintf( int_dir, sizeof( int_dir ), "%s\\%s", g_build_dir, g_int_dir );

    ensure_dir( g_build_dir );
    ensure_dir( int_dir );
    ensure_dir( gen_dir );
    ensure_dir( obj_dir );

    // Reflection must run before the compiler so <name>.generated.{c,h} exist.
    // Tool deps are always our responsibility even under -no-deps -- VS has no
    // knowledge of them -- so we always build if needed. build_target is
    // idempotent; the up-to-date check short-circuits when nothing changed.
    if ( target->has_reflect )
    {
        target_info_t* refl_tool = find_reflect_tool();
        if ( !refl_tool )
        {
            printf( ORB_INDENT "[orb error] '%s' no reflect_tool is registered\n", target->name );
            return false;
        }
        if ( build_target( ctx, refl_tool, NULL ) == false )
            return false;

        const char* rname = target->reflect_name ? target->reflect_name : target->name;
        printf( ORB_INDENT "[orb reflect] %s\n", rname );

        char refl_cmd[ PATH_MAX * 2 ];
        snprintf( refl_cmd, sizeof( refl_cmd ), "bin\\%s.exe %s %s %s",
                  refl_tool->name, target->root_dir, gen_dir, rname );

        if ( build_run_cmd( refl_cmd ) != 0 )
            return false;
    }

    return build_target_compile( ctx, target, obj_dir, gen_dir );
}

// clang-format on
/*============================================================================================*/
