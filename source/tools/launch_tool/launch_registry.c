/*==============================================================================================

    tools/launch_tool/launch_registry.c -- machine-local project registry.

    <engine>/build/.orb_projects: one absolute project path per line, engine-local and
    gitignored -- the reverse direction of a project's .orb_engine.  build_tool -create
    appends on project creation; the launcher loads, imports, and prunes.  Missing paths
    are kept but flagged (a temporarily unavailable project must not be silently dropped);
    pruning is an explicit action.

==============================================================================================*/

// clang-format off

/*==============================================================================================
    Helpers
==============================================================================================*/

static void
registry_file_path( char* out, size_t size )
{
    snprintf( out, size, "%s/build/.orb_projects", s_launch.engine_root );
}

/* Path equality with Windows semantics: case-insensitive, slash-kind-insensitive. */
static bool
registry_path_eq( const char* a, const char* b )
{
    for ( ;; ++a, ++b )
    {
        char ca = ( *a == '\\' ) ? '/' : ( *a >= 'A' && *a <= 'Z' ) ? ( char )( *a + 32 ) : *a;
        char cb = ( *b == '\\' ) ? '/' : ( *b >= 'A' && *b <= 'Z' ) ? ( char )( *b + 32 ) : *b;
        if ( ca != cb )   return false;
        if ( ca == '\0' ) return true;
    }
}

/* Basename: text after the last slash of either kind. */
static const char*
registry_basename( const char* path )
{
    const char* base = path;
    for ( const char* p = path; *p; ++p )
        if ( *p == '/' || *p == '\\' )
            base = p + 1;
    return base;
}

/* A registered path is "present" when its project file is reachable. */
static void
registry_probe( launch_project_t* prj )
{
    char probe[ LAUNCH_PATH_MAX + 16 ];
    snprintf( probe, sizeof( probe ), "%s/orb.targets", prj->path );
    prj->present = sys_file_exists( probe );
}

static bool
registry_save( void )
{
    char dir[ LAUNCH_PATH_MAX + 16 ];
    snprintf( dir, sizeof( dir ), "%s/build", s_launch.engine_root );
    sys_dir_make( dir );

    char path[ LAUNCH_PATH_MAX + 32 ];
    registry_file_path( path, sizeof( path ) );

    FILE* f = fopen( path, "w" );
    if ( !f )
        return false;
    for ( u32 i = 0; i < s_launch.project_count; ++i )
        fprintf( f, "%s\n", s_launch.projects[ i ].path );
    fclose( f );
    return true;
}

/*==============================================================================================
    Registry API (declared in launch_tool.h)
==============================================================================================*/

void
launch_registry_load( void )
{
    s_launch.project_count = 0;
    s_launch.selected      = -1;

    char path[ LAUNCH_PATH_MAX + 32 ];
    registry_file_path( path, sizeof( path ) );

    sys_file_data_t fd = sys_file_read_entire( path );
    if ( !fd.ok )
        return;    /* no registry yet -- empty list */

    char* p = ( char* )fd.data;    /* NUL-terminated by contract (sys_file_data_t) */
    while ( *p && s_launch.project_count < LAUNCH_MAX_PROJECTS )
    {
        char* nl = strchr( p, '\n' );
        if ( nl ) *nl = '\0';

        size_t len = strlen( p );
        while ( len > 0 && ( p[ len - 1 ] == '\r' || p[ len - 1 ] == ' ' ) ) p[ --len ] = '\0';

        if ( len > 0 )
        {
            launch_project_t* prj = &s_launch.projects[ s_launch.project_count++ ];
            snprintf( prj->path, sizeof( prj->path ), "%s", p );
            snprintf( prj->name, sizeof( prj->name ), "%s", registry_basename( prj->path ) );
            registry_probe( prj );
        }
        if ( !nl ) break;
        p = nl + 1;
    }
    sys_file_free( &fd );
}

/* Import a project by path.  Tolerates pasted decoration (surrounding quotes, trailing
   slash).  Validates <path>/orb.targets exists; a duplicate selects the existing entry.
   Returns false with nothing changed when the path does not hold a project. */
bool
launch_registry_add( const char* path_in )
{
    char path[ LAUNCH_PATH_MAX ];
    snprintf( path, sizeof( path ), "%s", path_in );

    /* Trim whitespace and quotes off both ends, then any trailing slash. */
    size_t len = strlen( path );
    while ( len > 0 && ( path[ len - 1 ] == ' ' || path[ len - 1 ] == '"' ) ) path[ --len ] = '\0';
    char* s = path;
    while ( *s == ' ' || *s == '"' ) ++s;
    len = strlen( s );
    while ( len > 1 && ( s[ len - 1 ] == '/' || s[ len - 1 ] == '\\' ) ) s[ --len ] = '\0';

    if ( !s[ 0 ] )
        return false;

    for ( u32 i = 0; i < s_launch.project_count; ++i )
    {
        if ( registry_path_eq( s_launch.projects[ i ].path, s ) )
        {
            s_launch.selected = ( i32 )i;    /* already registered -- just select it */
            return true;
        }
    }

    char probe[ LAUNCH_PATH_MAX + 16 ];
    snprintf( probe, sizeof( probe ), "%s/orb.targets", s );
    if ( !sys_file_exists( probe ) )
        return false;

    if ( s_launch.project_count >= LAUNCH_MAX_PROJECTS )
        return false;

    launch_project_t* prj = &s_launch.projects[ s_launch.project_count++ ];
    snprintf( prj->path, sizeof( prj->path ), "%s", s );
    snprintf( prj->name, sizeof( prj->name ), "%s", registry_basename( prj->path ) );
    registry_probe( prj );

    s_launch.selected = ( i32 )( s_launch.project_count - 1 );
    return registry_save();
}

/* Drop entries whose path no longer holds a project.  Explicit action, never automatic. */
void
launch_registry_remove_missing( void )
{
    u32 keep = 0;
    for ( u32 i = 0; i < s_launch.project_count; ++i )
    {
        if ( s_launch.projects[ i ].present )
            s_launch.projects[ keep++ ] = s_launch.projects[ i ];
    }
    if ( keep != s_launch.project_count )
    {
        s_launch.project_count = keep;
        s_launch.selected      = -1;
        registry_save();
    }
}

/*============================================================================================*/
// clang-format on
