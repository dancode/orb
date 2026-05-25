/*==============================================================================================

    build_tool_07_link.c -- Linker and archiver command construction and execution.

    Assembles the link.exe / lib.exe command line for a single target and runs it.
    lib.exe is used for static libraries; link.exe handles both DLLs and executables.

    One public entry point:
      build_target_link() -- called from build_target() in 08_exec.c.

    PDB rotation:
      Each link produces a uniquely-timestamped bin/<name>_<ts>.pdb so the linker
      never has to overwrite a PDB an attached debugger holds open. Stale unlocked
      leftovers are garbage-collected by cleanup_stale_pdbs() before each link.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Link Command Struct ---
==============================================================================================*/

typedef struct
{
    char exe      [ 32       ];  // lib.exe or link.exe
    char artifact [ PATH_MAX ];  // final output path (for the summary display line)
    char flags    [ 256      ];  // /nologo /DLL ...
    char output   [ 512      ];  // /OUT:... /IMPLIB:...
    char pdb      [ 256      ];  // /DEBUG /PDB:... (empty for lib.exe)
    char inputs   [ 512      ];  // objDir/*.obj
    char libs     [ 1024     ];  // dep.lib ... user32.lib ...

} link_cmd_t;

/*==============================================================================================
    Verify the last byte of every link_cmd_t field is still '\0' before assembly.
==============================================================================================*/

static void
lk_check_overflow( const link_cmd_t* lk )
{
    if ( lk->exe     [ 32       - 1 ] || lk->artifact[ PATH_MAX - 1 ] ||
         lk->flags   [ 256      - 1 ] || lk->output  [ 512      - 1 ] ||
         lk->pdb     [ 256      - 1 ] || lk->inputs  [ 512      - 1 ] ||
         lk->libs    [ 1024     - 1 ] )
    {
        printf( ORB_INDENT "[orb error] link_cmd_t sentinel overwritten -- field overflow\n" );
        exit( 1 );
    }
}

/*==============================================================================================
    cleanup_stale_pdbs() -- remove PDB files from previous links.

    Each link emits a uniquely-named bin/<target>_<timestamp>.pdb so the linker
    never has to overwrite a PDB an attached debugger may hold open. This routine
    sweeps away unlocked leftovers at the start of each link.
    remove() silently fails for any PDB still locked by a debugger, which is
    correct -- the held file survives, the new link goes to a fresh name.
==============================================================================================*/

static void
cleanup_stale_pdbs( const char* target_name )
{
    char pattern[ PATH_MAX ];
    snprintf( pattern, sizeof( pattern ), "bin\\%s_*.pdb", target_name );

    struct _finddata_t fd;
    intptr_t h = _findfirst( pattern, &fd );
    if ( h == -1 ) return;

    do
    {
        char path[ PATH_MAX ];
        snprintf( path, sizeof( path ), "bin\\%s", fd.name );
        remove( path );
    }
    while ( _findnext( h, &fd ) == 0 );
    _findclose( h );
}

/*==============================================================================================
    --- Link Section Printer ---

    Print link_cmd_t fields to `out` according to g_out_flags.
    Called before assembly so the user sees what is about to be run.
==============================================================================================*/

static void
lk_print( FILE* out, const link_cmd_t* lk, const target_info_t* target )
{
    if ( g_out_flags & ORB_OUT_SUMMARY_LINK )
        fprintf( out, ORB_INDENT "[orb link] %s -> %s\n", target->name, lk->artifact );

    if ( g_out_flags & ORB_OUT_ANY_LINK )
        fprintf( out, "\n" );

    bool any = false;
    if ( g_out_flags & ORB_OUT_LINK_INPUTS ) any |= print_section( out, "inputs:",  lk->inputs, NULL );
    if ( g_out_flags & ORB_OUT_LINK_LIBS   ) any |= print_section( out, "libs:",    lk->libs,   NULL );
    if ( g_out_flags & ORB_OUT_LINK_FLAGS  ) any |= print_section( out, "flags:",   lk->flags,  NULL );
    if ( g_out_flags & ORB_OUT_LINK_OUTPUT ) any |= print_section( out, "output:",  lk->output, "/OUT:" );
    if ( g_out_flags & ORB_OUT_LINK_PDB    ) any |= print_section( out, "pdb:",     lk->pdb,    "/PDB:" );
    if ( any ) fprintf( out, "\n" );
}

/*==============================================================================================
    --- Link Command Assembly ---

    Join the link_cmd_t fields into a single string for the actual link/lib call.
    Response-file spill happens here when the assembled string exceeds the shell limit.
    The PDB section is optional: lib.exe doesn't take /DEBUG /PDB, so we omit
    that field when lk->pdb is empty.
==============================================================================================*/

static void
lk_assemble( const link_cmd_t* lk, cmd_buf_t* cmd, const char* rsp_path )
{
    if ( lk->pdb[ 0 ] )
        cmd_append( cmd, "%s %s %s %s %s %s", lk->exe, lk->flags, lk->output, lk->pdb, lk->inputs, lk->libs );
    else
        cmd_append( cmd, "%s %s %s %s %s",    lk->exe, lk->flags, lk->output, lk->inputs, lk->libs );

    cmd_spill_to_response_file( cmd, rsp_path );
}

/*==============================================================================================

    build_target_link()

    Fill a link_cmd_t, print active sections, assemble the command, and run it.
      TARGET_STATIC_LIB  -> lib.exe
      TARGET_DYNAMIC_LIB -> link.exe /DLL /IMPLIB
      TARGET_EXECUTABLE  -> link.exe

==============================================================================================*/

bool
build_target_link( build_context_t* ctx, target_info_t* target, const char* obj_dir )
{
    link_cmd_t lk = { 0 };

    // In monolithic mode dynamic modules are archived as static libs. Compute
    // an effective type once so the rest of the function branches cleanly.
    target_type_t effective_type = target->type;
    if ( ctx->is_monolithic && effective_type == TARGET_DYNAMIC_LIB )
        effective_type = TARGET_STATIC_LIB;

    if ( effective_type == TARGET_STATIC_LIB )
    {
        // --- Static library (lib.exe) ---
        // lib.exe bundles .obj files into a flat .lib archive. No linking,
        // no PDB, no dep resolution needed.
        snprintf( lk.exe,      sizeof( lk.exe ),      "lib.exe" );
        snprintf( lk.artifact, sizeof( lk.artifact ),  "bin\\%s.lib", target->name );
        snprintf( lk.flags,    sizeof( lk.flags ),     "/nologo" );
        snprintf( lk.output,   sizeof( lk.output ),    "/OUT:bin\\%s.lib", target->name );
        snprintf( lk.inputs,   sizeof( lk.inputs ),    "%s\\*.obj", obj_dir );
        // lk.pdb and lk.libs stay empty by zero-init -- lib.exe ignores both.
    }
    else
    {
        // --- Executable or DLL (link.exe) ---
        const char* ext = ( effective_type == TARGET_DYNAMIC_LIB ) ? ".dll" : ".exe";

        snprintf( lk.exe,      sizeof( lk.exe ),      "link.exe" );
        snprintf( lk.artifact, sizeof( lk.artifact ),  "bin\\%s%s", target->name, ext );
        snprintf( lk.inputs,   sizeof( lk.inputs ),    "%s\\*.obj", obj_dir );

        if ( effective_type == TARGET_DYNAMIC_LIB )
            snprintf( lk.flags, sizeof( lk.flags ), "/nologo /DLL" );
        else
            snprintf( lk.flags, sizeof( lk.flags ), "/nologo" );

        // DLLs also produce an import library (.lib) that dependents link against.
        if ( effective_type == TARGET_DYNAMIC_LIB )
            snprintf( lk.output, sizeof( lk.output ),
                      "/OUT:bin/%s.dll /IMPLIB:bin/%s.lib", target->name, target->name );
        else
            snprintf( lk.output, sizeof( lk.output ), "/OUT:bin\\%s.exe", target->name );

        // Uniquely-timestamped PDB so a debugger holding the previous one can
        // never block the linker. Stale unlocked leftovers are collected first.
        cleanup_stale_pdbs( target->name );
        snprintf( lk.pdb, sizeof( lk.pdb ),
                  "/DEBUG /PDB:bin/%s_%lld.pdb", target->name, ( long long )time( NULL ) );

        // Libraries: each declared dep's .lib, plus the four Windows system libs.
        for ( int i = 0; target->deps[ i ]; ++i )
            CC_APPEND( lk.libs, "%sbin/%s.lib", lk.libs[ 0 ] ? " " : "", target->deps[ i ] );
        CC_APPEND( lk.libs, "%suser32.lib shell32.lib gdi32.lib advapi32.lib",
                  lk.libs[ 0 ] ? " " : "" );
    }

    // Print sections, assemble, optionally echo raw command, then run.
    FILE* log_out = cc_open_log();
    lk_print( log_out, &lk, target );

    char      rsp_path[ PATH_MAX ];
    cmd_buf_t cmd = { 0 };
    snprintf( rsp_path, sizeof( rsp_path ), "%s\\%s.rsp", obj_dir,
              target->type == TARGET_STATIC_LIB ? "lib" : "link" );
    lk_check_overflow( &lk );
    lk_assemble( &lk, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_LINK_CMD ) print_raw_cmd( log_out, cmd.buf );
    cc_close_log( log_out );

    // NULL includes_path: no /showIncludes parsing for link/lib, but line-by-line
    // capture still applies so [MSVC] prefixing and ORB_OUT_MSVC_OUTPUT gating work.
    return build_run_cmd_capture_includes( cmd.buf, NULL ) == 0;
}

// clang-format on
/*============================================================================================*/
