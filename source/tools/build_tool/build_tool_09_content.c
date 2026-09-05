/*==============================================================================================

    build_tool_09_content.c -- Resource manifest harvest and content cook.

    Two steps that read a target's sources and content but feed nothing to the compiler:

      Harvest (res_tool)   Always on. Scans the target's units and those of its link closure
                           for RID() / RES_TREE() tokens, resolves every name against the
                           content roots, and writes <obj_dir>/<name>_res_manifest.txt. Every
                           static lib, dynamic module and executable carries one; the image
                           manifests are the packager's input, the lib manifests scope a cook
                           to one target. Runs on every rebuild and whenever content_stale()
                           finds the content directories newer than the manifest -- without
                           compiling anything, since no compiler reads it.

      Cook (asset_tool)    Opt-in with -content. Turns each cookable name in the manifest (a
                           stage-tagged .hlsl, a .recipe) into its runtime form under
                           <build>/content. Off by default: the build is a code compiler unless
                           asked, and `build_tool -content` (or `-target gui -content`) cooks
                           whatever the manifests have accumulated since.

    Strictness: a failure in either step is a warning and the target still compiles and links;
    -strict-content makes it an error. reflect_tool is deliberately not covered here -- its
    .generated.c/.h are compiled, so its failure is a compile failure in every mode.

    build_target() (09_exec.c) reaches this file through four hooks:

      content_resolve()     step 0    -- locate and build res_tool; NULL when the harvest is off
      build_cook_content()  step 2.5  -- pre-cook from the previous manifest (-content only)
      content_stale()       step 3    -- content newer than the manifest: refresh without a rebuild
      content_refresh()     step 6.5  -- harvest, then cook the fresh manifest (-content only)

    cmd_res_manifest() is the -res-manifest command the MSBuild pre-build event runs.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Policy ---
==============================================================================================*/

/*  True for every target the cooker is built FROM: asset_tool, the cookers it lists as
    tool_deps, and everything those reach through deps, mono_deps and tool_deps (sys, pack,
    base, dev_font, ...). None of them may cook: a cooking target gains an implicit edge into
    asset_tool, which for these would close a cycle. Their names are still cooked -- the
    cooked file is keyed on the name, so any target above them that links them (gui, a
    host) cooks the same file once. Computed once from the immutable target pool. */

static bool
target_is_cooker_dep( const target_info_t* t )
{
    static bool s_ready = false;
    static bool s_in_closure[ MAX_TARGETS ];

    if ( !s_ready )
    {
        s_ready = true;
        memset( s_in_closure, 0, sizeof( s_in_closure ) );

        // Iterative closure over a worklist; the pool is small and immutable, so a plain
        // fixed-point pass beats recursion here.
        const target_info_t* root = find_asset_tool();
        if ( root )
            s_in_closure[ ( int )( root - g_targets ) ] = true;
        for ( bool grew = root != NULL; grew; )
        {
            grew = false;
            for ( int i = 0; i < g_target_count; ++i )
            {
                if ( !s_in_closure[ i ] ) continue;
                const target_info_t* m = &g_targets[ i ];
                const char* const* lists[ 3 ] = { m->deps, m->mono_deps, m->tool_deps };
                for ( int l = 0; l < 3; ++l )
                    for ( int d = 0; lists[ l ][ d ]; ++d )
                    {
                        const target_info_t* dep = find_target( lists[ l ][ d ] );
                        if ( !dep ) continue;
                        int di = ( int )( dep - g_targets );
                        if ( !s_in_closure[ di ] ) { s_in_closure[ di ] = true; grew = true; }
                    }
            }
        }
    }
    return s_in_closure[ ( int )( t - g_targets ) ];
}

/*  True when this run cooks for `t`: -content was given, the target carries a manifest, and
    the cooker does not depend on it (see target_is_cooker_dep; that also covers the content
    tools themselves). Consulted by the cook pass here and by the scheduler, which wires
    asset_tool as an implicit dep of exactly these targets. */

static bool
cook_active( const target_info_t* t )
{
    return g_cook && target_wants_res_manifest( t ) && !target_is_content_tool( t ) && !target_is_cooker_dep( t );
}

/*  Diagnostic label for the content steps: "error" under -strict-content, "warn" otherwise.
    Every call site pairs it with `if ( g_content_strict )` around the abort. */

static const char*
content_severity( void )
{
    return g_content_strict ? "error" : "warn";
}

/*  A content tool that could not be built is unavailable for the whole run: without these
    latches every target would retry the same failing compile and repeat the same diagnostic
    (twice over, since each target cooks in two passes). Plain bools, deliberately unlocked:
    workers already past the check when one of them sets the latch repeat the message, so the
    worst case is one line per worker rather than one per target -- bounded, and never a
    correctness question. Neither is ever set under -strict-content, which returns first. */

static bool g_res_tool_down   = false;
static bool g_asset_tool_down = false;

/*  Set once the cooker has been resolved successfully, so the nested build_target() in
    build_cook_content() runs at most once per process instead of once per cooking target.
    Same unlocked-bool reasoning as the latches above: a worker already past the check repeats
    the resolve, which is redundant work rather than a wrong answer. Under the scheduler the
    resolve is redundant anyway -- add_job() wires asset_tool as an implicit dep of every
    cooking target -- and this call is the fallback for the paths that have no scheduler
    (-no-deps from VS, serial -target). */

static bool g_asset_tool_ready = false;

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

    // Content roots in the same order, highest priority first: this project's content/,
    // then the engine's when this is a child project -- so a project file shadows the
    // engine's under the same name. A root that does not exist yet is passed anyway:
    // res_tool records it in the deps file and its appearance makes the manifest stale.
    char roots[ PATH_MAX * 2 + 32 ];
    {
        char root_dirs[ 2 ][ PATH_MAX ];
        int  root_count = res_content_roots( root_dirs );
        int  n          = 0;
        for ( int r = 0; r < root_count; ++r )
            n += snprintf( roots + n, sizeof( roots ) - ( size_t )n, " -root %s", root_dirs[ r ] );
    }

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

/*==============================================================================================
    --- Cook ---

    The manifest lists every content file the target's code names.  Some of those need a
    cooked form before the runtime can load them: a stage-tagged .hlsl becomes an .oshd
    container, a .recipe becomes the file its "kind" line says (a font bake).  This step
    cooks each such entry into the cooked mirror, <build>/content/<name>.<cooked ext>, the tree
    the host mounts above content/ so the cooked file wins by name.  Images and other loose
    content are not cooked here; the runtime reads them from content/ as they are.

    The cooker is asset_tool, which reads a shader's stage tag out of its filename and forwards
    to shader_tool, or parses a recipe and forwards to font_tool.  The first entry that needs
    cooking builds whichever target carries is_asset_tool, and under the parallel scheduler
    add_job() wires that target ahead of every cooking target, so no target that merely names
    a shader or a recipe declares a tool_dep on it.

    Staleness is a plain mtime compare against the cooked file: the source, a shader's sibling
    .hlsli files, and the cooker executables (the container format lives in them, so a rebuilt
    cooker recooks).  It does NOT consult the target's artifact: content and the C code that
    loads it change on their own schedules.  So this runs twice per build_target: before the
    up-to-date check, over the manifest the previous build wrote (an edited shader recooks with
    no C change), and again after a fresh manifest is written (a newly marked name cooks on the
    build that introduced it).  Every target that names a shader cooks from its own manifest,
    and the cooked file's mtime is what keeps a name shared by many targets from cooking more
    than once per run: the first target cooks, the rest see the fresh file and skip.
==============================================================================================*/

// The cooked extension (no dot) a recipe produces, read from its "kind" line; "" when the file
// cannot be read or names no cooking kind. Mirrors res_kind_from_name in engine/res/res_cook.h.
static void
recipe_cooked_ext( const char* recipe_path, char* out, size_t cap )
{
    out[ 0 ] = '\0';
    FILE* f  = fopen( recipe_path, "rb" );
    if ( !f )
        return;
    char line[ 512 ];
    while ( fgets( line, sizeof( line ), f ) )
    {
        const char* p = line;
        while ( *p == ' ' || *p == '\t' ) ++p;
        if ( strncmp( p, "kind", 4 ) != 0 || ( p[ 4 ] != ' ' && p[ 4 ] != '\t' ) )
            continue;
        p += 4;
        while ( *p == ' ' || *p == '\t' ) ++p;
        char word[ 32 ];
        int  n = 0;
        while ( *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && n < ( int )sizeof( word ) - 1 )
            word[ n++ ] = *p++;
        word[ n ] = '\0';
        if      ( strcmp( word, "font"   ) == 0 ) snprintf( out, cap, "orb_font" );
        else if ( strcmp( word, "image"  ) == 0 ) snprintf( out, cap, "tex" );
        else if ( strcmp( word, "shader" ) == 0 ) snprintf( out, cap, "oshd" );
        break;
    }
    fclose( f );
}

bool
build_cook_content( build_context_t* ctx, target_info_t* target, const char* obj_dir )
{
    if ( !cook_active( target ) )
        return true;

    char man_path[ PATH_MAX ];
    path_res_manifest( target, obj_dir, man_path, sizeof( man_path ) );

    platform_mapped_file_t map;
    if ( !platform_map_file( man_path, &map ) )
        return true;    // no manifest yet: the post-manifest pass cooks
    if ( map.size == 0 )
        return true;

    char roots[ 2 ][ PATH_MAX ];
    int  root_count = res_content_roots( roots );

    // ok goes false only under -strict-content; otherwise every failure below is reported and
    // the caller carries on to compile and link.
    bool        ok               = true;
    bool        asset_tool_ready = false;    // built at most once per call, the first time a
                                              // manifest entry actually needs cooking
    const char* p   = map.data;
    const char* end = map.data + map.size;
    char        line[ PATH_MAX * 2 ];
    while ( ok && mmap_next_line( &p, end, line, sizeof( line ) ) )
    {
        if ( !line[ 0 ] || line[ 0 ] == '#' )
            continue;

        // Columns: name, source file under its content root, [in <subtree>]. Neither of the
        // first two carries spaces (a name is printable ASCII without them), so whitespace
        // tokenizes the row.
        char* name = line;
        char* rel  = name;
        while ( *rel && *rel != ' ' && *rel != '\t' ) ++rel;
        if ( !*rel ) continue;
        *rel++ = '\0';
        while ( *rel == ' ' || *rel == '\t' ) ++rel;
        char* tail = rel;
        while ( *tail && *tail != ' ' && *tail != '\t' ) ++tail;
        *tail = '\0';
        if ( !*rel || name[ strlen( name ) - 1 ] == '/' )
            continue;    // a subtree row; its leaves follow as rows of their own

        const char* dot = strrchr( rel, '.' );
        if ( !dot ) continue;
        const char* src_ext = dot + 1;

        char cooked_ext[ 16 ] = "";
        char src[ PATH_MAX ]  = "";
        for ( int r = 0; r < root_count && !src[ 0 ]; ++r )
        {
            char cand[ PATH_MAX ];
            snprintf( cand, sizeof( cand ), "%s/%s", roots[ r ], rel );
            if ( platform_get_mtime( cand ) != 0 )
                snprintf( src, sizeof( src ), "%s", cand );
        }
        if ( !src[ 0 ] )
            continue;    // res_tool would have failed the manifest; a stale row is not ours to report

        bool is_shader = str_icmp( src_ext, "hlsl" ) == 0;
        if ( is_shader )
            snprintf( cooked_ext, sizeof( cooked_ext ), "oshd" );
        else if ( str_icmp( src_ext, "recipe" ) == 0 )
            recipe_cooked_ext( src, cooked_ext, sizeof( cooked_ext ) );
        if ( !cooked_ext[ 0 ] )
            continue;    // loose content: the runtime reads it from content/ as it is

        if ( !asset_tool_ready )
        {
            if ( g_asset_tool_down )
                break;    // reported on the first target that needed it

            if ( !g_asset_tool_ready )
            {
                target_info_t* asset_tool = find_asset_tool();
                const char*    down       = NULL;    // why nothing can be cooked this run

                if ( !asset_tool )
                    down = "no is_asset_tool target is registered";
                else
                {
                    // Resolve the cooker's own deps rather than trusting the caller's scheduling
                    // context (ctx->skip_deps may be true). Under the scheduler this is already
                    // done -- add_job() wires asset_tool as an implicit dep of every cooking
                    // target -- so this is the fallback for the paths without one.
                    //
                    // force_rebuild is deliberately dropped: the call only has to leave a current
                    // cooker on disk. Carrying it in recompiles asset_tool's whole closure from
                    // inside a worker, and relinking bin/sys.lib while a sibling worker links
                    // against it fails that worker with LNK1104 -- the per-target mutex orders
                    // two writers, not a writer against readers. A -force run still rebuilds the
                    // cooker through its own scheduled job.
                    build_context_t asset_ctx = *ctx;
                    asset_ctx.skip_deps       = false;
                    asset_ctx.skip_tool_deps  = false;
                    asset_ctx.force_rebuild   = false;
                    if ( !build_target( &asset_ctx, asset_tool, NULL, NULL ) )
                        down = "the content cooker did not build";
                }

                if ( down )
                {
                    if ( g_content_strict )
                    {
                        printf( ORB_INDENT "[orb error] '%s' names '%s', which needs cooking, but %s\n",
                                target->name, name, down );
                        ok = false;
                        break;
                    }
                    printf( ORB_INDENT "[orb warn] %s -- nothing is cooked into %s" PATH_SEP "content"
                                       " this run; the first name that wanted it was '%s'\n",
                            down, g_build_dir, name );
                    g_asset_tool_down = true;
                    break;    // the cooker is unusable, so no later entry can cook either
                }
                g_asset_tool_ready = true;
            }
            asset_tool_ready = true;
        }

        char dst[ PATH_MAX ];
        snprintf( dst, sizeof( dst ), "%s" PATH_SEP "content" PATH_SEP "%s.%s", g_build_dir, name, cooked_ext );
        for ( char* c = dst; *c; ++c )
            if ( *c == '/' ) *c = PATH_SEP[ 0 ];

        platform_mtime_t src_mtime = platform_get_mtime( src );

        // A newer cooker makes every cooked file stale: a format bump changes the cooked bytes
        // without touching any source.
        {
            platform_mtime_t m = platform_get_mtime( "bin" PATH_SEP "asset_tool.exe" );
            if ( m > src_mtime ) src_mtime = m;
            m = platform_get_mtime( is_shader ? "bin" PATH_SEP "shader_tool.exe" : "bin" PATH_SEP "font_tool.exe" );
            if ( m > src_mtime ) src_mtime = m;
        }

        // Shared .hlsli code beside a shader counts toward staleness: dxc resolves #include
        // relative to the including file, so an edit to a sibling include changes the cooked
        // bytes without touching the .hlsl's own mtime. The whole directory is folded in rather
        // than the actual include graph -- shader dirs hold a handful of files, and one spurious
        // recook costs less than a parser.
        if ( is_shader )
        {
            char dir[ PATH_MAX ];
            snprintf( dir, sizeof( dir ), "%s", src );
            char* sep = strrchr( dir, PATH_SEP[ 0 ] );
            char* alt = strrchr( dir, '/' );
            if ( alt > sep ) sep = alt;
            if ( sep )
            {
                *sep = '\0';
                char pattern[ PATH_MAX ];
                snprintf( pattern, sizeof( pattern ), "%s" PATH_SEP "*.hlsli", dir );

                platform_find_data_t fd;
                platform_find_t      h = platform_find_first( pattern, &fd );
                if ( h != PLATFORM_FIND_INVALID )
                {
                    do
                    {
                        char inc[ PATH_MAX ];
                        snprintf( inc, sizeof( inc ), "%s" PATH_SEP "%s", dir, fd.name );
                        platform_mtime_t m = platform_get_mtime( inc );
                        if ( m > src_mtime )
                            src_mtime = m;
                    } while ( platform_find_next( h, &fd ) );
                    platform_find_close( h );
                }
            }
        }

        // A recipe with no face line of its own takes it from the family.txt beside it
        // (asset_tool), so that file's edits change the cooked bytes too.
        if ( !is_shader )
        {
            char fam[ PATH_MAX ];
            snprintf( fam, sizeof( fam ), "%s", src );
            char* sep = strrchr( fam, PATH_SEP[ 0 ] );
            char* alt = strrchr( fam, '/' );
            if ( alt > sep ) sep = alt;
            if ( sep )
            {
                snprintf( sep + 1, sizeof( fam ) - ( size_t )( sep + 1 - fam ), "family.txt" );
                platform_mtime_t m = platform_get_mtime( fam );
                if ( m > src_mtime )
                    src_mtime = m;
            }
        }

        // Keyed on the OUTPUT, not on this target. Every target linking gui names the gui
        // shaders, and under the parallel scheduler two of them reach this at the same moment;
        // so can two build_tool invocations. The staleness test is inside the lock with the
        // cook, so the loser of the race sees the winner's fresh file and skips instead of
        // writing the same bytes underneath it.
        char key[ 256 ];
        snprintf( key, sizeof( key ), "cook_%s", name );
        for ( char* c = key; *c; ++c )
            if ( *c == '/' || *c == '.' ) *c = '_';
        void* cook_lock = build_lock_target( key );

        // -force is deliberately not consulted here. It rebuilds targets, and content staleness
        // is decoupled from target staleness (see the header of this section); honouring it
        // would recook every name once per target that lists it, which for a shader named by
        // every gui-linking target is dozens of identical cooks per run. A -force run still
        // recooks everything exactly once, because it relinks the cookers and their mtimes fold
        // into src_mtime above -- the first target cooks, the rest see the fresh file and skip.
        if ( platform_get_mtime( dst ) >= src_mtime )
        {
            build_unlock_target( cook_lock );
            continue;
        }

        {
            char dst_dir[ PATH_MAX ];
            snprintf( dst_dir, sizeof( dst_dir ), "%s", dst );
            char* sep = strrchr( dst_dir, PATH_SEP[ 0 ] );
            if ( sep ) { *sep = '\0'; ensure_dir( dst_dir ); }
        }

        if ( g_out_flags & ORB_OUT_REFLECT )
            log_printf( ORB_INDENT "[orb cook] %s\n", name );

        char cmd[ PATH_MAX * 2 ];
        snprintf( cmd, sizeof( cmd ), "bin" PATH_SEP "asset_tool.exe cook %s %s", src, dst );
        int ret = build_run_cmd( cmd );
        build_unlock_target( cook_lock );
        if ( ret != 0 )
        {
            // One uncookable file does not stop the rest: the loop condition keeps going
            // while ok holds, and ok only drops under -strict-content.
            printf( ORB_INDENT "[orb %s] '%s' content cook failed: %s\n",
                    content_severity(), target->name, src );
            if ( g_content_strict )
                ok = false;
        }
    }

    platform_unmap_file( &map );
    return ok;
}

/*  Step 6.5 hook, also the whole of a content-only refresh in step 3. Harvests the manifest,
    then cooks from it under -content so a name the fresh manifest introduced is cooked on
    the same build. A failed harvest removes the manifest, so there is nothing to cook from.
    Returns false only under -strict-content. */

static bool
content_refresh( build_context_t* ctx, target_info_t* target, const char* obj_dir, const target_info_t* res_tool )
{
    bool manifest_ok = build_gen_res_manifest( target, obj_dir, res_tool );
    if ( !manifest_ok )
        return !g_content_strict;
    return build_cook_content( ctx, target, obj_dir );
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
