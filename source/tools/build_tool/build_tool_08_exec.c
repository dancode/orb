/*==============================================================================================

    build_tool_08_exec.c -- Per-target build orchestration.

    build_target() is the core worker function. It builds one target by running
    the following phases in order:

      0. Dependency resolution  -- recurse into link deps and tool deps first.
      1. Path preparation       -- obj_dir, gen_dir, out_path.
      2. Per-target mutex lock  -- serialize concurrent invocations on the same target.
      3. Up-to-date check       -- four freshness tests (A-D); short-circuit if clean.
      4. Directory creation     -- ensure every write destination exists.
      5. Locked-file management -- rename any in-use .exe aside before relinking.
      6. Reflection codegen     -- invoke reflect_tool if has_reflect is set.
      7. Compile + link         -- call 06_compile and 07_link; restore .exe on failure.
      8. Config stamp           -- write _config.txt so the next check detects mode flips.

    Concurrency:
      From step 2 onward a per-target named mutex is held so two build_tool.exe
      invocations (or two parallel workers from 09_sched) targeting the same name
      serialize here. Independent targets run fully in parallel.

      skip_deps=true (set by the scheduler and VS -no-deps invocations) skips
      step 0 because the scheduler itself owns dep ordering -- re-recursing would
      visit every dep once per dependent and race shared outputs.

==============================================================================================*/
// clang-format off

bool
build_target( build_context_t* ctx, target_info_t* target, bool* out_skipped )
{
    if ( out_skipped )
        *out_skipped = false;

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
            if ( !build_target( ctx, dep, NULL ) )
                return false;
        }
    }

    // Tool dependencies -- always our responsibility regardless of -no-deps.
    // VS has no visibility into tool executables not listed in the solution.
    for ( int i = 0; target->tool_deps[ i ]; ++i )
    {
        target_info_t* tool = find_target( target->tool_deps[ i ] );
        if ( !tool )
        {
            printf( ORB_INDENT "[orb error] '%s' has unknown tool dep '%s'\n",
                    target->name, target->tool_deps[ i ] );
            return false;
        }
        if ( !build_target( ctx, tool, NULL ) )
            return false;
    }

    // Implicit reflect tool dep -- same always-rebuild guarantee.
    if ( target->has_reflect )
    {
        refl_tool = find_reflect_tool();
        if ( !refl_tool )
        {
            printf( ORB_INDENT "[orb error] '%s' needs reflection but no is_reflect_tool target is registered\n",
                    target->name );
            return false;
        }
        if ( !build_target( ctx, refl_tool, NULL ) )
            return false;
    }

    // --- 2. Per-Target Mutex Lock ---
    //
    // Acquired BEFORE the up-to-date check so a second concurrent invocation
    // observes post-build artifact mtimes -- never a half-written .obj/.lib.

    void* target_lock = build_lock_target( target->name );
    bool  result      = true;

    // --- 1. Path Preparation ---

    char obj_dir[ PATH_MAX ];
    snprintf( obj_dir, sizeof( obj_dir ), "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
    char gen_dir[ PATH_MAX ];
    snprintf( gen_dir, sizeof( gen_dir ), "%s\\%s", g_build_dir, g_gen_dir );

    const char* ext = ( target->type == TARGET_STATIC_LIB )    ? ".lib"
                    : ( target->type == TARGET_DYNAMIC_LIB )   ? ( ctx->is_monolithic ? ".lib" : ".dll" )
                                                               : ".exe";
    char out_path[ PATH_MAX ];
    snprintf( out_path, sizeof( out_path ), "bin\\%s%s", target->name, ext );

    // --- 3. Up-to-Date Check ---
    //
    // Four independent freshness tests (A-D), each guarded by the running
    // up_to_date flag so we short-circuit out of expensive walks. A miss on
    // ANY test forces a full rebuild.

    platform_mtime_t out_mtime  = build_get_mtime( out_path );
    bool       up_to_date = ( out_mtime != 0 ) && !ctx->force_rebuild;

    // A. Any explicit translation unit newer than the artifact?
    if ( up_to_date )
    {
        for ( int i = 0; target->units[ i ]; ++i )
        {
            char src_path[ PATH_MAX ];
            snprintf( src_path, sizeof( src_path ), "%s\\%s", target->root_dir, target->units[ i ] );
            if ( build_get_mtime( src_path ) > out_mtime )
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
            snprintf( dep_path, sizeof( dep_path ), "bin\\%s.lib", target->deps[ i ] );
            if ( build_get_mtime( dep_path ) > out_mtime )
            {
                up_to_date = false;
                break;
            }
        }
    }

    // C. Config change check. _config.txt is written after every successful
    //    compile+link. Missing = first build or post-clean = must rebuild.
    if ( up_to_date )
    {
        const char* current_config = ( ctx->config == CONFIG_DEBUG ) ? "Debug" : "Release";
        char        config_marker[ PATH_MAX ];
        snprintf( config_marker, sizeof( config_marker ), "%s\\_config.txt", obj_dir );
        FILE* cf = fopen( config_marker, "r" );
        if ( !cf )
        {
            up_to_date = false;
        }
        else
        {
            char stored[ 16 ] = { 0 };
            fgets( stored, sizeof( stored ), cf );
            fclose( cf );
            strip_eol( stored );
            if ( strcmp( stored, current_config ) != 0 )
                up_to_date = false;
        }
    }

    // D. Header include check. The previous compile wrote every #included header
    //    path into <obj_dir>/_includes.txt (from cl.exe's /showIncludes output).
    //    Replay that list and rebuild if any header is newer than the artifact.
    //    Skipped when -no-include-track is set.
    if ( up_to_date && g_include_track )
    {
        char includes_path[ PATH_MAX ];
        snprintf( includes_path, sizeof( includes_path ), "%s\\_includes.txt", obj_dir );
        FILE* includes = fopen( includes_path, "r" );
        if ( !includes )
        {
            // No file = no recorded header set = assume stale.
            up_to_date = false;
        }
        else
        {
            char header_path[ PATH_MAX ];
            while ( fgets( header_path, sizeof( header_path ), includes ) )
            {
                strip_eol( header_path );
                if ( header_path[ 0 ] == '\0' )
                    continue;

                // mtime 0 means the header was deleted -- treat as forced rebuild
                // so the compiler surfaces the missing include as an error.
                platform_mtime_t h_mtime = build_get_mtime( header_path );
                if ( h_mtime == 0 || h_mtime > out_mtime )
                {
                    up_to_date = false;
                    break;
                }
            }
            fclose( includes );
        }
    }

    if ( up_to_date )
    {
        if ( out_skipped )
            *out_skipped = true;
        result = true;
        goto cleanup;
    }

    // --- 4. Directory Creation ---

    {
        char int_root[ PATH_MAX ];
        snprintf( int_root, sizeof( int_root ), "%s\\%s", g_build_dir, g_int_dir );
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

    char exe_path[ PATH_MAX ] = { 0 };
    char old_path[ PATH_MAX ] = { 0 };
    bool renamed              = false;

    if ( target->type == TARGET_EXECUTABLE )
    {
        snprintf( exe_path, sizeof( exe_path ), "bin\\%s.exe", target->name );
        snprintf( old_path, sizeof( old_path ), "bin\\%s.exe.old", target->name );
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
        const char* rname = target->reflect_name ? target->reflect_name : target->name;
        if ( g_out_flags & ORB_OUT_REFLECT )
        {
            // Route to the per-target log when inside a parallel worker.
            const char* _lp = sched_log_path();
            FILE*       _lf = _lp ? fopen( _lp, "a" ) : NULL;
            fprintf( _lf ? _lf : stdout, ORB_INDENT "[orb reflect] %s\n", rname );
            if ( _lf ) fclose( _lf );
        }
        char refl_cmd[ PATH_MAX * 2 ];
        snprintf( refl_cmd, sizeof( refl_cmd ), "bin\\%s.exe %s %s %s",
                  refl_tool->name, target->root_dir, gen_dir, rname );
        if ( build_run_cmd( refl_cmd ) != 0 )
        {
            if ( renamed ) rename( old_path, exe_path );
            result = false;
            goto cleanup;
        }
    }

    // --- 7. Compile & Link ---

    if ( !build_target_compile( ctx, target, obj_dir, gen_dir ) )
    {
        if ( renamed ) rename( old_path, exe_path );
        result = false;
        goto cleanup;
    }

    if ( !build_target_link( ctx, target, obj_dir ) )
    {
        if ( renamed ) rename( old_path, exe_path );
        result = false;
        goto cleanup;
    }

    // --- 8. Config Stamp ---
    // Record the config used so the next incremental check detects a
    // Debug<->Release switch even when no source file changed.
    {
        char config_marker[ PATH_MAX ];
        snprintf( config_marker, sizeof( config_marker ), "%s\\_config.txt", obj_dir );
        FILE* cf = fopen( config_marker, "w" );
        if ( cf )
        {
            fprintf( cf, "%s\n", ctx->config == CONFIG_DEBUG ? "Debug" : "Release" );
            fclose( cf );
        }
    }

cleanup:
    build_unlock_target( target_lock );
    return result;
}

// clang-format on
/*============================================================================================*/
