/*==============================================================================================

    build_tool_08_link.c -- Linker and archiver command construction and execution.

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
    Verify the last byte of every link_cmd_t field is still '\0' before assembly.
==============================================================================================*/

static void
lk_check_overflow( const link_cmd_t* lk )
{
    if ( lk->exe     [ sizeof( lk->exe      ) - 1 ] || lk->artifact[ sizeof( lk->artifact ) - 1 ] ||
         lk->flags   [ sizeof( lk->flags    ) - 1 ] || lk->output  [ sizeof( lk->output   ) - 1 ] ||
         lk->pdb     [ sizeof( lk->pdb      ) - 1 ] || lk->inputs  [ sizeof( lk->inputs   ) - 1 ] ||
         lk->libs    [ sizeof( lk->libs     ) - 1 ] )
    {
        printf( ORB_INDENT "[orb error] link_cmd_t sentinel overwritten -- field overflow\n" );
        exit( 1 );
    }
}

/*==============================================================================================
    --- Link Section Printer ---

    Print link_cmd_t fields to `out` according to g_out_flags.
    Called before assembly so the user sees what is about to be run.
==============================================================================================*/

static void
lk_print( FILE* out, const link_cmd_t* lk, const target_info_t* target )
{
    ( void )target;
    if ( g_out_flags & ORB_OUT_SUMMARY_LINK )
        fprintf( out, ORB_INDENT "[orb link] %s\n", lk->artifact );

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

    // Inputs: all compiled objects in the target's obj dir -- platform glob.
    platform_lk_obj_pattern( obj_dir, lk.inputs, sizeof( lk.inputs ) );

    if ( effective_type == TARGET_STATIC_LIB )
    {
        // Static library: archive only, no PDB, no dep libs.
        platform_lk_fill_static( target->name, &lk );
    }
    else
    {
        // Executable or DLL: run pre-link cleanup, fill command fields, then
        // append dep libs and system libs.
        platform_lk_pre_link( target->name, ctx->config );
        platform_lk_fill_dynamic( ctx, target, &lk );

        for ( int i = 0; target->deps[ i ]; ++i )
        {
            target_info_t* dep      = find_target( target->deps[ i ] );
            target_type_t  dep_type = dep ? dep->type : TARGET_STATIC_LIB;
            // In monolithic mode every DLL dep was built as a static lib.
            if ( ctx->is_monolithic && dep_type == TARGET_DYNAMIC_LIB )
                dep_type = TARGET_STATIC_LIB;
            platform_lk_append_dep_lib( target->deps[ i ], dep_type, lk.libs, sizeof( lk.libs ) );
        }
        // Monolithic-only deps: modules that are runtime-loaded in modular builds
        // but must be explicitly linked when everything is compiled as static libs.
        if ( ctx->is_monolithic )
        {
            for ( int i = 0; target->mono_deps[ i ]; ++i )
            {
                target_info_t* dep      = find_target( target->mono_deps[ i ] );
                target_type_t  dep_type = dep ? dep->type : TARGET_DYNAMIC_LIB;
                // mono_deps are typically DLLs; in monolithic mode they are static libs.
                if ( dep_type == TARGET_DYNAMIC_LIB )
                    dep_type = TARGET_STATIC_LIB;
                platform_lk_append_dep_lib( target->mono_deps[ i ], dep_type, lk.libs, sizeof( lk.libs ) );
            }
        }
        platform_lk_append_sys_libs( lk.libs, sizeof( lk.libs ) );
    }

    // Print sections, assemble, optionally echo raw command, then run.
    FILE* log_out = log_open();
    lk_print( log_out, &lk, target );

    char      rsp_path[ PATH_MAX ];
    cmd_buf_t cmd = { 0 };
    snprintf( rsp_path, sizeof( rsp_path ), "%s" PATH_SEP "%s.rsp", obj_dir,
              effective_type == TARGET_STATIC_LIB ? "lib" : "link" );

    lk_check_overflow( &lk );
    lk_assemble( &lk, &cmd, rsp_path );
    if ( g_out_flags & ORB_OUT_LINK_CMD ) print_raw_cmd( log_out, cmd.buf );
    log_close( log_out );

    // NULL includes_path: no dep-tracking parse for link/lib, but line-by-line
    // capture still applies so compiler-output prefixing and gating work.
    return build_run_cmd_capture_includes( cmd.buf, NULL ) == 0;
}

// clang-format on
/*============================================================================================*/
