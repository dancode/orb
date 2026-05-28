/*==============================================================================================

    build_tool_03_registry.c -- orb.targets text file parser.

    Reads orb.targets from the project root and appends targets and solutions to
    the dynamic pools in build_tool_02_data.c (g_targets[], g_solutions[]).

    If orb.targets is missing, a warning is printed but the build continues with
    only the built-in targets (build_tool, reflect_tool). This keeps -bootstrap
    functional even on a fresh checkout before any targets file exists.

    FILE FORMAT
    -----------
    Lines beginning with # are comments. Blank lines are ignored.
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
            tool_dep     <tool dependency name>             (one per line; multiple allowed)
            reflect     [<custom reflect output name>]      (flag; optional name override)
            include_dir <path>                              (extra include dir; repeatable)
            flag        is_tool | is_build_tool | is_reflect_tool

    SOLUTION BLOCK:
        solution <name>
            out         <output directory for .sln/.vcxproj files>
            add         <target1> [target2] ...   (space-separated; line repeatable)
            nav         <navigation source directory> (optional)
            include_dir <path>                          (extra include dir; repeatable)
            flag        monolithic

    EXAMPLE:
        engine  ..\

        target my_game
            type        exe
            root        src
            folder      01_GAME
            unit        main.c
            dep         core

==============================================================================================*/
// clang-format off

/*==============================================================================================
    --- Token helpers ---
==============================================================================================*/

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
// Silently drops if the array is already full.
static void
reg_append_slot( const char** slots, int max_slots, const char* s )
{
    for ( int i = 0; i < max_slots - 1; ++i )
    {
        if ( !slots[ i ] )
        {
            slots[ i ] = pool_str( s );
            return;
        }
    }
    printf( ORB_INDENT "[orb warn] slot array full, dropped: %s\n", s );
}

// Append one string to a solution's target_names array.
static void
reg_sln_add( solution_info_t* sln, const char* name )
{
    for ( int i = 0; i < MAX_SLN_TARGETS - 1; ++i )
    {
        if ( !sln->target_names[ i ] )
        {
            sln->target_names[ i ] = pool_str( name );
            return;
        }
    }
    printf( ORB_INDENT "[orb warn] solution '%s' target list full, dropped: %s\n", sln->name, name );
}

/*==============================================================================================
    registry_load()

    Parse orb.targets and append to g_targets[] / g_solutions[].
    Returns true on success (including missing file). Returns false only on a
    hard error (pool overflow, file I/O failure after open).
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
        if ( last ) *last = '\0';
        else { base_dir[ 0 ] = '.'; base_dir[ 1 ] = '\0'; }
    }

    platform_mapped_file_t mf;
    if ( !platform_map_file( path, &mf ) )
    {
        if ( !is_external )
            printf( ORB_INDENT "[orb warn] '%s' not found -- only built-in targets (build_tool, reflect_tool) available.\n", path );
        return true;
    }

    typedef enum { MODE_NONE, MODE_TARGET, MODE_SOLUTION } parse_mode_t;
    parse_mode_t     mode    = MODE_NONE;
    target_info_t*   cur_t   = NULL;
    solution_info_t* cur_sln = NULL;

    char line[ 1024 ];
    int  lineno = 0;
    bool ok     = true;

    const char* p   = mf.data;
    const char* end = mf.data ? mf.data + mf.size : NULL;

    while ( mmap_next_line( &p, end, line, sizeof( line ) ) )
    {
        ++lineno;
        reg_strip( line );
        if ( !line[ 0 ] || line[ 0 ] == '#' )
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
            cur_t->name        = pool_str( line + 7 );
            cur_t->is_external = is_external;
            cur_sln            = NULL;
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
            cur_sln->name        = pool_str( line + 9 );
            cur_sln->is_external = is_external;
            cur_t                = NULL;
            continue;
        }

        // --- Property line (belongs to current block) ---

        char* key;
        char* val;
        reg_split_kv( line, &key, &val );

        if ( mode == MODE_TARGET && cur_t )
        {
            if ( strcmp( key, "type" ) == 0 && val )
            {
                     if ( strcmp( val, "static"  ) == 0 ) cur_t->type = TARGET_STATIC_LIB;
                else if ( strcmp( val, "dynamic" ) == 0 ) cur_t->type = TARGET_DYNAMIC_LIB;
                else                                      cur_t->type = TARGET_EXECUTABLE;
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
            else if ( strcmp( key, "folder" ) == 0 && val ) cur_t->sln_folder = pool_str( val );
            else if ( strcmp( key, "unit"   ) == 0 && val ) reg_append_slot( cur_t->units, TARGET_MAX_SLOTS, val );
            else if ( ( strcmp( key, "dep" ) == 0 || strcmp( key, "tool_dep" ) == 0 ) && val )
            {
                /* dep/tool_dep accept space-separated lists: dep sys mod core */
                const char** slots = ( key[ 0 ] == 'd' && key[ 1 ] == 'e' ) ? cur_t->deps : cur_t->tool_deps;
                char tmp[ 1024 ];
                snprintf( tmp, sizeof( tmp ), "%s", val );
                char* tok = strtok( tmp, " \t" );
                while ( tok )
                {
                    reg_append_slot( slots, TARGET_MAX_SLOTS, tok );
                    tok = strtok( NULL, " \t" );
                }
            }
            else if ( strcmp( key, "reflect" ) == 0 )
            {
                cur_t->has_reflect  = true;
                if ( val ) cur_t->reflect_name = pool_str( val );
            }
            else if ( strcmp( key, "flag" ) == 0 && val )
            {
                if ( strcmp( val, "is_tool"         ) == 0 ) cur_t->is_tool         = true;
                if ( strcmp( val, "is_build_tool"   ) == 0 ) cur_t->is_build_tool   = true;
                if ( strcmp( val, "is_reflect_tool" ) == 0 ) cur_t->is_reflect_tool = true;
            }
            else if ( strcmp( key, "include_dir" ) == 0 && val )
            {
                /* Resolve relative path against this file's directory, store absolute. */
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
                reg_append_slot( cur_t->extra_include_dirs, MAX_EXTRA_INCLUDE_DIRS, abs_buf );
            }
        }
        else if ( mode == MODE_SOLUTION && cur_sln )
        {
            if      ( strcmp( key, "out" ) == 0 && val ) cur_sln->out_dir = pool_str( val );
            else if ( strcmp( key, "nav" ) == 0 && val ) cur_sln->nav_dir = pool_str( val );
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
                reg_append_slot( cur_sln->extra_include_dirs, MAX_EXTRA_INCLUDE_DIRS, abs_buf );
            }
            else if ( strcmp( key, "add" ) == 0 && val )
            {
                // Space-separated list of target names on one line.
                char tmp[ 1024 ];
                snprintf( tmp, sizeof( tmp ), "%s", val );
                char* tok = strtok( tmp, " \t" );
                while ( tok )
                {
                    reg_sln_add( cur_sln, tok );
                    tok = strtok( NULL, " \t" );
                }
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
