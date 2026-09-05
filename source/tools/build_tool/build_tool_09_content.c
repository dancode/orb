/*==============================================================================================

    build_tool_09_content.c -- Resource manifest harvest and the content phase.

    Two steps that read a target's sources and content but feed nothing to the compiler:

      Harvest (res_tool)   Per target, always on. Scans the target's units and those of its
                           link closure for RID() / RES_TREE() tokens, resolves every name
                           against the content roots, and writes <obj_dir>/<name>_res_manifest.txt.
                           Every static lib, dynamic module and executable carries one; the
                           image manifests are the packager's input, the lib manifests scope
                           a cook to one target. Runs on every rebuild and whenever
                           content_stale() finds the content directories newer than the
                           manifest -- without compiling anything, since no compiler reads it.

      Content phase        Once per build, after the code graph has finished, on the main
      (asset_tool)         thread. Hands the manifests of the targets just built to asset_tool
                           in one call. By default asset_tool only CHECKS them -- which cooked
                           files (a stage-tagged .hlsl -> .oshd, a .recipe -> its kind) are
                           missing or older than their inputs -- and the build ends with one
                           line saying so. With -content it cooks them into <build>/content.
                           What each kind cooks to and when an output is stale is asset_tool's
                           knowledge alone; this file passes it manifests and content roots.

    Strictness: a failure in either step is a warning and the build still succeeds;
    -strict-content makes it an error. reflect_tool is deliberately not covered here -- its
    .generated.c/.h are compiled, so its failure is a compile failure in every mode.

    build_target() (09_exec.c) reaches the harvest through three hooks:

      content_resolve()     step 0    -- locate and build res_tool; NULL when the harvest is off
      content_stale()       step 3    -- content newer than the manifest: re-harvest, no rebuild
      content_harvest()     step 6.5  -- write the manifest after a rebuild

    main() runs build_content_phase() once the graph has built. cmd_res_manifest() is the
    -res-manifest command the MSBuild pre-build event runs.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Policy ---
==============================================================================================*/

/*  Diagnostic label for the content steps: "error" under -strict-content, "warn" otherwise.
    Every call site pairs it with `if ( g_content_strict )` around the abort. */

static const char*
content_severity( void )
{
    return g_content_strict ? "error" : "warn";
}

/*  res_tool could not be built or found: no target gets a manifest this run. Without the
    latch every target would retry the same failing build and repeat the same diagnostic. A
    plain unlocked bool: a worker already past the check when another sets it repeats the
    message once, which is bounded and never a correctness question. Never set under
    -strict-content, which returns first. */

static bool g_res_tool_down = false;

// Content roots, highest priority first: this project's content/, then the engine's when this
// is a child project. Returns the count.
static int
res_content_roots( char roots[ 2 ][ PATH_MAX ] )
{
    int n = 0;
    path_abs( roots[ n ], "content", PATH_MAX );
    ++n;
    if ( g_engine_root[ 0 ] )
        snprintf( roots[ n++ ], PATH_MAX, "%s/content", g_engine_root );
    return n;
}

// The " -root <dir>" arguments both tools take, in priority order.
static void
res_root_args( char* buf, size_t cap )
{
    char root_dirs[ 2 ][ PATH_MAX ];
    int  root_count = res_content_roots( root_dirs );
    int  n          = 0;
    buf[ 0 ]        = '\0';
    for ( int r = 0; r < root_count; ++r )
        n += snprintf( buf + n, cap - ( size_t )n, " -root %s", root_dirs[ r ] );
}

/*==============================================================================================
    --- Harvest: Resource Manifest ---

    The target's name set is the union of the RID() / RES_TREE() tokens in its own units and
    in every library it links statically, so the scan input is the unit list of the target's
    whole link closure.  build_tool owns the graph and writes that list to
    <obj_dir>/_res_units.txt; res_tool owns the scan (it follows #include from each unit,
    so unity fragments and headers are covered), resolves every name against the content
    roots, and writes <obj_dir>/<name>_res_manifest.txt.  Nothing is compiled from it.

    Monolithic-only deps are deliberately NOT folded in: a module linked as a static lib
    under -monolithic still gets its own manifest, exactly as its DLL form does, so the exe's
    manifest must not absorb it.

    A CONTENT change is the one input the compiler never sees, so res_tool also writes the
    content directories the manifest was computed from to <obj_dir>/_res_deps.txt, and
    content_stale() replays that list against the manifest's own mtime.
==============================================================================================*/

// Appends the absolute unit paths of t, then of its link deps, to `f`. visited[] is indexed
// like g_targets[] and keeps a diamond in the graph from listing a unit twice.
static void
res_list_units( FILE* f, const target_info_t* t, bool visited[ MAX_TARGETS ] )
{
    for ( int i = 0; t->units[ i ]; ++i )
    {
        char rel[ PATH_MAX ], abs_p[ PATH_MAX ];
        snprintf( rel, sizeof( rel ), "%s/%s", t->root_dir, t->units[ i ] );
        path_abs( abs_p, rel, sizeof( abs_p ) );
        fprintf( f, "%s\n", abs_p );
    }
    for ( int i = 0; t->deps[ i ]; ++i )
    {
        const target_info_t* dep = find_target( t->deps[ i ] );
        if ( !dep )
            continue;
        int idx = ( int )( dep - g_targets );
        if ( visited[ idx ] )
            continue;
        visited[ idx ] = true;
        res_list_units( f, dep, visited );
    }
}

bool
build_gen_res_manifest( target_info_t* target, const char* obj_dir, const target_info_t* res_tool )
{
    char list_path[ PATH_MAX ];
    snprintf( list_path, sizeof( list_path ), "%s" PATH_SEP "_res_units.txt", obj_dir );
    char out_path[ PATH_MAX ];
    path_res_manifest( target, obj_dir, out_path, sizeof( out_path ) );

    FILE* lf = fopen( list_path, "w" );
    if ( !lf )
    {
        printf( ORB_INDENT "[orb %s] '%s' cannot write %s\n", content_severity(), target->name, list_path );
        return false;
    }
    fprintf( lf, "# res_tool inputs for '%s': its units and those of its link closure\n", target->name );
    {
        bool visited[ MAX_TARGETS ] = { 0 };
        visited[ ( int )( target - g_targets ) ] = true;
        res_list_units( lf, target, visited );
    }
    fclose( lf );

    if ( g_out_flags & ORB_OUT_REFLECT )
        log_printf( ORB_INDENT "[orb res] %s -> %s_res_manifest.txt\n", target->name, target->name );

    // Quoted-include roots, in the compiler's order: the project source root, then the
    // engine's when this is a child project.
    char src_root[ PATH_MAX ];
    path_abs( src_root, "source", sizeof( src_root ) );

    char engine_inc[ PATH_MAX + 8 ] = { 0 };
    if ( g_engine_root[ 0 ] )
        snprintf( engine_inc, sizeof( engine_inc ), " -inc %s/source", g_engine_root );

    // Content roots in the same order, highest priority first, so a project file shadows the
    // engine's under the same name. A root that does not exist yet is passed anyway: res_tool
    // records it in the deps file and its appearance makes the manifest stale.
    char roots[ PATH_MAX * 2 + 32 ];
    res_root_args( roots, sizeof( roots ) );

    char deps_path[ PATH_MAX ];
    snprintf( deps_path, sizeof( deps_path ), "%s" PATH_SEP "_res_deps.txt", obj_dir );

    const char* silent = ( g_out_flags & ORB_OUT_REFLECT ) ? "" : " -silent";
    char cmd[ PATH_MAX * 8 ];
    snprintf( cmd, sizeof( cmd ), "bin" PATH_SEP "%s.exe -list %s -out %s -deps %s -name %s -inc %s%s%s%s",
              res_tool->name, list_path, out_path, deps_path, target->name, src_root, engine_inc, roots, silent );
    if ( build_run_cmd( cmd ) != 0 )
    {
        printf( ORB_INDENT "[orb %s] '%s' resource manifest failed -- see the res_tool errors above\n",
                content_severity(), target->name );
        return false;
    }
    return true;
}

/*  Step 0 hook. Locates res_tool for a target that carries a manifest and, on the serial
    path, builds it (the scheduler pre-wires it as a graph dep). Sets *out_res_tool to the
    tool, or to NULL when the harvest is off for this target or for the run: no manifest, so
    steps 3 and 6.5 skip their content work and the target still compiles. Returns false only
    under -strict-content, where an unavailable res_tool fails the target. */

static bool
content_resolve( build_context_t* ctx, target_info_t* target, target_info_t** out_res_tool )
{
    *out_res_tool = NULL;
    if ( !target_wants_res_manifest( target ) || g_res_tool_down )
        return true;

    target_info_t* res_tool = find_res_tool();

    const char* down = NULL;    // why no target gets a manifest this run
    if ( !res_tool )
        down = "no is_res_tool target is registered";
    else if ( !ctx->skip_tool_deps && !build_target( ctx, res_tool, NULL, NULL ) )
        down = "res_tool did not build";
    else
    {
        // On the scheduler path res_tool is a graph dep, so a soft failure of that job
        // reaches us as a missing exe rather than as a failed build_target.
        char rt_exe[ PATH_MAX ];
        snprintf( rt_exe, sizeof( rt_exe ), "bin" PATH_SEP "%s.exe", res_tool->name );
        if ( !platform_file_exists( rt_exe ) )
            down = "res_tool is not built";
    }

    if ( down )
    {
        if ( g_content_strict )
        {
            printf( ORB_INDENT "[orb error] '%s' needs a resource manifest but %s\n", target->name, down );
            return false;
        }
        printf( ORB_INDENT "[orb warn] %s -- no target gets a resource manifest this run\n", down );
        g_res_tool_down = true;
        return true;
    }

    *out_res_tool = res_tool;
    return true;
}

/*  Step 3 hook. True when the manifest must be regenerated: it is missing, res_tool.exe is
    newer than it (how a manifest is spelled lives in the tool), the deps file is missing, or
    a content directory it lists has changed since it was written. A directory's mtime moves
    when a file is added, removed or renamed inside it, which is exactly the change that
    alters the manifest (or breaks a name's resolution) without touching any source. A line
    starting with '!' is a content root that did not exist at harvest time; its appearance
    is the stale signal.

    Compared against the MANIFEST's mtime, not the artifact's: the manifest is a runtime
    input, so content going stale must not make the compiled code look stale. Equal
    timestamps count as stale: mtimes may be whole seconds, and a content edit in the same
    second the harvest finished would otherwise go unseen until some source changed. The
    cost is one extra harvest in that second, after which the check settles.

    A missing manifest is stale so a name that failed to resolve is retried on the next
    build: res_tool deletes the manifest and writes no deps file in that case, and outside
    -strict-content the target links anyway. */

static bool
content_stale( const target_info_t* target, const char* obj_dir, const target_info_t* res_tool )
{
    char man_path[ PATH_MAX ];
    path_res_manifest( target, obj_dir, man_path, sizeof( man_path ) );
    platform_mtime_t man_mtime = platform_get_mtime( man_path );
    if ( man_mtime == 0 )
        return true;

    char rt_exe[ PATH_MAX ];
    snprintf( rt_exe, sizeof( rt_exe ), "bin" PATH_SEP "%s.exe", res_tool->name );
    if ( platform_get_mtime( rt_exe ) >= man_mtime )
        return true;

    char deps_path[ PATH_MAX ];
    snprintf( deps_path, sizeof( deps_path ), "%s" PATH_SEP "_res_deps.txt", obj_dir );
    platform_mapped_file_t dep_map;
    if ( !platform_map_file( deps_path, &dep_map ) )
        return true;    // no recorded content set: assume stale

    bool        stale = false;
    const char* p     = dep_map.data;
    const char* end   = dep_map.data + dep_map.size;
    char        dep[ PATH_MAX ];
    while ( !stale && mmap_next_line( &p, end, dep, sizeof( dep ) ) )
    {
        if ( !dep[ 0 ] ) continue;
        if ( dep[ 0 ] == '!' )
        {
            if ( platform_file_exists( dep + 1 ) )
                stale = true;
            continue;
        }
        platform_mtime_t d_mtime = platform_get_mtime( dep );
        if ( d_mtime == 0 || d_mtime >= man_mtime )
            stale = true;
    }
    platform_unmap_file( &dep_map );
    return stale;
}

/*  Step 6.5 hook, also the whole of a content-only refresh in step 3. Writes the manifest;
    a failed harvest removes it, so the content phase has nothing of this target's to act on.
    Returns false only under -strict-content. */

static bool
content_harvest( target_info_t* target, const char* obj_dir, const target_info_t* res_tool )
{
    if ( build_gen_res_manifest( target, obj_dir, res_tool ) )
        return true;
    return !g_content_strict;
}

/*==============================================================================================
    --- Content Phase ---

    One asset_tool call over the manifests of every target this build covered:

        bin/asset_tool.exe -list <obj>/_content_manifests.txt -root ... -out <build>/content
                           [-check] [-f]

    Cooked files are inputs to the RUNTIME, not to the compiler, so nothing in the graph waits
    on them and the phase runs after the last job. Under -content the scheduler adds asset_tool
    as a root job so the cooker is current when the phase starts; the -no-deps path builds it
    here. Without -content the phase is a report: asset_tool -check exits 3 when any cooked
    file is missing or older than its inputs, and the build ends with one line saying so. A
    cooker that is not built means no report this run, not a failure -- a code-only checkout
    never has to build the content pipeline to compile.

    asset_tool's last line is a fixed-format summary, parsed here so the build can print its
    own line with the counts. Its per-name lines are relabelled: "cook <name> (<why>)" prints
    as [orb cook] at the same level as the per-target compile summary, "stale <name> (<why>)"
    and the converters' own output only under ORB_OUT_REFLECT; a line carrying "error" always
    prints.

    A second build_tool running -content at the same time serializes on the content_phase
    lock rather than cooking the same file underneath this one.
==============================================================================================*/

typedef struct
{
    int  total, stale, missing, cooked, failed;
    bool have_summary;
} content_report_t;

static void
content_phase_line( char* line, void* ud )
{
    content_report_t* r = ( content_report_t* )ud;

    if ( strncmp( line, "asset_tool: content ", 20 ) == 0 && strstr( line, "total=" ) )
    {
        const char* p;
        r->have_summary = true;
        if ( ( p = strstr( line, "total="   ) ) != NULL ) r->total   = atoi( p + 6 );
        if ( ( p = strstr( line, "stale="   ) ) != NULL ) r->stale   = atoi( p + 6 );
        if ( ( p = strstr( line, "missing=" ) ) != NULL ) r->missing = atoi( p + 8 );
        if ( ( p = strstr( line, "cooked="  ) ) != NULL ) r->cooked  = atoi( p + 7 );
        if ( ( p = strstr( line, "failed="  ) ) != NULL ) r->failed  = atoi( p + 7 );
        return;
    }
    if ( strncmp( line, "asset_tool: cook ", 17 ) == 0 )
    {
        if ( g_out_flags & ORB_OUT_SUMMARY_COMPILE )
            printf( ORB_INDENT "[orb cook] %s\n", line + 17 );
        return;
    }
    if ( strncmp( line, "asset_tool:   stale ", 20 ) == 0 )
    {
        if ( g_out_flags & ORB_OUT_REFLECT )
            printf( ORB_INDENT "[orb stale] %s\n", line + 20 );
        return;
    }
    if ( ( g_out_flags & ORB_OUT_REFLECT ) || strstr( line, "error" ) )
        printf( ORB_INDENT "  %s\n", line );
}

bool
build_content_phase( build_context_t* ctx, target_info_t* const* targets, int count )
{
    target_info_t* asset_tool = find_asset_tool();

    // --- The cooker ---

    const char* down = NULL;    // why asset_tool cannot run this time
    char        exe[ PATH_MAX ] = "";
    if ( !asset_tool )
        down = "no is_asset_tool target is registered";
    else
    {
        snprintf( exe, sizeof( exe ), "bin" PATH_SEP "%s.exe", asset_tool->name );

        // -no-deps has no scheduler to build the cooker; do it here with the cooker's own dep
        // resolution, and without -force: the call only has to leave a current exe on disk.
        if ( g_cook && ctx->skip_deps )
        {
            build_context_t tctx  = *ctx;
            tctx.skip_deps        = false;
            tctx.skip_tool_deps   = false;
            tctx.force_rebuild    = false;
            if ( !build_target( &tctx, asset_tool, NULL, NULL ) )
                down = "the content cooker did not build";
        }
        if ( !down && !platform_file_exists( exe ) )
            down = "the content cooker is not built";
    }

    if ( down )
    {
        if ( g_cook )
        {
            printf( ORB_INDENT "[orb %s] %s -- nothing is cooked into %s" PATH_SEP "content this run\n",
                    content_severity(), down, g_build_dir );
            return !g_content_strict;
        }
        if ( g_out_flags & ORB_OUT_REFLECT )
            printf( ORB_INDENT "[orb content] check skipped: %s\n", down );
        return true;
    }

    // --- The manifests of the targets this build covered ---
    //
    // A target with none (its harvest is off, or failed and removed the file) contributes
    // nothing; res_tool has already said why.

    char list_path[ PATH_MAX ];
    {
        char obj_root[ PATH_MAX ];
        path_obj_root( obj_root, sizeof( obj_root ) );
        ensure_dir( obj_root );
        snprintf( list_path, sizeof( list_path ), "%s" PATH_SEP "_content_manifests.txt", obj_root );
    }
    FILE* lf = fopen( list_path, "w" );
    if ( !lf )
    {
        printf( ORB_INDENT "[orb %s] cannot write %s\n", content_severity(), list_path );
        return !g_content_strict;
    }
    fprintf( lf, "# resource manifests of the targets the last build covered\n" );
    int listed = 0;
    for ( int i = 0; i < count; ++i )
    {
        const target_info_t* t = targets[ i ];
        if ( !target_wants_res_manifest( t ) )
            continue;
        char obj_dir[ PATH_MAX ], man_path[ PATH_MAX ];
        path_obj_dir( t, obj_dir, sizeof( obj_dir ) );
        path_res_manifest( t, obj_dir, man_path, sizeof( man_path ) );
        if ( !platform_file_exists( man_path ) )
            continue;
        fprintf( lf, "%s\n", man_path );
        ++listed;
    }
    fclose( lf );
    if ( listed == 0 )
        return true;

    // --- One asset_tool run ---

    char roots[ PATH_MAX * 2 + 32 ];
    res_root_args( roots, sizeof( roots ) );

    char cmd[ PATH_MAX * 5 ];
    snprintf( cmd, sizeof( cmd ), "%s -list %s%s -out %s" PATH_SEP "content%s%s", exe, list_path, roots,
              g_build_dir, g_cook ? "" : " -check", ( g_cook && ctx->force_rebuild ) ? " -f" : "" );

    content_report_t rep  = { 0 };
    void*            lock = build_lock_target( "content_phase" );
    int              rc   = platform_spawn_capture( cmd, content_phase_line, &rep );
    build_unlock_target( lock );

    if ( rc < 0 || !rep.have_summary )
    {
        printf( ORB_INDENT "[orb %s] asset_tool gave no content report (exit %d)\n", content_severity(), rc );
        return !g_content_strict;
    }

    // --- The one line the build ends with ---

    if ( g_cook )
    {
        if ( rep.cooked || ( g_out_flags & ORB_OUT_REFLECT ) )
            printf( ORB_INDENT "[orb content] cooked %d of %d into %s" PATH_SEP "content\n",
                    rep.cooked, rep.total, g_build_dir );
        if ( rep.failed || rc != 0 )
        {
            printf( ORB_INDENT "[orb %s] %d content cook(s) failed -- see the asset_tool errors above\n",
                    content_severity(), rep.failed );
            return !g_content_strict;
        }
        return true;
    }

    if ( rep.stale )
        printf( ORB_INDENT "%s[orb content]%s %d cooked file(s) out of date (%d missing) -- run build_tool -content\n",
                g_clr_yellow, g_clr_reset, rep.stale, rep.missing );
    else if ( g_out_flags & ORB_OUT_REFLECT )
        printf( ORB_INDENT "[orb content] %d cooked file(s) up to date\n", rep.total );

    if ( rep.failed || ( rc != 0 && rc != 3 ) )
    {
        printf( ORB_INDENT "[orb %s] content check reported %d error(s)\n", content_severity(), rep.failed );
        return !g_content_strict;
    }
    return true;
}

/*==============================================================================================
    --- Command: -res-manifest (MSBuild pre-build event) ---

    Generates the target's resource manifest only, building res_tool first if needed. The
    native MSBuild projects build without build_target, so this is how their pre-build event
    still resolves every marked name and writes obj/<target>/<target>_res_manifest.txt.

    Exits 0 on a pipeline failure unless -strict-content: a nonzero exit from a pre-build
    event stops the MSBuild project, which is exactly what the default must not do.
==============================================================================================*/

static int
cmd_res_manifest( build_context_t* ctx, target_info_t* target, const char* target_upper )
{
    if ( !target ) { printf( ORB_INDENT "[orb error] -res-manifest requires -target\n" ); return 1; }
    if ( !target_wants_res_manifest( target ) )
    {
        printf( ORB_INDENT "[orb error] '%s' carries no resource manifest (a build tool or an alias)\n",
                target->name );
        return 1;
    }
    target_info_t* res_tool = find_res_tool();
    if ( !res_tool )
    {
        printf( ORB_INDENT "[orb %s] no res_tool is registered\n", content_severity() );
        return g_content_strict ? 1 : 0;
    }
    if ( !build_target( ctx, res_tool, NULL, NULL ) )
    {
        printf( ORB_INDENT "[orb %s] '%s' resource manifest skipped -- res_tool did not build\n",
                content_severity(), target->name );
        return g_content_strict ? 1 : 0;
    }

    char obj_dir[ PATH_MAX ];
    path_obj_dir( target, obj_dir, sizeof( obj_dir ) );
    ensure_dir( g_build_dir );
    ensure_dir( obj_dir );
    if ( !build_gen_res_manifest( target, obj_dir, res_tool ) && g_content_strict )
    {
        printf( ORB_BANNER "%s[ %s: FAILED ]%s\n", g_clr_red, target_upper, g_clr_reset );
        return 1;
    }
    printf( "\n" );
    return 0;
}

// clang-format on
/*============================================================================================*/
