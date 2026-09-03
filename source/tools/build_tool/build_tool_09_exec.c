/*==============================================================================================

    build_tool_09_exec.c -- Per-target build orchestration.

    build_target() is the core worker function. It builds one target by running
    the following phases in order:

      0. Dependency resolution  -- recurse into link deps, mono deps and tool deps first.
      1. Per-target mutex lock  -- serialize concurrent invocations on the same target.
      2. Path preparation       -- obj_dir, gen_dir, out_path.
      2.5 Content cook (pre)    -- asset_tool over the cookable names in the previous
                                   manifest (shaders, recipes), into <build>/content. Builds
                                   whichever target carries is_asset_tool on first use --
                                   see build_cook_content().
      3. Up-to-date check       -- five freshness tests (A-E); short-circuit if clean.
      4. Directory creation     -- ensure every write destination exists.
      5. Locked-file management -- rename any in-use .exe aside before relinking.
      6. Reflection codegen     -- invoke reflect_tool if has_reflect is set.
      6.5 Resource manifest     -- invoke res_tool over the image's unit closure (every exe
                                   and dynamic module); writes <name>_res_manifest.txt, then
                                   the content cook runs again over the fresh manifest.
      7. Compile + link         -- call 06_compile and 07_link; restore .exe on failure. A
                                   target with has_win_resources compiles its .rc first and
                                   links the .res in (Windows only).
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

    Content pipeline:
      Steps 2.5 and 6.5 (res_tool, asset_tool and the cookers it forwards to) produce runtime
      data only. A failure in either is reported and the target still compiles and links --
      see the Content Pipeline Strictness block in build_tool.c. -strict-content restores
      hard failure. Step 6 (reflect_tool) is fatal in both modes: its output is compiled.

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
    --- Content Cook ---

    The image's resource manifest (below) lists every content file its code names.  Some of
    those need a cooked form before the runtime can load them: a stage-tagged .hlsl becomes an
    .oshd container, a .recipe becomes the file its "kind" line says (a font bake).  This step
    cooks each such entry into the cooked mirror, <build>/content/<name>.<cooked ext>, the tree
    the host mounts above content/ so the cooked file wins by name.  Images and other loose
    content are not cooked here; the runtime reads them from content/ as they are.

    The cooker is asset_tool, which reads a shader's stage tag out of its filename and forwards
    to shader_tool, or parses a recipe and forwards to font_tool.  Any target whose closure
    names a shader or a recipe must therefore carry 'tool_dep asset_tool shader_tool font_tool'
    (gui does, which covers every image that links it) -- that is what orders the cookers
    ahead of it under the parallel scheduler.

    Staleness is a plain mtime compare against the cooked file: the source, a shader's sibling
    .hlsli files, and the cooker executables (the container format lives in them, so a rebuilt
    cooker recooks).  It deliberately does NOT consult the target's artifact: content and the
    C code that loads it change on their own schedules, and coupling them would either
    recompile the world after a one-line shader edit or leave a stale .oshd next to a fresh
    .lib.  So this runs twice per build_target: before the up-to-date check, over the manifest
    the previous build wrote (an edited shader recooks with no C change), and again after a
    fresh manifest is written (a newly marked name cooks on the build that introduced it).
==============================================================================================*/

/*  Diagnostic label for the content pipeline (res_tool, asset_tool and the cookers it
    forwards to): "error" under -strict-content, "warn" otherwise. Every call site pairs it
    with `if ( g_content_strict )` around the abort -- see the Content Pipeline Strictness
    block in build_tool.c for why the default is non-fatal. */

static const char*
content_severity( void )
{
    return g_content_strict ? "error" : "warn";
}

/*  A content tool that could not be built is unavailable for the whole run: without these
    latches every image would retry the same failing compile and repeat the same diagnostic
    (twice over, since each target cooks in two passes). Plain bools, deliberately unlocked:
    workers already past the check when one of them sets the latch repeat the message, so the
    worst case is one line per worker rather than one per target -- bounded, and never a
    correctness question. Neither is ever set under -strict-content, which returns first. */

static bool g_res_tool_down   = false;
static bool g_asset_tool_down = false;

/*  Set once the cooker has been resolved successfully, so the nested build_target() below runs
    at most once per process instead of once per cooking target. Same unlocked-bool reasoning as
    the latches above: a worker already past the check repeats the resolve, which is redundant
    work rather than a wrong answer. Under the scheduler the resolve is redundant anyway --
    add_job() wires asset_tool as an implicit dep of every target carrying a manifest -- and this
    call is the fallback for the paths that have no scheduler (-no-deps from VS, serial
    -target). */

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
    if ( !target_wants_res_manifest( target ) )
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
                break;    // reported on the first image that needed it

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
                    // done -- add_job() wires asset_tool as an implicit dep of every target
                    // carrying a manifest -- so this is the fallback for the paths without one.
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

        // Keyed on the OUTPUT, not on this target. Every image linking gui names the gui
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
        // would recook every name once per image that lists it, which for a shader named by
        // every gui-linking image is dozens of identical cooks per run. A -force run still
        // recooks everything exactly once, because it relinks the cookers and their mtimes fold
        // into src_mtime above -- the first image cooks, the rest see the fresh file and skip.
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

/*==============================================================================================
    --- Resource Manifest ---

    The image's name set is the union of the RID() / RES_TREE() tokens in its own units and
    in every library it links statically, so the scan input is the unit list of the target's
    whole link closure.  build_tool owns the graph and writes that list to
    <obj_dir>/_res_units.txt; res_tool owns the scan (it follows #include from each unit,
    so unity fragments and headers are covered), resolves every name against the content
    roots, and writes <obj_dir>/<name>_res_manifest.txt.  Nothing is compiled from it.

    Monolithic-only deps are deliberately NOT folded in: a module linked as a static lib
    under -monolithic still gets its own manifest, exactly as its DLL form does, so the exe's
    manifest must not absorb it.

    Runs only when the target is being rebuilt (after the up-to-date check): a name added
    anywhere in the closure either changes one of this target's units or headers, or
    rebuilds a dep's .lib, and each of those already makes the target stale.  A CONTENT
    change is the one input the compiler never sees, so res_tool also writes the content
    directories the manifest was computed from to <obj_dir>/_res_deps.txt, and the
    up-to-date check replays that list (test E in build_target).
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

bool
build_target( build_context_t* ctx, target_info_t* target, bool* out_skipped, uint64_t* out_elapsed_ms )
{
    if ( out_skipped    ) *out_skipped    = false;
    if ( out_elapsed_ms ) *out_elapsed_ms = 0;

    target_info_t* refl_tool = NULL;    // Located in step 0; reused in step 6.
    target_info_t* res_tool  = NULL;    // Located in step 0 when the target carries a manifest; reused in step 6.5.

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

    // Implicit res tool dep -- same two paths as reflection: built here on the serial
    // path, pre-wired as a graph dep by the scheduler. Unlike reflection, an unavailable
    // res_tool leaves res_tool NULL and step 6.5 is skipped: the manifest is a runtime
    // input, so the target still compiles unless -strict-content is set.
    if ( target_wants_res_manifest( target ) && !g_res_tool_down )
    {
        res_tool = find_res_tool();

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
                printf( ORB_INDENT "[orb error] '%s' needs a resource manifest but %s\n",
                        target->name, down );
                return false;
            }
            printf( ORB_INDENT "[orb warn] %s -- no target gets a resource manifest this run\n", down );
            g_res_tool_down = true;
            res_tool        = NULL;
        }
    }

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
    // Inside the lock for the same reason everything else is -- two invocations must not
    // write one cooked file at once -- but ahead of the up-to-date check, because a cooked
    // file is an input to the RUNTIME and not to the compiler: a shader edit must re-cook
    // without dragging a recompile behind it, and an unchanged shader must not make the
    // target look stale. Works from the manifest the previous build wrote; idempotent, and one
    // mtime compare per cookable name when there is nothing to do.
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

    // E. Content check. res_tool wrote every content directory it listed into
    //    <obj_dir>/_res_deps.txt. A directory's mtime moves when a file is added, removed
    //    or renamed inside it, which is exactly the change that alters the resource
    //    manifest (or breaks a name's resolution) without touching any source. A line
    //    starting with '!' is a content root that did not exist when the manifest was
    //    generated: it going stale means the root now exists.
    //    Equal timestamps count as stale here, unlike tests A-D: mtimes are whole seconds,
    //    and a content edit made in the same second the previous link finished would
    //    otherwise leave a manifest missing that edit until some source changed. The cost
    //    is one extra rebuild in that second; the next link lands later and the check
    //    settles. A newer res_tool.exe is stale too: how a manifest is spelled lives in the
    //    tool.
    //    A missing manifest is stale as well. res_tool deletes it and writes no deps file
    //    when a name fails to resolve, and outside -strict-content the target links anyway --
    //    without this the fresh artifact would look current and the harvest would never be
    //    retried, so the broken name would go quiet after one build.
    //    Keyed on res_tool rather than on target_wants_res_manifest: when the harvest is off
    //    for the run there is no manifest to miss and no deps to replay, and treating that as
    //    stale would recompile every image on every build until res_tool builds again.
    if ( up_to_date && res_tool )
    {
        char deps_path[ PATH_MAX ];
        snprintf( deps_path, sizeof( deps_path ), "%s" PATH_SEP "_res_deps.txt", obj_dir );
        char man_path[ PATH_MAX ];
        path_res_manifest( target, obj_dir, man_path, sizeof( man_path ) );

        platform_mapped_file_t dep_map;
        if ( !platform_file_exists( man_path ) )
        {
            up_to_date = false;
        }
        else if ( platform_get_mtime( "bin" PATH_SEP "res_tool.exe" ) >= out_mtime )
        {
            up_to_date = false;
        }
        else if ( !platform_map_file( deps_path, &dep_map ) )
        {
            // No file = no recorded content set = assume stale.
            up_to_date = false;
        }
        else
        {
            const char* p   = dep_map.data;
            const char* end = dep_map.data + dep_map.size;
            char dep[ PATH_MAX ];
            while ( up_to_date && mmap_next_line( &p, end, dep, sizeof( dep ) ) )
            {
                if ( !dep[ 0 ] ) continue;
                if ( dep[ 0 ] == '!' )
                {
                    if ( platform_file_exists( dep + 1 ) )
                        up_to_date = false;
                    continue;
                }
                platform_mtime_t d_mtime = platform_get_mtime( dep );
                if ( d_mtime == 0 || d_mtime >= out_mtime )
                    up_to_date = false;
            }
            platform_unmap_file( &dep_map );
        }
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
    // The fresh manifest may name content the previous one did not; cook it now so the image
    // that introduced a name runs against its cooked form on this same build.

    if ( res_tool )
    {
        // A failed harvest removes the manifest, so there is nothing left to cook from.
        bool manifest_ok = build_gen_res_manifest( target, obj_dir, res_tool );
        if ( !manifest_ok && g_content_strict )
        {
            result = false;
            goto cleanup;
        }
        if ( manifest_ok && !build_cook_content( ctx, target, obj_dir ) )
        {
            result = false;    // only reachable under -strict-content
            goto cleanup;
        }
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
