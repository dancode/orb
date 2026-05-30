/*==============================================================================================

    build_tool_13_create.c -- -create command: scaffold a new engine module.

    Emits the standard header and source file set for a static or dynamic module,
    then prints the orb.targets stanza to stdout for copy-paste registration.

    Usage:
        build_tool.exe -create <name> -dir <source/path> [-type static|dynamic]

    Static (engine-style, like sys/core/app):
        <name>.h  <name>_api.h  <name>_host.h  <name>.c  <name>_api.c

    Dynamic (hot-reload DLL, like render/audio):
        <name>.h  <name>_api.h  <name>.c

==============================================================================================*/
// clang-format off

#include <ctype.h>

/* ---- String helpers ---- */

static void
create_str_upper( const char* src, char* buf, size_t buf_size )
{
    size_t i = 0;
    for ( ; src[ i ] && i < buf_size - 1; ++i )
        buf[ i ] = ( char )toupper( ( unsigned char )src[ i ] );
    buf[ i ] = '\0';
}

/* Copy src into buf replacing every backslash with a forward slash. */
static void
create_str_fwd( const char* src, char* buf, size_t buf_size )
{
    size_t i = 0;
    for ( ; src[ i ] && i < buf_size - 1; ++i )
        buf[ i ] = ( src[ i ] == '\\' ) ? '/' : src[ i ];
    buf[ i ] = '\0';
}

/* ---- File helpers ---- */

static bool
create_file_exists( const char* path )
{
    FILE* f = fopen( path, "r" );
    if ( f ) { fclose( f ); return true; }
    return false;
}

/* Opens `path` for writing. Returns NULL if it already exists or cannot be created. */
static FILE*
create_open_write( const char* path )
{
    if ( create_file_exists( path ) )
    {
        printf( ORB_INDENT "[orb warn]  already exists, skipping: %s\n", path );
        return NULL;
    }
    FILE* f = fopen( path, "w" );
    if ( !f )
        printf( ORB_INDENT "[orb error] cannot create: %s\n", path );
    return f;
}

/* ---- File emitters ---- */

/* <name>.h -- pure types, no vtable, no function declarations. */
static void
create_emit_h( const char* path, const char* name, const char* NAME, const char* inc_dir )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "#ifndef %s_H\n", NAME );
    fprintf( f, "#define %s_H\n", NAME );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s/%s.h -- %s module types.\n", inc_dir, name, name );
    fprintf( f, "    Include in DLL modules that use %s through the vtable (%s()->...).\n", name, name );
    fprintf( f, "    Include %s_host.h instead for direct-call access (host, sandbox).\n", name );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include \"orb.h\"\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Types\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "/* TODO: add %s-specific types here */\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/*============================================================================================*/\n" );
    fprintf( f, "#endif    // %s_H\n", NAME );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <name>_api.h -- API struct and dual-mode gateway macros. */
static void
create_emit_api_h( const char* path, const char* name, const char* NAME,
                   const char* inc_dir, bool is_static )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    const char* link_note = is_static
        ? "always statically linked into the host"
        : "hot-reloadable DLL; BUILD_STATIC switches to static gateway";

    fprintf( f, "#ifndef %s_API_H\n", NAME );
    fprintf( f, "#define %s_API_H\n", NAME );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s/%s_api.h -- %s module API struct and gateway macro.\n", inc_dir, name, name );
    fprintf( f, "    %s.\n", link_note );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include \"%s/%s.h\"\n", inc_dir, name );
    fprintf( f, "#include \"engine/mod/mod_import.h\"\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    API Struct\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "typedef struct %s_api_s\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    void ( *tick )( float dt );    /* TODO: replace with real API functions */\n" );
    fprintf( f, "\n" );
    fprintf( f, "} %s_api_t;\n", name );
    fprintf( f, "\n" );
    fprintf( f, "#if defined( BUILD_STATIC ) || defined( %s_STATIC )\n", NAME );
    fprintf( f, "MOD_GATEWAY_STATIC( %s_api_t, %s )\n", name, name );
    fprintf( f, "#else\n" );
    fprintf( f, "MOD_GATEWAY_DYNAMIC( %s_api_t, %s )\n", name, name );
    fprintf( f, "#endif\n" );
    fprintf( f, "\n" );
    fprintf( f, "#if defined( BUILD_STATIC ) || defined( %s_STATIC )\n", NAME );
    fprintf( f, "    #define MOD_USE_%s    /* static build */\n", NAME );
    fprintf( f, "    #define MOD_FETCH_%s  true\n", NAME );
    fprintf( f, "#else\n" );
    fprintf( f, "    #define MOD_USE_%s    MOD_DEFINE_API_PTR( %s_api_t, %s )\n", NAME, name, name );
    fprintf( f, "    #define MOD_FETCH_%s  MOD_FETCH_API( %s_api_t, %s )\n", NAME, name, name );
    fprintf( f, "#endif\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*============================================================================================*/\n" );
    fprintf( f, "#endif    // %s_API_H\n", NAME );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <name>_host.h -- direct-call function declarations (static modules only). */
static void
create_emit_host_h( const char* path, const char* name, const char* NAME, const char* inc_dir )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "#ifndef %s_HOST_H\n", NAME );
    fprintf( f, "#define %s_HOST_H\n", NAME );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s/%s_host.h -- Host-only %s services.\n", inc_dir, name, name );
    fprintf( f, "    Includes %s_api.h.\n", name );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include \"%s/%s_api.h\"\n", inc_dir, name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Direct-call functions (host and sandbox use only)\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "void        %s_tick( float dt );    /* TODO: replace with real direct-call functions */\n",
             name );
    fprintf( f, "mod_desc_t* %s_get_mod_desc( void );\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/*============================================================================================*/\n" );
    fprintf( f, "#endif    // %s_HOST_H\n", NAME );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <name>.c -- unity build entry for a STATIC module. */
static void
create_emit_c_static( const char* path, const char* name, const char* inc_dir )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s.c -- Unity build entry for the %s module.\n", name, name );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include <stdio.h>\n" );
    fprintf( f, "#include \"orb.h\"\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include \"engine/mod/mod_export.h\"\n" );
    fprintf( f, "#include \"%s/%s_host.h\"\n", inc_dir, name );
    fprintf( f, "#include \"%s/%s_api.h\"\n", inc_dir, name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Unity build\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "/* Platform-specific implementation files go here:\n" );
    fprintf( f, "   #include \"win/win_%s.c\" */\n", name );
    fprintf( f, "\n" );
    fprintf( f, "#include \"%s/%s_api.c\"\n", inc_dir, name );
    fprintf( f, "\n" );
    fprintf( f, "/*============================================================================================*/\n" );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <name>_api.c -- API struct wiring and mod_desc_t lifecycle (static modules only). */
static void
create_emit_api_c( const char* path, const char* name )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s_api.c -- %s module wiring.\n", name, name );
    fprintf( f, "    Implements the %s_api_t vtable struct and the mod_desc_t lifecycle descriptor.\n", name );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Persistent state (allocated by the module system; preserved across hot-reloads)\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "typedef struct %s_state_s\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    int32_t placeholder;    /* replace with real state fields */\n" );
    fprintf( f, "\n" );
    fprintf( f, "} %s_state_t;\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/* static %s_state_t* s = NULL; */\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Implementation\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "static void\n" );
    fprintf( f, "%s_tick_impl( float dt )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    ( void )dt;    /* TODO */\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    API Struct\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "const %s_api_t g_%s_api_struct = {\n", name, name );
    fprintf( f, "    .tick = %s_tick_impl,\n", name );
    fprintf( f, "};\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Direct-call wrappers (declared in %s_host.h)\n", name );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "void\n" );
    fprintf( f, "%s_tick( float dt )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    %s_tick_impl( dt );\n", name );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Lifecycle\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "static bool\n" );
    fprintf( f, "%s_mod_init( void* raw_state, get_api_fn get_api )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    UNUSED( get_api );\n" );
    fprintf( f, "    UNUSED( raw_state );\n" );
    fprintf( f, "    /* s = ( %s_state_t* )raw_state; */\n", name );
    fprintf( f, "    return true;\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "static void\n" );
    fprintf( f, "%s_mod_exit( void* raw_state )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    UNUSED( raw_state );\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Module descriptor\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "mod_desc_t*\n" );
    fprintf( f, "%s_get_mod_desc( void )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    static mod_desc_t desc = {\n" );
    fprintf( f, "        .version       = 1,\n" );
    fprintf( f, "        .state_size    = sizeof( %s_state_t ),\n", name );
    fprintf( f, "        .func_api_size = sizeof( %s_api_t ),\n", name );
    fprintf( f, "        .func_api      = &g_%s_api_struct,\n", name );
    fprintf( f, "        .dep_count     = 0,\n" );
    fprintf( f, "        .init          = %s_mod_init,\n", name );
    fprintf( f, "        .exit          = %s_mod_exit,\n", name );
    fprintf( f, "        .reload        = NULL,\n" );
    fprintf( f, "    };\n" );
    fprintf( f, "    return &desc;\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*============================================================================================*/\n" );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <name>.c -- full implementation for a DYNAMIC (hot-reload DLL) module. */
static void
create_emit_c_dynamic( const char* path, const char* name, const char* inc_dir )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s.c -- %s module (hot-reloadable DLL).\n", name, name );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include <stdio.h>\n" );
    fprintf( f, "#include \"orb.h\"\n" );
    fprintf( f, "#define LOG_CH \"%s\"\n", name );
    fprintf( f, "\n" );
    fprintf( f, "#include \"engine/mod/mod_export.h\"\n" );
    fprintf( f, "#include \"engine/core/core_api.h\"\n" );
    fprintf( f, "#include \"%s/%s_api.h\"\n", inc_dir, name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Cached API pointers\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "MOD_USE_CORE;\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Persistent state (allocated by the module system; preserved across hot-reloads)\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "typedef struct %s_state_s\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    int32_t placeholder;    /* replace with real state fields */\n" );
    fprintf( f, "\n" );
    fprintf( f, "} %s_state_t;\n", name );
    fprintf( f, "\n" );
    fprintf( f, "static %s_state_t* g_state = NULL;\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    API implementations\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "static void\n" );
    fprintf( f, "%s_tick_impl( float dt )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    if ( !g_state ) return;\n" );
    fprintf( f, "    ( void )dt;    /* TODO */\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    API struct\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "const %s_api_t g_%s_api_struct = {\n", name, name );
    fprintf( f, "    .tick = %s_tick_impl,\n", name );
    fprintf( f, "};\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Lifecycle\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "static bool\n" );
    fprintf( f, "%s_init( void* raw_state, get_api_fn get_api )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    UNUSED( get_api );\n" );
    fprintf( f, "    g_state = ( %s_state_t* )raw_state;\n", name );
    fprintf( f, "    if ( !MOD_FETCH_CORE ) return false;\n" );
    fprintf( f, "    return true;\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "static bool\n" );
    fprintf( f, "%s_reload( void* raw_state, get_api_fn get_api )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    UNUSED( get_api );\n" );
    fprintf( f, "    g_state = ( %s_state_t* )raw_state;\n", name );
    fprintf( f, "    if ( !MOD_FETCH_CORE ) return false;\n" );
    fprintf( f, "    LOG_INFO( \"reloaded\" );\n" );
    fprintf( f, "    return true;\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "static void\n" );
    fprintf( f, "%s_exit( void* raw_state )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    UNUSED( raw_state );\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Module descriptor\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "mod_desc_t*\n" );
    fprintf( f, "%s_get_mod_desc( void )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    static mod_desc_t desc = {\n" );
    fprintf( f, "        .version       = 1,\n" );
    fprintf( f, "        .state_size    = sizeof( %s_state_t ),\n", name );
    fprintf( f, "        .func_api_size = sizeof( %s_api_t ),\n", name );
    fprintf( f, "        .func_api      = &g_%s_api_struct,\n", name );
    fprintf( f, "        .deps          = { \"core\" },\n" );
    fprintf( f, "        .dep_count     = 1,\n" );
    fprintf( f, "        .init          = %s_init,\n", name );
    fprintf( f, "        .exit          = %s_exit,\n", name );
    fprintf( f, "        .reload        = %s_reload,\n", name );
    fprintf( f, "    };\n" );
    fprintf( f, "    return &desc;\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "MOD_DEFINE_EXPORTS( %s )\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/*============================================================================================*/\n" );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* ---- Entry point ---- */

static bool
cmd_create_module( const char* name, const char* dir, bool is_dynamic )
{
    char dir_fwd[ PATH_MAX ];
    create_str_fwd( dir, dir_fwd, sizeof( dir_fwd ) );

    /* Strip leading "source/" to get the include-root-relative path used inside #include. */
    const char* inc_dir = dir_fwd;
    if ( strncmp( dir_fwd, "source/", 7 ) == 0 )
        inc_dir = dir_fwd + 7;

    char NAME[ 128 ];
    create_str_upper( name, NAME, sizeof( NAME ) );

    /* Ensure the target directory exists before writing any files. */
    ensure_dir( dir );

    /* Build one file path per artifact using the OS separator for fopen. */
    char p_h     [ PATH_MAX ];
    char p_api_h [ PATH_MAX ];
    char p_host_h[ PATH_MAX ];
    char p_c     [ PATH_MAX ];
    char p_api_c [ PATH_MAX ];

    snprintf( p_h,      sizeof( p_h ),      "%s%s%s.h",      dir, PATH_SEP, name );
    snprintf( p_api_h,  sizeof( p_api_h ),  "%s%s%s_api.h",  dir, PATH_SEP, name );
    snprintf( p_host_h, sizeof( p_host_h ), "%s%s%s_host.h", dir, PATH_SEP, name );
    snprintf( p_c,      sizeof( p_c ),      "%s%s%s.c",      dir, PATH_SEP, name );
    snprintf( p_api_c,  sizeof( p_api_c ),  "%s%s%s_api.c",  dir, PATH_SEP, name );

    const char* type_label = is_dynamic ? "dynamic" : "static";
    printf( ORB_BANNER "[orb create]  %s  (%s)  in %s\n\n", name, type_label, dir );

    create_emit_h    ( p_h,     name, NAME, inc_dir );
    create_emit_api_h( p_api_h, name, NAME, inc_dir, !is_dynamic );

    if ( is_dynamic )
    {
        create_emit_c_dynamic( p_c, name, inc_dir );
    }
    else
    {
        create_emit_host_h  ( p_host_h, name, NAME, inc_dir );
        create_emit_c_static( p_c,      name,       inc_dir );
        create_emit_api_c   ( p_api_c,  name );
    }

    /* Print the orb.targets stanza for copy-paste. */
    printf( "\n" );
    printf( ORB_BANNER "Add to orb.targets:\n" );
    printf( "\n" );
    printf( "    target %s\n", name );
    printf( "        type        %s\n", type_label );
    printf( "        root        %s\n", dir_fwd );
    printf( "        folder      TODO_FOLDER\n" );
    printf( "        unit        %s.c\n", name );
    if ( is_dynamic )
        printf( "        dep         core\n" );
    printf( "\n" );

    return true;
}

/*============================================================================================*/
