/*==============================================================================================

    build_tool_09_exec.c -- Per-target build orchestration.

    build_target() is the core worker function. It builds one target by running
    the following phases in order:

      0. Dependency resolution  -- recurse into link deps, mono deps and tool deps first;
                                   locate the reflect tool and, via content_resolve(), res_tool.
      1. Per-target mutex lock  -- serialize concurrent invocations on the same target.
      2. Path preparation       -- obj_dir, gen_dir, out_path.
      2.5 Content cook (pre)    -- -content only: asset_tool over the cookable names in the
                                   previous manifest (shaders, recipes), into <build>/content.
      3. Up-to-date check       -- four freshness tests (A-D) on the compiled artifact. When it
                                   is current, a manifest that content_stale() reports as
                                   behind its content directories is refreshed in place
                                   (harvest, then cook under -content) and the target is
                                   skipped: no compile, no link.
      4. Directory creation     -- ensure every write destination exists.
      5. Locked-file management -- rename any in-use .exe aside before relinking.
      6. Reflection codegen     -- invoke reflect_tool if has_reflect is set.
      6.5 Resource manifest     -- content_refresh(): res_tool over the target's unit closure
                                   writes <name>_res_manifest.txt, then the cook runs again
                                   over the fresh manifest under -content.
      7. Compile + link         -- call 07_compile and 08_link; restore .exe on failure. A
                                   target with has_win_resources compiles its .rc first and
                                   links the .res in (Windows only).
      8. Config+mode stamp      -- touch _<config>_<mode>.stamp; delete the other 3 combos.

    Concurrency:
      From step 1 onward a per-target named mutex is held so two build_tool.exe
      invocations (or two parallel workers from 10_sched) targeting the same name
      serialize here. Independent targets run fully in parallel.

      skip_deps=true (set by the scheduler and VS -no-deps invocations) skips
      step 0 because the scheduler itself owns dep ordering -- re-recursing would
      visit every dep once per dependent and race shared outputs.

      skip_tool_deps=true (set by the scheduler only) additionally skips the
      tool_deps loop and implicit reflect tool dep in step 0. VS -no-deps still
      needs those built; the scheduler pre-wires them as graph deps via add_job().

    Content:
      Steps 2.5, 3 (refresh) and 6.5 live in 09_content.c and produce runtime data only. A
      failure in any of them is reported and the target still compiles and links;
      -strict-content restores hard failure. Step 6 (reflect_tool) is fatal in both modes:
      its output is compiled.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- RC Compile Helper (Windows only) ---

    Used in step 7 of build_target() for targets that carry has_win_resources.  Defined
    here (after 06_spawn.c in the unity include chain) so build_run_cmd() is in scope.

    platform_compile_rc() -- rc.exe: .rc -> .res (version-info resource)

    The caller has already established that rc_src exists; a false return means rc.exe
    itself failed, and the link then runs without the resource. Non-fatal by design: the
    binary is still valid, just without the metadata that reduces AV heuristic
    false-positives.

    The manifest half of the feature has no helper here -- it goes in as linker flags,
    see platform_lk_fill_dynamic() in build_tool_win_toolchain.c.

==============================================================================================*/

#if defined( _WIN32 )
static bool
platform_compile_rc( const char* rc_src, const char* res_out )
{
    char cmd[ PATH_MAX * 2 ];
    snprintf( cmd, sizeof( cmd ), "rc.exe /nologo /fo %s %s", res_out, rc_src );
    int ret = build_run_cmd( cmd );
    if ( ret != 0 )
        printf( ORB_INDENT "[orb warn] rc.exe failed (exit %d) -- version resource not embedded\n", ret );
    return ret == 0;
}
#endif

/*==============================================================================================
    --- Reflection Codegen ---

    reflect_tool scans the target's source tree and writes <gen_dir>/<name>.generated.{c,h},
    the type and field tables ref_ registers at startup.  The .c is a compile unit of the
    target, so this runs ahead of the compiler and a failure is fatal in every mode.

    Two build paths reach it -- step 6 of build_target() and build_target_compile_only(), the
    -compile-only route VS drives with Ctrl+F7 -- so the command line has one definition here
    and the two paths cannot generate different code.  The caller owns locating the tool
    (find_reflect_tool()), building it, and creating gen_dir.
==============================================================================================*/

bool
build_gen_reflect( target_info_t* target, const char* gen_dir, const target_info_t* refl_tool )
{
    const char* rname = target_reflect_name( target );

    if ( g_out_flags & ORB_OUT_REFLECT )
        log_printf( ORB_INDENT "[orb reflect] %s\n", rname );

    // Pass -silent when ORB_OUT_REFLECT is off so the tool produces no output.
    // build_run_cmd routes to the per-target log in a parallel worker automatically.
    const char* silent = ( g_out_flags & ORB_OUT_REFLECT ) ? "" : " -silent";
    char cmd[ PATH_MAX * 2 ];
    snprintf( cmd, sizeof( cmd ), "bin" PATH_SEP "%s.exe -src %s -out %s -name %s%s",
              refl_tool->name, target->root_dir, gen_dir, rname, silent );
    return build_run_cmd( cmd ) == 0;
}

bool
build_target( build_context_t* ctx, target_info_t* target, bool* out_skipped, uint64_t* out_elapsed_ms )
{
    if ( out_skipped    ) *out_skipped    = false;
    if ( out_elapsed_ms ) *out_elapsed_ms = 0;

    target_info_t* refl_tool = NULL;    // Located in step 0; reused in step 6.
    target_info_t* res_tool  = NULL;    // Located in step 0 when the target carries a manifest; reused in steps 3 and 6.5.

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

        // Mono deps -- built in both modes, linked only in a monolithic one (step 7). A host
        // names a module here because it loads it, so it includes that module's API header;
        // when the module's API struct and gateway come from reflect_tool (REF_MODULE), the
        // header does not exist until the module has built.
        for ( int i = 0; target->mono_deps[ i ]; ++i )
        {
            target_info_t* dep = find_target( target->mono_deps[ i ] );
            if ( !dep )
            {
                printf( ORB_INDENT "[orb error] '%s' has unknown mono_dep '%s'\n",
                        target->name, target->mono_deps[ i ] );
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

    // Implicit res tool dep -- same two paths as reflection: built on the serial path,
    // pre-wired as a graph dep by the scheduler. Unlike reflection, an unavailable res_tool
    // leaves res_tool NULL and the content steps are skipped: the manifest is a runtime
    // input, so the target still compiles unless -strict-content is set.
    if ( !content_resolve( ctx, target, &res_tool ) )
        return false;

    // --- 1. Per-Target Mutex Lock ---
    //
    // Acquired BEFORE the up-to-date check so a second concurrent invocation
    // observes post-build artifact mtimes -- never a half-written .obj/.lib.

    void* target_lock = build_lock_target( target->name );
    bool  result      = true;

    // --- 2. Path Preparation ---

    char obj_dir[ PATH_MAX ];
    path_obj_dir( target, obj_dir, sizeof( obj_dir ) );
    char gen_dir[ PATH_MAX ];
    path_gen_dir( gen_dir, sizeof( gen_dir ) );

    // --- 2.5 Content Cook (pre) ---
    //
    // A no-op without -content. Inside the lock for the same reason everything else is --
    // two invocations must not write one cooked file at once -- but ahead of the up-to-date
    // check, because a cooked file is an input to the RUNTIME and not to the compiler: a
    // shader edit must re-cook without dragging a recompile behind it, and an unchanged
    // shader must not make the target look stale. Works from the manifest the previous build
    // wrote; idempotent, and one mtime compare per cookable name when there is nothing to do.
    //
    // Returns directly rather than through cleanup: nothing has been renamed yet, and the
    // variables that label reads are not declared until after the up-to-date check. Only
    // -strict-content can produce that return; otherwise a cook failure is a warning.

    if ( !build_cook_content( ctx, target, obj_dir ) )
    {
        build_unlock_target( target_lock );
        return false;
    }

    const char* ext = ( target->type == TARGET_STATIC_LIB )    ? ".lib"
                    : ( target->type == TARGET_DYNAMIC_LIB )   ? ( ctx->is_monolithic ? ".lib" : ".dll" )
                                                               : ".exe";
    char out_path[ PATH_MAX ];
    snprintf( out_path, sizeof( out_path ), "bin" PATH_SEP "%s%s", target->name, ext );

    // --- 3. Up-to-Date Check ---
    //
    // Four independent freshness tests (A-D), each guarded by the running
    // up_to_date flag so we short-circuit out of expensive walks. A miss on
    // ANY test forces a full rebuild. Content is deliberately not one of them:
    // the manifest has its own staleness test below, and refreshing it never
    // recompiles.

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
        // The code is current. The manifest may not be: a content file added, removed or
        // renamed under a directory it lists, or a manifest that has never been written
        // (a name that failed to resolve, or a target built before manifests existed).
        // Refresh it here, in place -- harvest, then cook under -content -- and still report
        // the target as skipped: nothing was compiled or linked.
        if ( res_tool && content_stale( target, obj_dir, res_tool ) )
        {
            ensure_dir( obj_dir );
            if ( !content_refresh( ctx, target, obj_dir, res_tool ) )
            {
                result = false;    // only reachable under -strict-content
                goto cleanup;
            }
        }
        if ( out_skipped )
            *out_skipped = true;
        result = true;
        goto cleanup;
    }

    uint64_t t_build_start = platform_time_ms();

    // --- 4. Directory Creation ---

    {
        char int_root[ PATH_MAX ];
        path_obj_root( int_root, sizeof( int_root ) );
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

    if ( target->has_reflect && !build_gen_reflect( target, gen_dir, refl_tool ) )
    {
        result = false;
        goto cleanup;
    }

    // --- 6.5 Resource Manifest + Content Cook (post) ---
    //
    // A rebuild may add names the previous manifest did not carry; harvest now, and under
    // -content cook the additions so the target runs against their cooked form on this
    // same build.

    if ( res_tool && !content_refresh( ctx, target, obj_dir, res_tool ) )
    {
        result = false;    // only reachable under -strict-content
        goto cleanup;
    }

    // --- 7. Compile & Link ---

#if defined( _WIN32 )
    // A target that opted into Windows resources compiles its version-info resource
    // (<name>.rc -> <name>.res) and hands the result to the linker, so the image carries
    // the publisher metadata AV scanners and Windows Explorer use to identify it. A target
    // with the flag but no .rc, or a machine without rc.exe, links without one.
    char res_path[ PATH_MAX ] = { 0 };
    if ( target->has_win_resources )
    {
        // Resolved from root_dir, not from the CWD: when a child project builds the
        // engine's build_tool the sources live under the engine root, not under this
        // project's source/.
        char rc_src[ PATH_MAX ];
        snprintf( rc_src, sizeof( rc_src ), "%s/%s.rc", target->root_dir, target->name );
        if ( platform_file_exists( rc_src ) )
        {
            snprintf( res_path, sizeof( res_path ), "%s" PATH_SEP "%s.res", obj_dir, target->name );
            if ( g_out_flags & ORB_OUT_SUMMARY_COMPILE )
                printf( ORB_INDENT "[orb rc] %s\n", rc_src );
            if ( !platform_compile_rc( rc_src, res_path ) )
                res_path[ 0 ] = '\0';    // nothing for the linker to fold in
        }
    }
#endif

    if ( !build_target_compile( ctx, target, obj_dir, gen_dir ) )
    {
        result = false;
        goto cleanup;
    }

    if ( !build_target_link( ctx, target, obj_dir,
#if defined( _WIN32 )
                             res_path[ 0 ] ? res_path : NULL
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
    else if ( renamed )
    {
        // The new image is in place, so the copy held for rollback is dead weight -- and a
        // stale unsigned .exe is exactly what should not accumulate in bin/. The delete
        // fails while the old image is still mapped by a running process (Windows permits
        // the rename above but not the unlink), including build_tool rebuilding itself;
        // the next build of this target retries it.
        remove( old_path );
    }
    build_unlock_target( target_lock );
    return result;
}

// clang-format on
/*============================================================================================*/
