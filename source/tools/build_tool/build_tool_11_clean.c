/*==============================================================================================

    build_tool_11_clean.c -- Artifact cleanup for the -clean command.

    Two clean modes:

      Per-target (target != NULL) -- removes only that target's artifacts:

        bin/<name>.{lib,dll,exe,exp,pdb}, obj/<name>/, and generated reflection files.
        Called from each VS .vcxproj's NMakeCleanCommandLine so a solution rebuild
        cleans each project independently rather than wiping the whole bin/ tree.

      Global (target == NULL) -- wipes all intermediates and artifacts.

        Skips is_tool executables (reflect_tool, build_tool) so tools survive a
        full clean. They are rebuilt on demand by dep resolution, not by VS, so
        deleting them would leave no path to recreate them.

    BUILD_SAFE_MODE: uses Win32 API (DeleteFileA / RemoveDirectoryA) directly so
    no cmd.exe child process is spawned. Original del/rd path preserved under #else.

==============================================================================================*/
// clang-format off

#if defined( BUILD_SAFE_MODE )

/*==============================================================================================
    Safe-mode clean: Win32 API only, no cmd.exe or shell built-ins.
==============================================================================================*/

void
build_clean( target_info_t* target )
{
    char path[ PATH_MAX ];

    if ( target )
    {
        const char* ext = ( target->type == TARGET_STATIC_LIB )  ? "lib"
                        : ( target->type == TARGET_DYNAMIC_LIB ) ? "dll"
                        :                                          "exe";

        snprintf( path, sizeof( path ), "bin\\%s.%s", target->name, ext );
        platform_delete_file_quiet( path );

        if ( target->type == TARGET_DYNAMIC_LIB )
        {
            // Cover both monolithic (.lib primary) and dynamic (.dll primary)
            // outputs plus the import .exp.
            snprintf( path, sizeof( path ), "bin\\%s.lib", target->name );
            platform_delete_file_quiet( path );
            snprintf( path, sizeof( path ), "bin\\%s.dll", target->name );
            platform_delete_file_quiet( path );
            snprintf( path, sizeof( path ), "bin\\%s.exp", target->name );
            platform_delete_file_quiet( path );
        }

        char pdb_glob[ 128 ];
        snprintf( pdb_glob, sizeof( pdb_glob ), "%s_*.pdb", target->name );
        platform_delete_glob_quiet( "bin", pdb_glob );

        char obj_dir[ PATH_MAX ];
        snprintf( obj_dir, sizeof( obj_dir ), "%s\\%s\\%s", g_build_dir, g_int_dir, target->name );
        platform_rmdir_quiet( obj_dir );

        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;
            snprintf( path, sizeof( path ), "%s\\%s\\%s.generated.c", g_build_dir, g_gen_dir, rname );
            platform_delete_file_quiet( path );
            snprintf( path, sizeof( path ), "%s\\%s\\%s.generated.h", g_build_dir, g_gen_dir, rname );
            platform_delete_file_quiet( path );
        }

        printf( ORB_BANNER "[orb clean] %s -- bin\\%s.%s, %s\\%s\\%s%s\n",
                target->name, target->name, ext,
                g_build_dir, g_int_dir, target->name,
                target->has_reflect ? " (+reflect)" : "" );
    }
    else
    {
        // Remove the entire obj and generated subtrees; the next build's
        // ensure_dir() calls recreate them.
        char obj_root[ PATH_MAX ], gen_root[ PATH_MAX ];
        snprintf( obj_root, sizeof( obj_root ), "%s\\%s", g_build_dir, g_int_dir );
        snprintf( gen_root, sizeof( gen_root ), "%s\\%s", g_build_dir, g_gen_dir );
        platform_rmdir_quiet( obj_root );
        platform_rmdir_quiet( gen_root );

        platform_delete_glob_quiet( "bin", "*.pdb" );
        platform_delete_glob_quiet( "bin", "*.lib" );
        platform_delete_glob_quiet( "bin", "*.dll" );
        platform_delete_glob_quiet( "bin", "*.exp" );

        // Delete executables only for non-tool targets. is_tool executables
        // (reflect_tool, build_tool) are rebuilt by dep resolution and have no
        // VS project to rebuild them after a clean, so leave them in place.
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( g_targets[ i ].is_external ) continue;
            if ( g_targets[ i ].type == TARGET_EXECUTABLE && !g_targets[ i ].is_tool )
            {
                snprintf( path, sizeof( path ), "bin\\%s.exe", g_targets[ i ].name );
                platform_delete_file_quiet( path );
            }
        }

        printf( ORB_BANNER "[orb clean] all -- bin\\*, %s\\{%s,%s}\\*\n",
                g_build_dir, g_int_dir, g_gen_dir );
    }
}

#else

/*==============================================================================================
    Standard clean: cmd.exe / del / rd path (original implementation).
==============================================================================================*/

/* Silently delete a file or directory using a format string. Suppresses
   both stdout and stderr so "file not found" errors don't pollute the log. */

static void
del_q( const char* fmt, ... )
{
    char cmd[ PATH_MAX * 2 ];
    va_list args;
    va_start( args, fmt );
    vsnprintf( cmd, sizeof( cmd ), fmt, args );
    va_end( args );
    build_run_cmd_quiet( cmd );
}

/*============================================================================================*/
/* Deletes build artifacts for target or global clean if target is NULL */

void
build_clean( target_info_t* target )
{
    if ( target )
    {
        // Per-target clean. Sub-commands run silently; we print one summary line
        // so MSBuild output stays parseable.
        const char* ext = ( target->type == TARGET_STATIC_LIB )  ? "lib"
                        : ( target->type == TARGET_DYNAMIC_LIB ) ? "dll"
                        :                                          "exe";

        del_q( "del /q bin\\%s.%s >nul 2>nul", target->name, ext );

        if ( target->type == TARGET_DYNAMIC_LIB )
        {
            // Cover both monolithic (.lib primary) and dynamic (.dll primary)
            // outputs plus the import .exp. del /q is a no-op when the file
            // doesn't exist, so listing all three is always safe.
            del_q( "del /q bin\\%s.lib >nul 2>nul", target->name );
            del_q( "del /q bin\\%s.dll >nul 2>nul", target->name );
            del_q( "del /q bin\\%s.exp >nul 2>nul", target->name );
        }

        del_q( "del /q bin\\%s_*.pdb >nul 2>nul", target->name );
        del_q( "rd /s /q %s\\%s\\%s >nul 2>nul", g_build_dir, g_int_dir, target->name );

        if ( target->has_reflect )
        {
            const char* rname = target->reflect_name ? target->reflect_name : target->name;
            del_q( "del /q %s\\%s\\%s.generated.c >nul 2>nul", g_build_dir, g_gen_dir, rname );
            del_q( "del /q %s\\%s\\%s.generated.h >nul 2>nul", g_build_dir, g_gen_dir, rname );
        }

        printf( ORB_BANNER "[orb clean] %s -- bin\\%s.%s, %s\\%s\\%s%s\n",
                target->name, target->name, ext,
                g_build_dir, g_int_dir, target->name,
                target->has_reflect ? " (+reflect)" : "" );
    }
    else
    {
        // Global wipe. Every del runs silently; one summary line at the end.
        del_q( "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_int_dir );
        del_q( "del /s /q %s\\%s\\* >nul 2>nul", g_build_dir, g_gen_dir );
        build_run_cmd_quiet( "del /s /q bin\\*.pdb >nul 2>nul" );
        build_run_cmd_quiet( "del /s /q bin\\*.lib >nul 2>nul" );
        build_run_cmd_quiet( "del /s /q bin\\*.dll >nul 2>nul" );
        build_run_cmd_quiet( "del /s /q bin\\*.exp >nul 2>nul" );

        // Delete executables only for non-tool targets. is_tool executables
        // (reflect_tool, build_tool) are rebuilt by dep resolution and have no
        // VS project to rebuild them after a clean, so leave them in place.
        for ( int i = 0; i < g_target_count; ++i )
        {
            if ( g_targets[ i ].is_external ) continue;
            if ( g_targets[ i ].type == TARGET_EXECUTABLE && !g_targets[ i ].is_tool )
                del_q( "del /q bin\\%s.exe >nul 2>nul", g_targets[ i ].name );
        }

        printf( ORB_BANNER "[orb clean] all -- bin\\*, %s\\{%s,%s}\\*\n",
                g_build_dir, g_int_dir, g_gen_dir );
    }
}

#endif

// clang-format on
/*============================================================================================*/
