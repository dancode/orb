/*==============================================================================================

    build_tool_09_exec.c -- Per-target build orchestration.

    build_target() is the core worker function. It builds one target by running
    the following phases in order:

      0. Dependency resolution  -- recurse into link deps and tool deps first.
      1. Per-target mutex lock  -- serialize concurrent invocations on the same target.
      1.5 Shader cook           -- asset_tool over every 'shader' line, into bin/shaders.
      2. Path preparation       -- obj_dir, gen_dir, out_path.
      3. Up-to-date check       -- four freshness tests (A-D); short-circuit if clean.
      4. Directory creation     -- ensure every write destination exists.
      5. Locked-file management -- rename any in-use .exe aside before relinking.
      6. Reflection codegen     -- invoke reflect_tool if has_reflect is set.
      7. Compile + link         -- call 06_compile and 07_link; restore .exe on failure.
      8. Config+mode stamp      -- touch _<config>_<mode>.stamp; delete the other 3 combos.

    Concurrency:
      From step 1 onward a per-target named mutex is held so two build_tool.exe
      invocations (or two parallel workers from 09_sched) targeting the same name
      serialize here. Independent targets run fully in parallel.

      skip_deps=true (set by the scheduler and VS -no-deps invocations) skips
      step 0 because the scheduler itself owns dep ordering -- re-recursing would
      visit every dep once per dependent and race shared outputs.

      skip_tool_deps=true (set by the scheduler only) additionally skips the
      tool_deps loop and implicit reflect tool dep in step 0. VS -no-deps still
      needs those built; the scheduler pre-wires them as graph deps via add_job().

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- RC Compile and Manifest Embed Helpers (Windows only) ---

    Used in step 7 of build_target() when is_build_tool is true.  Defined here
    (after 06_spawn.c in the unity include chain) so build_run_cmd() is in scope.

    platform_compile_rc()     -- rc.exe: .rc -> .res (version-info resource)
    platform_embed_manifest() -- mt.exe: embed XML manifest into PE RT_MANIFEST

    Both are non-fatal: a warning is printed on failure and the binary is still
    usable, just without the metadata that reduces AV heuristic false-positives.

    Guarded by BUILD_TOOL_EMBED_MANIFEST to match the call site in step 7 -- without
    it the whole feature is dormant and the helpers would compile as dead code.

==============================================================================================*/

#if defined( _WIN32 ) && defined( BUILD_TOOL_EMBED_MANIFEST )
static bool
platform_compile_rc( const char* rc_src, const char* res_out )
{
    if ( !platform_file_exists( rc_src ) )
        return true;

    char cmd[ PATH_MAX * 2 ];
    snprintf( cmd, sizeof( cmd ), "rc.exe /nologo /fo %s %s", res_out, rc_src );
    int ret = build_run_cmd( cmd );
    if ( ret != 0 )
        printf( ORB_INDENT "[orb warn] rc.exe failed (exit %d) -- version resource not embedded\n", ret );
    return ret == 0;
}

static void
platform_embed_manifest( const char* exe_path, const char* manifest_src )
{
    if ( !platform_file_exists( manifest_src ) )
        return;

    char cmd[ PATH_MAX * 2 ];
    snprintf( cmd, sizeof( cmd ),
              "mt.exe -nologo -manifest %s -outputresource:%s;1", manifest_src, exe_path );
    int ret = build_run_cmd( cmd );
    if ( ret != 0 )
        printf( ORB_INDENT "[orb warn] mt.exe failed (exit %d) -- manifest not embedded\n", ret );
}
#endif

/*==============================================================================================
    --- Shader Cook ---

    Every 'shader' line on a target names a stage-tagged .hlsl under its root, and the cook
    turns it into bin/shaders/<stem>.oshd -- the reflected container the runtime loads.  The
    cooker is asset_tool, which reads the stage tag out of the filename (gui.vs.hlsl -> vs_6_0)
    and forwards to shader_tool; both must therefore be listed as tool deps by any target that
    declares shaders, which is also what orders them ahead of it under the parallel scheduler.

    Staleness is a plain mtime compare against the .oshd.  It deliberately does NOT consult the
    target's artifact: a shader and the C code that draws with it change on their own schedules,
    and coupling them would either recompile the world after a one-line shader edit or leave a
    stale .oshd next to a fresh .lib.
==============================================================================================*/

bool
build_cook_shaders( build_context_t* ctx, target_info_t* target )
{
    if ( !target->shaders[ 0 ] )
        return true;

    if ( !platform_file_exists( "bin" PATH_SEP "asset_tool.exe" ) )
    {
        printf( ORB_INDENT "[orb error] '%s' declares shaders but bin/asset_tool.exe is missing"
                           " -- add 'tool_dep asset_tool shader_tool' to the target\n", target->name );
        return false;
    }

    ensure_dir( "bin" PATH_SEP "shaders" );

    for ( int i = 0; target->shaders[ i ]; ++i )
    {
        char src[ PATH_MAX ];
        snprintf( src, sizeof( src ), "%s" PATH_SEP "%s", target->root_dir, target->shaders[ i ] );

        /* The output name is the source's, with .hlsl traded for .oshd -- the stage tag stays
           part of the stem, so gui.vs.hlsl and gui.ps.hlsl land as a distinguishable pair. */
        const char* base = target->shaders[ i ];
        for ( const char* p = base; *p; ++p )
            if ( *p == '/' || *p == '\\' )
                base = p + 1;

        char stem[ PATH_MAX ];
        snprintf( stem, sizeof( stem ), "%s", base );
        char* dot = strrchr( stem, '.' );
        if ( !dot || strcmp( dot, ".hlsl" ) != 0 )
        {
            printf( ORB_INDENT "[orb error] '%s' shader '%s' is not a .hlsl\n",
                    target->name, target->shaders[ i ] );
            return false;
        }
        *dot = '\0';

        char dst[ PATH_MAX ];
        snprintf( dst, sizeof( dst ), "bin" PATH_SEP "shaders" PATH_SEP "%s.oshd", stem );

        platform_mtime_t src_mtime = platform_get_mtime( src );
        if ( src_mtime == 0 )
        {
            printf( ORB_INDENT "[orb error] '%s' shader source not found: %s\n", target->name, src );
            return false;
        }

        /* Keyed on the OUTPUT, not on this target.  Two targets built from one source tree can
           declare the same shader, and under the parallel scheduler they reach this at the same
           moment; so can two build_tool invocations.  The staleness test is inside the lock with
           the cook, so the loser of the race sees the winner's fresh .oshd and skips instead of
           writing the same file underneath it -- which is what a shared cook temp turns into. */
        void* cook_lock = build_lock_target( stem );

        if ( !ctx->force_rebuild && platform_get_mtime( dst ) >= src_mtime )
        {
            build_unlock_target( cook_lock );
            continue;
        }

        if ( g_out_flags & ORB_OUT_REFLECT )
        {
            const char* lp = sched_log_path();
            FILE*       lf = lp ? fopen( lp, "a" ) : NULL;
            fprintf( lf ? lf : stdout, ORB_INDENT "[orb shader] %s\n", stem );
            if ( lf ) fclose( lf );
        }

        char cmd[ PATH_MAX * 2 ];
        snprintf( cmd, sizeof( cmd ), "bin" PATH_SEP "asset_tool.exe cook %s %s", src, dst );
        int ret = build_run_cmd( cmd );
        build_unlock_target( cook_lock );
        if ( ret != 0 )
        {
            printf( ORB_INDENT "[orb error] '%s' shader cook failed: %s\n", target->name, src );
            return false;
        }
    }

    return true;
}

bool
build_target( build_context_t* ctx, target_info_t* target, bool* out_skipped, uint64_t* out_elapsed_ms )
{
    if ( out_skipped    ) *out_skipped    = false;
    if ( out_elapsed_ms ) *out_elapsed_ms = 0;

    target_info_t* refl_tool = NULL;    // Located in step 0; reused in step 6.

    // --- 0. Dependency Resolution ---

    if ( !ctx->skip_deps )
    {
        // Link dependencies -- build each dep before this target.
        for ( int i = 0; target->deps[ i ]; ++i )
        {
            target_info_t* dep = find_target( target->deps[ i ] );
            if ( !dep )
            {
                printf( ORB_INDENT "[orb error] '%s' depends on unknown target '%s'\n",
                        target->name, target->deps[ i ] );
                return false;
            }
            if ( !build_target( ctx, dep, NULL, NULL ) )
                return false;
        }
    }

    // Tool dependencies -- skipped when the scheduler already owns dep ordering.
    // VS -no-deps sets skip_deps but not skip_tool_deps; it still needs tools built.
    if ( !ctx->skip_tool_deps )
    {
        for ( int i = 0; target->tool_deps[ i ]; ++i )
        {
            target_info_t* tool = find_target( target->tool_deps[ i ] );
            if ( !tool )
            {
                printf( ORB_INDENT "[orb error] '%s' has unknown tool dep '%s'\n",
                        target->name, target->tool_deps[ i ] );
                return false;
            }
            if ( !build_target( ctx, tool, NULL, NULL ) )
                return false;
        }

        // Implicit reflect tool dep -- same always-rebuild guarantee as tool_deps.
        if ( target->has_reflect )
        {
            refl_tool = find_reflect_tool();
            if ( !refl_tool )
            {
                printf( ORB_INDENT "[orb error] '%s' needs reflection but no is_reflect_tool target is registered\n",
                        target->name );
                return false;
            }
            if ( !build_target( ctx, refl_tool, NULL, NULL ) )
                return false;
        }
    }
    else if ( target->has_reflect )
    {
        // Scheduler path: reflect tool was already built as a graph dep.
        // We still need refl_tool for the codegen invocation in step 6.
        refl_tool = find_reflect_tool();
        if ( !refl_tool )
        {
            printf( ORB_INDENT "[orb error] '%s' needs reflection but no is_reflect_tool target is registered\n",
                    target->name );
            return false;
        }
    }

    // --- 1. Per-Target Mutex Lock ---
    //
    // Acquired BEFORE the up-to-date check so a second concurrent invocation
    // observes post-build artifact mtimes -- never a half-written .obj/.lib.

    void* target_lock = build_lock_target( target->name );
    bool  result      = true;

    // --- 1.5 Shader Cook ---
    //
    // Inside the lock for the same reason everything else is -- two invocations must not
    // write one .oshd at once -- but ahead of the up-to-date check, because the cooked file
    // is an input to the RUNTIME and not to the compiler: a shader edit must re-cook without
    // dragging a recompile behind it, and an unchanged shader must not make the target look
    // stale. Idempotent, and one mtime compare per declared shader when there is nothing to do.
    //
    // Returns directly rather than through cleanup: nothing has been renamed yet, and the
    // variables that label reads are not declared until after the up-to-date check.

    if ( !build_cook_shaders( ctx, target ) )
    {
        build_unlock_target( target_lock );
        return false;
    }

    // --- 2. Path Preparation ---

    char obj_dir[ PATH_MAX ];
    snprintf( obj_dir, sizeof( obj_dir ), "%s" PATH_SEP "%s" PATH_SEP "%s", g_build_dir, g_int_dir, target->name );
    char gen_dir[ PATH_MAX ];
    snprintf( gen_dir, sizeof( gen_dir ), "%s" PATH_SEP "%s", g_build_dir, g_gen_dir );

    const char* ext = ( target->type == TARGET_STATIC_LIB )    ? ".lib"
                    : ( target->type == TARGET_DYNAMIC_LIB )   ? ( ctx->is_monolithic ? ".lib" : ".dll" )
                                                               : ".exe";
    char out_path[ PATH_MAX ];
    snprintf( out_path, sizeof( out_path ), "bin" PATH_SEP "%s%s", target->name, ext );

    // --- 3. Up-to-Date Check ---
    //
    // Four independent freshness tests (A-D), each guarded by the running
    // up_to_date flag so we short-circuit out of expensive walks. A miss on
    // ANY test forces a full rebuild.

    platform_mtime_t out_mtime  = platform_get_mtime( out_path );
    bool       up_to_date = ( out_mtime != 0 ) && !ctx->force_rebuild;

    // A. Any explicit translation unit newer than the artifact?
    if ( up_to_date )
    {
        for ( int i = 0; target->units[ i ]; ++i )
        {
            char src_path[ PATH_MAX ];
            snprintf( src_path, sizeof( src_path ), "%s" PATH_SEP "%s", target->root_dir, target->units[ i ] );
            if ( platform_get_mtime( src_path ) > out_mtime )
            {
                up_to_date = false;
                break;
            }
        }
    }

    // B. Any linked dep .lib newer than the artifact?
    //    Catches the case where a sibling target rebuilt and we need to re-link.
    if ( up_to_date )
    {
        for ( int i = 0; target->deps[ i ]; ++i )
        {
            char dep_path[ PATH_MAX ];
            snprintf( dep_path, sizeof( dep_path ), "bin" PATH_SEP "%s.lib", target->deps[ i ] );
            if ( platform_get_mtime( dep_path ) > out_mtime )
            {
                up_to_date = false;
                break;
            }
        }
    }

    // C. Config + mode change check. A per-(config,mode) stamp file
    //    (_debug_mono.stamp / _debug_modular.stamp / _release_*.stamp) is created after
    //    every successful compile+link. Presence of the correct one is the signal --
    //    no file content to read or compare. Mode matters because a dynamic target's
    //    output collides by name across modes: modular emits bin/<name>.dll plus a small
    //    import bin/<name>.lib, while monolithic emits a full static bin/<name>.lib. Without
    //    the mode in the stamp, a modular->monolithic switch would see the stale import lib,
    //    pass every freshness test, and link a lib that has no real symbol definitions.
    if ( up_to_date )
    {
        char config_stamp[ PATH_MAX ];
        snprintf( config_stamp, sizeof( config_stamp ), "%s" PATH_SEP "_%s_%s.stamp", obj_dir,
                  ctx->config == CONFIG_DEBUG ? "debug" : "release",
                  ctx->is_monolithic ? "mono" : "modular" );
        if ( !platform_file_exists( config_stamp ) )
            up_to_date = false;
    }

    // D. Header include check. The previous compile wrote every #included header
    //    path into <obj_dir>/_includes.txt (flattened from the compiler's per-unit
    //    dep files). Replay that list and rebuild if any header is newer than the
    //    artifact. Skipped when -no-include-track is set.
    if ( up_to_date && g_include_track )
    {
        char includes_path[ PATH_MAX ];
        snprintf( includes_path, sizeof( includes_path ), "%s" PATH_SEP "_includes.txt", obj_dir );

        platform_mapped_file_t inc_map;
        if ( !platform_map_file( includes_path, &inc_map ) )
        {
            // No file = no recorded header set = assume stale.
            up_to_date = false;
        }
        else if ( inc_map.size > 0 )
        {
            // Walk mapped bytes directly; no fgets buffering or CRT overhead.
            const char* p   = inc_map.data;
            const char* end = inc_map.data + inc_map.size;
            // Each line is a header path, e.g. "C:\path\to\header.h".
            char header_path[ PATH_MAX ];
            while ( up_to_date && mmap_next_line( &p, end, header_path, sizeof( header_path ) ) )
            {
                if ( !header_path[ 0 ] ) continue;
                // mtime 0 means the header was deleted -- treat as forced rebuild
                // so the compiler surfaces the missing include as an error.
                platform_mtime_t h_mtime = platform_get_mtime( header_path );
                if ( h_mtime == 0 || h_mtime > out_mtime )
                    up_to_date = false;
            }
            platform_unmap_file( &inc_map );
        }
        // Empty file (size == 0): no headers recorded, nothing to check, stay up to date.
    }

    // Declared here so cleanup: can access them regardless of which goto fires.
    char exe_path[ PATH_MAX ] = { 0 };
    char old_path[ PATH_MAX ] = { 0 };
    bool renamed              = false;

    if ( up_to_date )
    {
        if ( out_skipped )
            *out_skipped = true;
        result = true;
        goto cleanup;
    }

    uint64_t t_build_start = platform_time_ms();

    // --- 4. Directory Creation ---

    {
        char int_root[ PATH_MAX ];
        snprintf( int_root, sizeof( int_root ), "%s" PATH_SEP "%s", g_build_dir, g_int_dir );
        ensure_dir( "bin" );
        ensure_dir( g_build_dir );
        ensure_dir( int_root );
        ensure_dir( gen_dir );
        ensure_dir( obj_dir );
    }

    // --- 5. Locked-File Management ---
    //
    // Windows refuses to overwrite a running .exe (sharing violation), but WILL
    // let you rename one. Shove the old image to <name>.exe.old first; restore
    // it if compile or link fails; let it be overwritten on the next success.

    if ( target->type == TARGET_EXECUTABLE )
    {
        snprintf( exe_path, sizeof( exe_path ), "bin" PATH_SEP "%s.exe", target->name );
        snprintf( old_path, sizeof( old_path ), "bin" PATH_SEP "%s.exe.old", target->name );
        if ( platform_file_exists( exe_path ) )
        {
            remove( old_path );
            if ( rename( exe_path, old_path ) == 0 )
                renamed = true;
        }
    }

    // --- 6. Reflection Codegen ---

    if ( target->has_reflect )
    {
        const char* rname   = target->reflect_name ? target->reflect_name : target->name;
        const char* log_path = sched_log_path();

        // Header line: route to per-target log in a parallel worker, stdout otherwise.
        if ( g_out_flags & ORB_OUT_REFLECT )
        {
            FILE* lf = log_path ? fopen( log_path, "a" ) : NULL;
            fprintf( lf ? lf : stdout, ORB_INDENT "[orb reflect] %s\n", rname );
            if ( lf ) fclose( lf );
        }

        // Pass -silent when ORB_OUT_REFLECT is off so the tool produces no output.
        // build_run_cmd routes to the per-target log in a parallel worker automatically.
        const char* silent = ( g_out_flags & ORB_OUT_REFLECT ) ? "" : " -silent";
        char refl_cmd[ PATH_MAX * 2 ];
        snprintf( refl_cmd, sizeof( refl_cmd ), "bin" PATH_SEP "%s.exe -src %s -out %s -name %s%s",
                  refl_tool->name, target->root_dir, gen_dir, rname, silent );
        if ( build_run_cmd( refl_cmd ) != 0 )
        {
            result = false;
            goto cleanup;
        }
    }

    // --- 7. Compile & Link ---

#if defined( _WIN32 ) && defined( BUILD_TOOL_EMBED_MANIFEST )
    // For the build_tool target only: compile the version-info resource (.rc -> .res)
    // and pass it to the linker so the binary carries publisher metadata that AV
    // scanners and Windows Explorer use to identify the executable.  Both steps are
    // non-fatal warnings; the binary is still valid if rc.exe or mt.exe are absent.
    // Guard: define BUILD_TOOL_EMBED_MANIFEST at compile time to enable.
    char res_path[ PATH_MAX ] = { 0 };
    if ( target->is_build_tool )
    {
        const char* rc_src  = "source" PATH_SEP "tools" PATH_SEP "build_tool"
                              PATH_SEP "build_tool.rc";
        snprintf( res_path, sizeof( res_path ), "%s" PATH_SEP "build_tool.res", obj_dir );
        if ( g_out_flags & ORB_OUT_SUMMARY_COMPILE )
            printf( ORB_INDENT "[orb rc] %s\n", rc_src );
        platform_compile_rc( rc_src, res_path );
    }
#endif

    if ( !build_target_compile( ctx, target, obj_dir, gen_dir ) )
    {
        result = false;
        goto cleanup;
    }

    if ( !build_target_link( ctx, target, obj_dir,
#if defined( _WIN32 ) && defined( BUILD_TOOL_EMBED_MANIFEST )
                             ( target->is_build_tool && res_path[ 0 ] ) ? res_path : NULL
#else
                             NULL
#endif
    ) )
    {
        result = false;
        goto cleanup;
    }


    if ( out_elapsed_ms )
        *out_elapsed_ms = platform_time_ms() - t_build_start;

    // --- 8. Config + Mode Stamp ---
    // Create the stamp for the (config,mode) just built; delete the other three combos so
    // any Debug<->Release or modular<->monolithic switch is detected as a miss on the next
    // check. The mode axis is what makes a modular->monolithic switch rebuild the dynamic
    // targets as real static archives instead of reusing the stale DLL import libs.
    {
        const char* cfg  = ctx->config == CONFIG_DEBUG ? "debug" : "release";
        const char* mode = ctx->is_monolithic ? "mono" : "modular";
        char good_stamp[ PATH_MAX ];
        snprintf( good_stamp, sizeof( good_stamp ), "%s" PATH_SEP "_%s_%s.stamp", obj_dir, cfg, mode );
        platform_touch_file( good_stamp );

        static const char* k_cfgs[]  = { "debug", "release" };
        static const char* k_modes[] = { "mono", "modular" };
        for ( int ci = 0; ci < 2; ++ci )
            for ( int mi = 0; mi < 2; ++mi )
            {
                if ( strcmp( k_cfgs[ ci ], cfg ) == 0 && strcmp( k_modes[ mi ], mode ) == 0 )
                    continue; /* keep the one we just built */
                char bad_stamp[ PATH_MAX ];
                snprintf( bad_stamp, sizeof( bad_stamp ), "%s" PATH_SEP "_%s_%s.stamp", obj_dir,
                          k_cfgs[ ci ], k_modes[ mi ] );
                platform_delete_file( bad_stamp );
            }
    }

cleanup:
    if ( renamed && !result )
    {
        if ( rename( old_path, exe_path ) != 0 )
            fprintf( stderr, "[orb warn] failed to restore %s from %s\n", exe_path, old_path );
    }
    build_unlock_target( target_lock );
    return result;
}

// clang-format on
/*============================================================================================*/
