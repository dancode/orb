/*==============================================================================================

    build_tool_02_registry.c -- orb.targets text file parser.

    Reads orb.targets from the project root and appends targets and solutions to
    the dynamic pools in build_tool_02_data.c (g_targets[], g_solutions[]).

    If orb.targets is missing, a warning is printed but the build continues with
    only the built-in targets (build_tool, reflect_tool). This keeps -bootstrap
    functional even on a fresh checkout before any targets file exists.

    FILE FORMAT
    -----------
    Lines beginning with # are comments. Blank lines are ignored.
    Tokens are separated by whitespace. Blocks begin with a keyword at column 0.

    TARGET BLOCK:
        target <name>
            type    static | dynamic | exe
            root    <source directory relative to project root>
            folder  <VS solution virtual folder>
            unit    <unity entry .c filename>           (one per line; multiple allowed)
            dep     <dependency target name>            (one per line; multiple allowed)
            tool_dep <tool dependency name>             (one per line; multiple allowed)
            reflect [<custom reflect output name>]      (flag; optional name override)
            flag    is_tool | is_build_tool | is_reflect_tool

    SOLUTION BLOCK:
        solution <name>
            out     <output directory for .sln/.vcxproj files>
            add     <target1> [target2] ...   (space-separated; line repeatable)
            nav     <navigation source directory> (optional)
            flag    monolithic

    EXAMPLE:
        target core
            type    static
            root    source/engine/core
            folder  02_ENGINE
            unit    core.c
            dep     sys
            dep     rs
            reflect

        solution orb_core
            out     build/proj
            add     core

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
registry_load( const char* path )
{
    platform_mapped_file_t mf;
    if ( !platform_map_file( path, &mf ) )
    {
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

    while ( p && p < end )
    {
        /* Copy one line from the mapped view into the mutable line buffer. */
        const char* nl  = (const char*)memchr( p, '\n', (size_t)( end - p ) );
        size_t      len = nl ? (size_t)( nl - p ) : (size_t)( end - p );
        if ( len >= sizeof( line ) ) len = sizeof( line ) - 1;
        memcpy( line, p, len );
        line[ len ] = '\0';
        p = nl ? nl + 1 : end;

        ++lineno;
        reg_strip( line );
        if ( !line[ 0 ] || line[ 0 ] == '#' )
            continue;

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
            cur_t->name = pool_str( line + 7 );
            cur_sln     = NULL;
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
            cur_sln->name = pool_str( line + 9 );
            cur_t         = NULL;
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
            else if ( strcmp( key, "root"   ) == 0 && val ) cur_t->root_dir   = pool_str( val );
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
        }
        else if ( mode == MODE_SOLUTION && cur_sln )
        {
            if      ( strcmp( key, "out" ) == 0 && val ) cur_sln->out_dir = pool_str( val );
            else if ( strcmp( key, "nav" ) == 0 && val ) cur_sln->nav_dir = pool_str( val );
            else if ( strcmp( key, "flag" ) == 0 && val )
            {
                if ( strcmp( val, "monolithic" ) == 0 ) cur_sln->is_monolithic = true;
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
