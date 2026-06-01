/*==============================================================================================

    build_tool_03_registry.c -- orb.targets text file parser.

    Reads orb.targets from the project root and appends targets and solutions to
    the dynamic pools in build_tool_02_data.c (g_targets[], g_solutions[]).

    If orb.targets is missing, a warning is printed but the build continues with
    only the built-in targets (build_tool, reflect_tool). This keeps -bootstrap
    functional even on a fresh checkout before any targets file exists.

    FILE FORMAT
    -----------
    Lines beginning with # are comments; # also ends any line mid-way (inline comment). Blank lines are ignored.
    Tokens are separated by whitespace. Blocks begin with a keyword at column 0.

    TOP-LEVEL DIRECTIVES (before any target/solution block):

        engine <path>
            Declares the engine installation this project builds on.
            <path> may be relative (resolved against this file's directory) or absolute.
            Effect:
              - Sets g_engine_root to the resolved absolute path.
              - Auto-imports <path>/orb.targets with is_external=true (all engine
                targets become available as deps but excluded from local build/gen/clean).
              - Engine source headers (<engine>/source, <engine>/build/generated) are
                automatically added to the include path for all local targets.
              - Built-in targets (build_tool, reflect_tool) resolve their paths against
                the engine root and are marked external.
            Only processed from the root orb.targets (not from imports).

        import <path>
            Merge another .targets file. All targets/solutions loaded from it are
            marked external. Path relative to this file's directory.

    TARGET BLOCK:
        target <name>
            type        static | dynamic | exe
            root        <source directory relative to project root>
            folder      <VS solution virtual folder>
            unit        <unity entry .c filename>           (one per line; multiple allowed)
            dep         <dependency target name>            (one per line; multiple allowed)
            tool_dep    <tool dependency name>              (one per line; multiple allowed)
            mono_dep    <monolithic-only link dep>          (one per line; multiple allowed)
            reflect_name <name>                             (override generated file base name)
            inc         <path>                              (extra include dir; alias for include_dir)
            include_dir <path>                              (extra include dir; repeatable)
            define      <NAME>[=value]                      (per-target preprocessor define; repeatable)
            compile_flag <msvc|clang|all> <flag>            (per-target compiler flag; repeatable)
            subsystem   console | windows                   (exe only; /SUBSYSTEM: on Win32; default: console)
            link_flag   <msvc|clang|all> <flag>             (per-target linker flag; repeatable)

            flag        <token1> [token2] ...               (space-separated list of boolean flags)
                            reflect                         -- run reflect_tool before compile
                            host_only                       -- engine service; dynamic targets may not dep this
                            is_tool | is_build_tool | is_reflect_tool

    SOLUTION BLOCK:
        solution <name>
            out         <output directory for .sln/.vcxproj files>
            add         <target1> [target2] ...             (space-separated; line repeatable)
            nav         <navigation source directory>       (optional)
            include_dir <path>                              (IntelliSense-only; repeatable)
            flag        monolithic

        NOTE: solution-level 'include_dir' is forwarded only to vcxproj IntelliSense
        (AdditionalIncludeDirectories / NMakeIncludeSearchPath). It does NOT inject /I
        flags into the actual cl.exe invocation. This is intentional: a target is a
        single shared artifact and may appear in multiple solutions. Wiring solution
        settings into the compile would make builds non-reproducible depending on which
        solution triggered them. Anything that must affect compilation belongs on the
        target, not the solution.

    EXAMPLE:

        # Declare the engine root (child projects only; not used when building the engine itself).
        # Relative paths resolve against this file's directory.
        engine  ..\

        # Pull in another .targets file; all its targets are marked external (available as
        # deps but excluded from local -gen / clean / "build all").
        import  platform\platform.targets

        # --- Target: static library ---
        target base
            type        static
            root        source/base             # source directory; relative to project root
            folder      01_BASE                 # display-only folder in VS Solution Explorer
            unit        base.c                  # unity entry file; repeat for multiple TUs

        # --- Target: dynamic library with reflection and per-target settings ---
        target render
            type        dynamic
            root        source/render
            folder      02_RENDER
            unit        render.c
            dep         core sys                # link deps; space-separated or one per line
            tool_dep    asset_compiler          # must exist before building; not linked
            mono_dep    audio                   # linked only in monolithic (-mono) builds
            flag        reflect                 # run reflect_tool before compile
            reflect_name render_types           # optional: override generated file base name
            inc         source/render/private   # extra /I flag; real compile + IntelliSense
            include_dir third_party/stb         # alias for inc
            define      RENDER_VALIDATION       # per-target /D flag; real compile + IntelliSense
            define      RENDER_MAX_FRAMES=3
            compile_flag  msvc   /GS-           # appended only when compiling with cl.exe
            compile_flag  clang  -fno-stack..   # appended only when compiling with clang
            compile_flag  all    /WX            # appended for every compiler

        # --- Target: executable (windowed app) ---
        target game
            type        exe
            root        source/game
            folder      03_GAME
            unit        main.c
            dep         core render audio
            subsystem   windows                 # /SUBSYSTEM:WINDOWS (WinMain; no console window)
            link_flag   msvc /STACK:4194304     # 4 MB stack; appended only when linking with link.exe
            link_flag   msvc /WHOLEARCHIVE:bin/core.lib  # force-include all symbols from core.lib
            link_flag   all  /OPT:REF           # strip unreferenced symbols; safe for both toolchains

        # --- Target: host-only engine service (dynamic targets may not dep this) ---
        target core
            type        static
            root        source/engine/core
            folder      02_ENGINE
            unit        core.c
            dep         sys ref
            flag        reflect host_only       # multiple flags on one line

        # --- Target: build-time tool (survives global clean; built as a dep automatically) ---
        target asset_compiler
            type        exe
            root        source/tools/asset_compiler
            folder      08_TOOLS
            unit        asset_compiler.c
            flag        is_tool

        # --- Solution: standard modular workspace ---
        solution my_project
            out         build/proj              # output directory for .sln/.vcxproj files
            nav         source                  # optional: navigation-only project for all sources
            add         base core sys render    # targets to include; space-separated, repeatable
            add         audio physics game
            include_dir third_party/headers     # IntelliSense-only /I hint for all targets in this solution

        # --- Solution: monolithic (all DLLs compiled as static libs) ---
        solution my_project_mono
            out         build/proj_mono
            flag        monolithic
            add         my_project              # expand an existing solution's target list by name

        # --- Solution: narrow workspace for a single subsystem ---
        solution render_only
            out         build/proj
            add         core render

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Token helpers ---
==============================================================================================*/

// Parse 'val' as "<msvc|clang|all> <flag>" and append to slots[]/count if valid.
// keyword is "compile_flag" or "link_flag" and appears only in the message.
// Returns false on a hard error (slot table full); true otherwise.
static bool
reg_parse_flag_entry( const char* path, int lineno, const char* keyword,
                      const char* val, const char* target_name,
                      extra_flag_t* slots, int* count, int max )
{
    char  tmp[ 256 ];
    snprintf( tmp, sizeof( tmp ), "%s", val );
    char* flagp = tmp;
    while ( *flagp && (unsigned char)*flagp > ' ' ) flagp++;
    if ( *flagp ) { *flagp++ = '\0'; while ( *flagp && (unsigned char)*flagp <= ' ' ) flagp++; }

    compiler_t comp       = COMPILE_ALL;
    bool       comp_valid = true;
    if      ( strcmp( tmp, "msvc"  ) == 0 ) comp = COMPILE_MSVC;
    else if ( strcmp( tmp, "clang" ) == 0 ) comp = COMPILE_CLANG;
    else if ( strcmp( tmp, "all"   ) == 0 ) comp = COMPILE_ALL;
    else
    {
        printf( ORB_INDENT "[orb warn] %s:%d -- unknown compiler '%s' in %s"
                           " (use msvc/clang/all)\n", path, lineno, tmp, keyword );
        comp_valid = false;
    }

    if ( comp_valid && *flagp )
    {
        if ( *count < max )
        {
            extra_flag_t* ef = &slots[ (*count)++ ];
            ef->compiler = comp;
            snprintf( ef->flag, sizeof( ef->flag ), "%s", flagp );
        }
        else
        {
            printf( ORB_INDENT "[orb error] %s:%d -- %s slot full (max %d) for '%s',"
                               " dropped: %s\n", path, lineno, keyword, max, target_name, flagp );
            return false;
        }
    }
    return true;
}

// Strip in-place: remove leading whitespace, trailing whitespace, and trailing \r\n.
static void
reg_strip( char* s )
{
    // Trim trailing.
    char* end = s + strlen( s );
    while ( end > s && ( (unsigned char)end[ -1 ] <= ' ' ) )
        *--end = '\0';

    // Shift leading whitespace away.
    char* p = s;
    while ( *p && (unsigned char)*p <= ' ' )
        p++;
    if ( p != s )
        memmove( s, p, strlen( p ) + 1 );
}

// Split "key rest..." into key (first whitespace-delimited token) and rest (remainder).
// Both output pointers point into the same buffer `line`. rest may be NULL if no value.
static void
reg_split_kv( char* line, char** out_key, char** out_val )
{
    *out_key = line;
    *out_val = NULL;

    char* p = line;
    while ( *p && (unsigned char)*p > ' ' ) p++;    // skip key
    if ( *p )
    {
        *p++ = '\0';
        while ( *p && (unsigned char)*p <= ' ' ) p++;   // skip whitespace
        if ( *p ) *out_val = p;
    }
}

// Append one string to a NULL-terminated slot array (units/deps/tool_deps).
// Returns false on a hard error: a full array means an entry would be dropped,
// which silently corrupts the build graph -- the caller aborts the load instead.
// The last slot is reserved as the NULL terminator, so capacity is max_slots-1.
static bool
reg_append_slot( const char** slots, int max_slots, const char* s )
{
    for ( int i = 0; i < max_slots - 1; ++i )
    {
        if ( !slots[ i ] )
        {
            slots[ i ] = pool_str( s );
            return true;
        }
    }
    printf( ORB_INDENT "[orb error] slot array full (max %d), dropped: %s\n", max_slots - 1, s );
    return false;
}

// Append one string to a solution's target_names array.
// Returns false on a hard error (list full) for the same reason as reg_append_slot.
static bool
reg_sln_add( solution_info_t* sln, const char* name )
{
    for ( int i = 0; i < MAX_SLN_TARGETS - 1; ++i )
    {
        if ( !sln->target_names[ i ] )
        {
            sln->target_names[ i ] = pool_str( name );
            return true;
        }
    }
    printf( ORB_INDENT "[orb error] solution '%s' target list full (max %d), dropped: %s\n",
            sln->name, MAX_SLN_TARGETS - 1, name );
    return false;
}

/*==============================================================================================
    registry_load()

    Parse orb.targets and append to g_targets[] / g_solutions[].
    Returns true on success (including missing file). 
    Returns false only on a hard error (pool overflow, file I/O failure after open).
==============================================================================================*/

static bool
registry_load( const char* path, bool is_external )
{
    // Compute the directory of this file so relative 'root' and 'import' paths
    // resolve against the declaring file, not against build_tool's working directory.
    char base_dir[ PATH_MAX ];
    {
        char abs_path[ PATH_MAX ];

        if ( !platform_fullpath( abs_path, path, sizeof( abs_path ) ) )
            snprintf( abs_path, sizeof( abs_path ), "%s", path );
        snprintf( base_dir, sizeof( base_dir ), "%s", abs_path );
        char* last = NULL;
        for ( char* cp = base_dir; *cp; ++cp )
            if ( *cp == '/' || *cp == '\\' ) last = cp;
        if ( last ) { *last = '\0'; }
        else { base_dir[ 0 ] = '.'; base_dir[ 1 ] = '\0'; }
        
        // The '.' is a fallback when no directory component exists.
        // It means the current working directory (rare occurance).
    }

    platform_mapped_file_t mf;
    if ( !platform_map_file( path, &mf ) )
    {
        if ( !is_external )
            printf( ORB_INDENT "[orb warn] '%s' not found -- only built-in targets (build_tool, reflect_tool) available.\n", path );
        return true;
    }

    typedef enum { MODE_NONE, MODE_SKIP, MODE_TARGET, MODE_SOLUTION } parse_mode_t;
    parse_mode_t     mode    = MODE_NONE;
    target_info_t*   cur_t   = NULL;
    solution_info_t* cur_sln = NULL;

    char line[ 1024 ];
    int  lineno = 0;
    bool ok = true;

    const char* p   = mf.data;
    const char* end = mf.data ? mf.data + mf.size : NULL;

    while ( mmap_next_line( &p, end, line, sizeof( line ) ) )
    {
        ++lineno;
        reg_strip( line );
        // Strip inline comments: truncate at the first '#' and re-trim trailing whitespace.
        { char* cp = strchr( line, '#' ); if ( cp ) { *cp = '\0'; reg_strip( line ); } }
        if ( !line[ 0 ] )
            continue;

        // --- engine: declare engine root; auto-import engine/orb.targets as external ---
        // Only processed from the root orb.targets (is_external == false).

        if ( !is_external && strncmp( line, "engine ", 7 ) == 0 )
        {
            const char* rel = line + 7;
            while ( *rel == ' ' || *rel == '\t' ) ++rel;
            char engine_path[ PATH_MAX ];
            if ( platform_is_abs_path( rel ) )
            {
                snprintf( engine_path, sizeof( engine_path ), "%s", rel );
            }
            else
            {
                char combined[ PATH_MAX ];
                snprintf( combined, sizeof( combined ), "%s/%s", base_dir, rel );
                if ( !platform_fullpath( engine_path, combined, sizeof( engine_path ) ) )
                    snprintf( engine_path, sizeof( engine_path ), "%s", combined );
            }
            snprintf( g_engine_root, sizeof( g_engine_root ), "%s", engine_path );

            char engine_targets[ PATH_MAX ];
            snprintf( engine_targets, sizeof( engine_targets ), "%s/orb.targets", engine_path );
            if ( !registry_load( engine_targets, true ) ) { ok = false; break; }
            continue;
        }

        // --- import: merge another .targets file; path relative to this file ---

        if ( strncmp( line, "import ", 7 ) == 0 )
        {
            const char* rel = line + 7;
            while ( *rel == ' ' || *rel == '\t' ) ++rel;
            char import_path[ PATH_MAX ];
            if ( platform_is_abs_path( rel ) )
            {
                snprintf( import_path, sizeof( import_path ), "%s", rel );
            }
            else
            {
                char combined[ PATH_MAX ];
                snprintf( combined, sizeof( combined ), "%s/%s", base_dir, rel );
                if ( !platform_fullpath( import_path, combined, sizeof( import_path ) ) )
                    snprintf( import_path, sizeof( import_path ), "%s", combined );
            }
            // The recursive call opens a separate mapping; our mapping is unaffected.
            // Imported targets/solutions are always marked external regardless of depth.
            if ( !registry_load( import_path, true ) ) { ok = false; break; }
            continue;
        }

        // --- Block openers (must start at column 0 after strip) ---

        // "target" with no name: reg_strip eats the trailing space so the line
        // never matches "target <name>" -- catch it here before falling through.
        if ( strcmp( line, "target" ) == 0 )
        {
            printf( ORB_INDENT "[orb error] %s:%d -- target declared with no name\n",
                    path, lineno );
            ok      = false;
            mode    = MODE_SKIP;
            cur_t   = NULL;
            cur_sln = NULL;
            continue;
        }

        if ( strncmp( line, "target ", 7 ) == 0 )
        {
            if ( g_target_count >= MAX_TARGETS )
            {
                printf( ORB_INDENT "[orb error] %s:%d -- too many targets (MAX_TARGETS=%d)\n",
                        path, lineno, MAX_TARGETS );
                ok = false;
                break;
            }
            mode  = MODE_TARGET;
            cur_t = &g_targets[ g_target_count++ ];
            memset( cur_t, 0, sizeof( *cur_t ) );
            { const char* n = line + 7; while ( *n == ' ' || *n == '\t' ) ++n; cur_t->name = pool_str( n ); }
            cur_t->is_external = is_external;
            cur_sln            = NULL;
            continue;
        }

        if ( strcmp( line, "solution" ) == 0 )
        {
            printf( ORB_INDENT "[orb error] %s:%d -- solution declared with no name\n",
                    path, lineno );
            ok      = false;
            mode    = MODE_SKIP;
            cur_t   = NULL;
            cur_sln = NULL;
            continue;
        }

        if ( strncmp( line, "solution ", 9 ) == 0 )
        {
            if ( g_solution_count >= MAX_SOLUTIONS )
            {
                printf( ORB_INDENT "[orb error] %s:%d -- too many solutions (MAX_SOLUTIONS=%d)\n",
                        path, lineno, MAX_SOLUTIONS );
                ok = false;
                break;
            }
            mode    = MODE_SOLUTION;
            cur_sln = &g_solutions[ g_solution_count++ ];
            memset( cur_sln, 0, sizeof( *cur_sln ) );
            { const char* n = line + 9; while ( *n == ' ' || *n == '\t' ) ++n; cur_sln->name = pool_str( n ); }
            cur_sln->is_external = is_external;
            cur_t                = NULL;
            continue;
        }

        // --- Property line (belongs to current block) ---

        // Properties belonging to a block that errored on declaration are silently dropped.
        if ( mode == MODE_SKIP ) continue;

        char* key;
        char* val;
        reg_split_kv( line, &key, &val );

        if ( mode == MODE_TARGET && cur_t )
        {
            if ( strcmp( key, "type" ) == 0 && val )
            {
                if      ( strcmp( val, "static"  ) == 0 ) { cur_t->type = TARGET_STATIC_LIB;  cur_t->has_type = true; }
                else if ( strcmp( val, "dynamic" ) == 0 ) { cur_t->type = TARGET_DYNAMIC_LIB; cur_t->has_type = true; }
                else if ( strcmp( val, "exe"     ) == 0 ) { cur_t->type = TARGET_EXECUTABLE;  cur_t->has_type = true; }
                else
                {
                    printf( ORB_INDENT "[orb error] %s:%d -- target '%s': unknown type '%s'"
                                       " (use static/dynamic/exe)\n", path, lineno, cur_t->name, val );
                    ok = false;
                }
            }
            else if ( strcmp( key, "root" ) == 0 && val )
            {
                // Resolve relative roots against the declaring file's directory so
                // targets in imported .targets files point back to their own source tree.
                if ( platform_is_abs_path( val ) )
                {
                    cur_t->root_dir = pool_str( val );
                }
                else
                {
                    char combined[ PATH_MAX ], abs_buf[ PATH_MAX ];
                    snprintf( combined, sizeof( combined ), "%s/%s", base_dir, val );
                    if ( !platform_fullpath( abs_buf, combined, sizeof( abs_buf ) ) )
                        snprintf( abs_buf, sizeof( abs_buf ), "%s", combined );
                    cur_t->root_dir = pool_str( abs_buf );
                }
            }
            else if ( strcmp( key, "folder" ) == 0 && val ) cur_t->virtual_folder = pool_str( val );
            else if ( strcmp( key, "unit"   ) == 0 && val ) { if ( !reg_append_slot( cur_t->units, TARGET_MAX_SLOTS, val ) ) ok = false; }
            else if ( ( strcmp( key, "dep" ) == 0 || strcmp( key, "tool_dep" ) == 0
                     || strcmp( key, "mono_dep" ) == 0 ) && val )
            {
                /* dep/tool_dep/mono_dep accept space-separated lists: dep sys mod core */
                const char** slots = ( strcmp( key, "tool_dep" ) == 0 ) ? cur_t->tool_deps
                                   : ( strcmp( key, "mono_dep" ) == 0 ) ? cur_t->mono_deps
                                   : cur_t->deps;
                char tmp[ 1024 ];
                snprintf( tmp, sizeof( tmp ), "%s", val );
                char* tok = strtok( tmp, " \t" );
                while ( tok )
                {
                    if ( !reg_append_slot( slots, TARGET_MAX_SLOTS, tok ) ) ok = false;
                    tok = strtok( NULL, " \t" );
                }
            }
            else if ( strcmp( key, "reflect_name" ) == 0 && val )
            {
                cur_t->reflect_name = pool_str( val );
            }
            else if ( strcmp( key, "flag" ) == 0 && val )
            {
                /* flag accepts a space-separated list: flag  reflect host_only is_tool */
                char tmp[ 256 ];
                snprintf( tmp, sizeof( tmp ), "%s", val );
                char* tok = strtok( tmp, " \t" );
                while ( tok )
                {
                    if      ( strcmp( tok, "reflect"         ) == 0 ) cur_t->has_reflect    = true;
                    else if ( strcmp( tok, "host_only"       ) == 0 ) cur_t->is_host_only   = true;
                    else if ( strcmp( tok, "is_tool"         ) == 0 ) cur_t->is_tool        = true;
                    else if ( strcmp( tok, "is_build_tool"   ) == 0 ) cur_t->is_build_tool  = true;
                    else if ( strcmp( tok, "is_reflect_tool" ) == 0 ) cur_t->is_reflect_tool = true;
                    else
                        printf( ORB_INDENT "[orb warn] %s:%d -- target '%s': unknown flag '%s'\n",
                                path, lineno, cur_t->name, tok );
                    tok = strtok( NULL, " \t" );
                }
            }
            else if ( ( strcmp( key, "include_dir" ) == 0 || strcmp( key, "inc" ) == 0 ) && val )
            {
                /* 'inc' is an alias for 'include_dir'. Resolve relative paths against this
                   file's directory so targets in imported .targets files point to their own tree. */
                char abs_buf[ PATH_MAX ];
                if ( platform_is_abs_path( val ) )
                {
                    snprintf( abs_buf, sizeof( abs_buf ), "%s", val );
                }
                else
                {
                    char combined[ PATH_MAX ];
                    snprintf( combined, sizeof( combined ), "%s/%s", base_dir, val );
                    if ( !platform_fullpath( abs_buf, combined, sizeof( abs_buf ) ) )
                        snprintf( abs_buf, sizeof( abs_buf ), "%s", combined );
                }
                if ( !reg_append_slot( cur_t->extra_include_dirs, MAX_EXTRA_INCLUDE_DIRS, abs_buf ) ) ok = false;
            }
            else if ( strcmp( key, "define" ) == 0 && val )
            {
                /* Per-target preprocessor define: appended after global tables at compile time
                   and forwarded to IntelliSense PreprocessorDefinitions in vcxproj gen. */
                if ( !reg_append_slot( cur_t->extra_defines, MAX_EXTRA_DEFINES, val ) ) ok = false;
            }
            else if ( strcmp( key, "compile_flag" ) == 0 && val )
            {
                /* Per-target compiler flag: 'compile_flag <msvc|clang|all> <flag>'.
                   Only appended when the active compiler matches. Not forwarded to IntelliSense. */
                if ( !reg_parse_flag_entry( path, lineno, "compile_flag", val, cur_t->name,
                                            cur_t->extra_compile_flags, &cur_t->extra_compile_flag_count,
                                            MAX_EXTRA_COMPILE_FLAGS ) ) ok = false;
            }
            else if ( strcmp( key, "subsystem" ) == 0 && val )
            {
                /* Executable subsystem: 'subsystem <console|windows>'.
                   Translated to /SUBSYSTEM: by the platform linker fill. Ignored for DLL/LIB. */
                if      ( strcmp( val, "console" ) == 0 ) cur_t->subsystem = SUBSYSTEM_CONSOLE;
                else if ( strcmp( val, "windows" ) == 0 ) cur_t->subsystem = SUBSYSTEM_WINDOWS;
                else
                    printf( ORB_INDENT "[orb warn] %s:%d -- unknown subsystem '%s'"
                                       " (use console/windows)\n", path, lineno, val );
            }
            else if ( strcmp( key, "link_flag" ) == 0 && val )
            {
                /* Per-target linker flag: 'link_flag <msvc|clang|all> <flag>'.
                   Only appended when the active linker toolchain matches. */
                if ( !reg_parse_flag_entry( path, lineno, "link_flag", val, cur_t->name,
                                            cur_t->extra_link_flags, &cur_t->extra_link_flag_count,
                                            MAX_EXTRA_LINK_FLAGS ) ) ok = false;
            }
            else
            {
                printf( ORB_INDENT "[orb error] %s:%d -- target '%s': unknown property '%s'\n",
                        path, lineno, cur_t->name, key );
                ok = false;
            }
        }
        else if ( mode == MODE_SOLUTION && cur_sln )
        {
            if      ( strcmp( key, "out"     ) == 0 && val ) cur_sln->out_dir         = pool_str( val );
            else if ( strcmp( key, "nav"     ) == 0 && val ) cur_sln->nav_dir         = pool_str( val );
            else if ( strcmp( key, "startup" ) == 0 && val ) cur_sln->startup_project = pool_str( val );
            else if ( strcmp( key, "flag" ) == 0 && val )
            {
                if ( strcmp( val, "monolithic" ) == 0 ) cur_sln->is_monolithic = true;
            }
            else if ( strcmp( key, "include_dir" ) == 0 && val )
            {
                char abs_buf[ PATH_MAX ];
                if ( platform_is_abs_path( val ) )
                {
                    snprintf( abs_buf, sizeof( abs_buf ), "%s", val );
                }
                else
                {
                    char combined[ PATH_MAX ];
                    snprintf( combined, sizeof( combined ), "%s/%s", base_dir, val );
                    if ( !platform_fullpath( abs_buf, combined, sizeof( abs_buf ) ) )
                        snprintf( abs_buf, sizeof( abs_buf ), "%s", combined );
                }
                if ( !reg_append_slot( cur_sln->extra_include_dirs, MAX_EXTRA_INCLUDE_DIRS, abs_buf ) ) ok = false;
            }
            else if ( strcmp( key, "add" ) == 0 && val )
            {
                // Space-separated list of target or solution names on one line.
                // If a name matches a solution, expand it to all that solution's targets
                // so the child project can pull in an entire engine solution with one line.
                char tmp[ 1024 ];
                snprintf( tmp, sizeof( tmp ), "%s", val );
                char* tok = strtok( tmp, " \t" );
                while ( tok )
                {
                    // Target match takes priority. Only expand as a solution if no
                    // target by that name exists -- prevents 'add my_project' from
                    // accidentally matching the solution of the same name (which is
                    // still being built and has an empty target list at this point).
                    if ( find_target( tok ) )
                    {
                        if ( !reg_sln_add( cur_sln, tok ) ) ok = false;
                    }
                    else
                    {
                        solution_info_t* ref = NULL;
                        for ( int i = 0; i < g_solution_count; ++i )
                        {
                            if ( g_solutions[ i ].name && strcmp( g_solutions[ i ].name, tok ) == 0 )
                            {
                                ref = &g_solutions[ i ];
                                break;
                            }
                        }
                        if ( ref )
                        {
                            for ( int i = 0; i < MAX_SLN_TARGETS && ref->target_names[ i ]; ++i )
                                if ( !reg_sln_add( cur_sln, ref->target_names[ i ] ) ) ok = false;
                        }
                        else
                        {
                            if ( !reg_sln_add( cur_sln, tok ) ) ok = false;
                        }
                    }
                    tok = strtok( NULL, " \t" );
                }
            }
            else
            {
                printf( ORB_INDENT "[orb error] %s:%d -- solution '%s': unknown property '%s'\n",
                        path, lineno, cur_sln->name, key );
                ok = false;
            }
        }
        else
        {
            printf( ORB_INDENT "[orb warn] %s:%d -- orphan property '%s' (no active target/solution block)\n",
                    path, lineno, key );
        }
    }

    platform_unmap_file( &mf );
    return ok;
}

// clang-format on
/*============================================================================================*/
