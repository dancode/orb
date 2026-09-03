/*==============================================================================================

    build_tool_11_clean.c -- Artifact cleanup for the -clean command, plus the
    third-party runtime deploy that repopulates bin/ after cleans.

    Two clean modes:

      Per-target (target != NULL) -- removes only that target's artifacts:

        bin/<name>.{lib,dll,exe,exe.old,exp,ilk,pdb}, obj/<name>/, and generated
        reflection files.
        Called from each VS .vcxproj's NMakeCleanCommandLine so a solution rebuild
        cleans each project independently rather than wiping the whole bin/ tree.

      Global (target == NULL) -- wipes all intermediates and artifacts.

        Skips is_tool executables (reflect_tool, build_tool) so tools survive a
        full clean. They are rebuilt on demand by dep resolution, not by VS, so
        deleting them would leave no path to recreate them.

    Every deletion goes through the platform_* helpers (DeleteFileA / RemoveDirectoryA,
    unlink / rmdir), so a clean spawns no child process. That is what BUILD_SAFE_MODE
    used to buy here and it now holds unconditionally: no cmd.exe, no del / rd, nothing
    for EDR to see behind the build tool. It also removed a real behavioral split -- the
    old shell path ran `del /s /q build\obj\*`, which erased the files but left the
    directory tree standing, while the API path removed both.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Third-Party Runtime Deploy ---

    Mirrors g_third_party_bin_dir (flat -- no recursion) into bin/ so prebuilt runtime
    files (freetype.dll/.lib for font_tool, ...) survive cleans and fresh checkouts.
    Called after a global clean and before a full build-all (named-target builds and
    per-target cleans skip it). A file is copied only when the
    bin/ copy is missing or older than the source, so an up-to-date tree is a silent
    no-op. Both the source directory and the master switch live with the project
    constants in build_tool.c. Uses the platform_find/copy seam -- no shell children.
==============================================================================================*/

void
build_deploy_third_party( void )
{
    if ( !g_deploy_third_party ) return;
    if ( !platform_file_exists( g_third_party_bin_dir ) ) return;

    ensure_dir( "bin" );

    char pattern[ PATH_MAX ];
    snprintf( pattern, sizeof( pattern ), "%s" PATH_SEP "*", g_third_party_bin_dir );

    platform_find_data_t fd;
    platform_find_t      h = platform_find_first( pattern, &fd );
    if ( h == PLATFORM_FIND_INVALID ) return;

    int copied = 0;
    do
    {
        if ( fd.is_dir ) continue;

        char src[ PATH_MAX ], dst[ PATH_MAX ];
        snprintf( src, sizeof( src ), "%s" PATH_SEP "%s", g_third_party_bin_dir, fd.name );
        snprintf( dst, sizeof( dst ), "bin" PATH_SEP "%s", fd.name );

        if ( platform_get_mtime( dst ) >= platform_get_mtime( src ) ) continue;

        if ( platform_copy_file_quiet( src, dst ) )
            ++copied;
        else
            printf( ORB_INDENT "[orb warn] deploy failed: %s -> %s (file in use?)\n", src, dst );
    }
    while ( platform_find_next( h, &fd ) );
    platform_find_close( h );

    if ( copied )
        printf( ORB_INDENT "[orb deploy] %d file%s %s -> bin\n",
                copied, copied == 1 ? "" : "s", g_third_party_bin_dir );
}

/*==============================================================================================
    --- Clean ---

    Deletes build artifacts for one target, or for the whole tree when target is NULL.
==============================================================================================*/

/* Delete bin/<name>.<ext>, joining the parts so call sites stay one line. */

static void
clean_bin_file( const char* name, const char* ext )
{
    char path[ PATH_MAX ];
    snprintf( path, sizeof( path ), "bin" PATH_SEP "%s.%s", name, ext );
    platform_delete_file( path );
}

void
build_clean( target_info_t* target )
{
    char obj_root[ PATH_MAX ], gen_root[ PATH_MAX ];
    path_obj_root( obj_root, sizeof( obj_root ) );
    path_gen_dir( gen_root, sizeof( gen_root ) );

    if ( target )
    {
        const char* ext = ( target->type == TARGET_STATIC_LIB )  ? "lib"
                        : ( target->type == TARGET_DYNAMIC_LIB ) ? "dll"
                        :                                          "exe";

        clean_bin_file( target->name, ext );

        if ( target->type == TARGET_DYNAMIC_LIB )
        {
            // Cover both monolithic (.lib primary) and dynamic (.dll primary)
            // outputs plus the import .exp. Deleting a file that does not exist
            // is a silent no-op, so listing all three is always safe.
            clean_bin_file( target->name, "lib" );
            clean_bin_file( target->name, "dll" );
            clean_bin_file( target->name, "exp" );
        }

        // <name>.exe.old is the rollback copy build_target() renames a locked image to
        // before relinking; <name>.ilk is the incremental-link database. Neither is an
        // input to anything, and the .old is a stale unsigned executable.
        if ( target->type == TARGET_EXECUTABLE )
            clean_bin_file( target->name, "exe.old" );
        clean_bin_file( target->name, "ilk" );

        char pdb_glob[ 128 ];
        snprintf( pdb_glob, sizeof( pdb_glob ), "%s_*.pdb", target->name );
        platform_delete_glob_quiet( "bin", pdb_glob );

        char obj_dir[ PATH_MAX ];
        platform_rmdir_quiet( path_obj_dir( target, obj_dir, sizeof( obj_dir ) ) );

        if ( target->has_reflect )
        {
            const char* rname = target_reflect_name( target );
            char path[ PATH_MAX ];
            platform_delete_file( path_generated( gen_root, rname, "c", path, sizeof( path ) ) );
            platform_delete_file( path_generated( gen_root, rname, "h", path, sizeof( path ) ) );
        }

        printf( ORB_BANNER "[orb clean] %s -- bin" PATH_SEP "%s.%s, %s" PATH_SEP "%s%s\n",
                target->name, target->name, ext,
                obj_root, target->name,
                target->has_reflect ? " (+reflect)" : "" );
        return;
    }

    // Global wipe. The obj and generated subtrees go entirely; the next build's
    // ensure_dir() calls recreate them.
    platform_rmdir_quiet( obj_root );
    platform_rmdir_quiet( gen_root );

    platform_delete_glob_quiet( "bin", "*.pdb" );
    platform_delete_glob_quiet( "bin", "*.lib" );
    platform_delete_glob_quiet( "bin", "*.dll" );
    platform_delete_glob_quiet( "bin", "*.exp" );
    platform_delete_glob_quiet( "bin", "*.ilk" );

    // Rollback copies go even for the tool targets whose .exe survives below: a .old is
    // never the file anything rebuilds from. One still mapped by a running process
    // (build_tool's own, when it is the process doing the clean) refuses to unlink and
    // stays until the next build of that target replaces it.
    platform_delete_glob_quiet( "bin", "*.exe.old" );

    // Delete executables only for non-tool targets. is_tool executables
    // (reflect_tool, build_tool) are rebuilt by dep resolution and have no
    // VS project to rebuild them after a clean, so leave them in place.
    for ( int i = 0; i < g_target_count; ++i )
    {
        if ( g_targets[ i ].is_external ) continue;
        if ( g_targets[ i ].type == TARGET_EXECUTABLE && !g_targets[ i ].is_tool )
            clean_bin_file( g_targets[ i ].name, "exe" );
    }

    printf( ORB_BANNER "[orb clean] all -- bin" PATH_SEP "*, %s" PATH_SEP "{%s,%s}" PATH_SEP "*\n",
            g_build_dir, g_int_dir, g_gen_dir );
}

// clang-format on
/*============================================================================================*/
