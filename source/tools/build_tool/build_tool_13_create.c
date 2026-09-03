/*==============================================================================================

    build_tool_13_create.c -- -create command: scaffold a new engine module or child project.

    Usage:
        build_tool.exe -create <name> -dir <source/path> [-type static|dynamic]
        build_tool.exe -create <name> -type project [-dir <path>]

    Module scaffolding emits the standard header and source file set, then prints the
    orb.targets stanza to stdout for copy-paste registration.

    Static (engine-style, like sys/core/app):
        <name>.h  <name>_api.h  <name>_host.h  <name>.c  <name>_api.c

    Dynamic (hot-reload DLL, like render/audio):
        <name>.h  <name>_api.h  <name>.c  <name>_api.c

    Project scaffolding creates a complete standalone game project that builds on this
    engine (run from the engine root; -dir defaults to <name>).  The project builds a
    GAME MODULE DLL (runtime/run_project.h contract) that the engine's hosts load and run:
    host_game.exe -project <dir> / host_editor.exe -project <dir>.
        <dir>/orb.targets      -- 'engine' declaration + game DLL target + solution
        <dir>/src/<name>.c|.h  -- copy of source/project/sample_game/sample_game.c|.h
                                  with the identifier 'sample_game' renamed to <name>;
                                  the template is a compiled engine target, so it cannot rot
        <dir>/.orb_engine      -- absolute engine root (read by clean_build.bat)
        <dir>/bin/build_tool.bat  -- forwarder to the engine's build_tool.exe
        <dir>/clean_build.bat  -- wipe bin/ + build/ and restore the forwarder
        <dir>/.gitignore       -- generated outputs

    --------------------------------------

    Ex: build_tool.exe -create net -dir source/engine/net -type static
    Ex: build_tool.exe -create gui -dir source/runtime_modules/gui -type dynamic
    Ex: build_tool.exe -create my_game -type project

==============================================================================================*/
// clang-format off

/* ---- String helpers ---- */

/* C-identifier check shared by module and project creation: the name becomes file
   names, include guards, function prefixes, and a target name. */
static bool
create_valid_name( const char* name )
{
    if ( !name || !name[ 0 ] ) return false;
    if ( name[ 0 ] >= '0' && name[ 0 ] <= '9' ) return false;
    for ( const char* p = name; *p; ++p )
    {
        bool ok = ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) ||
                  ( *p >= '0' && *p <= '9' ) || ( *p == '_' );
        if ( !ok ) return false;
    }
    return true;
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
create_emit_h( const char* path, const char* name, const char* NAME, const char* inc_dir, bool is_static )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "#ifndef %s_H\n", NAME );
    fprintf( f, "#define %s_H\n", NAME );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s/%s.h -- %s module types.\n", inc_dir, name, name );
    fprintf( f, "    Include in DLL modules that use %s through the vtable (%s()->...).\n", name, name );
    if ( is_static )
        fprintf( f, "    Include %s_host.h instead for direct-call access (host, sandbox).\n", name );
    else
        fprintf( f, "    Include %s_api.h in the module's own .c files.\n", name );
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
    fprintf( f, "/*============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#if defined( BUILD_STATIC ) || defined( %s_STATIC )\n", NAME );
    fprintf( f, "    MOD_GATEWAY_STATIC( %s_api_t, %s )\n", name, name );
    fprintf( f, "    #define MOD_USE_%s    /* static build */\n", NAME );
    fprintf( f, "    #define MOD_FETCH_%s  true\n", NAME );
    fprintf( f, "#else\n" );
    fprintf( f, "    MOD_GATEWAY_DYNAMIC( %s_api_t, %s )\n", name, name );
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
    fprintf( f, "    Module Descriptor\n" );
    fprintf( f, "\n" );
    fprintf( f, "    Used by the host to register the %s module:\n", name );
    fprintf( f, "        mod_static_load( \"%s\", %s_get_mod_desc() );\n", name, name );
    fprintf( f, "    or via the build-mode-transparent macro:\n" );
    fprintf( f, "        mod_load( %s );\n", name );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "mod_desc_t* %s_get_mod_desc( void );\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Direct-call functions (host and sandbox use only)\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "void %s_tick( float dt );    /* TODO: replace with real direct-call functions */\n", name );
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

    char NAME[ 128 ];
    str_upper( name, NAME, sizeof( NAME ) );

    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s.c -- Unity build entry for the %s module.\n", name, name );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include \"orb.h\"\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include \"engine/mod/mod_export.h\"\n" );
    fprintf( f, "#include \"%s/%s_host.h\"\n", inc_dir, name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Platform units\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "/* Platform-specific implementation files go here:\n" );
    fprintf( f, "   #include \"win/win_%s.c\" */\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Unity build\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "/* Implementation files go here:\n" );
    fprintf( f, "   #include \"%s/%s_function.c\" */\n", inc_dir, name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Public API wiring  (must be last -- all implementations must be in scope)\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#ifndef %s_API_C_PRELUDE\n", NAME );
    fprintf( f, "#include \"%s/%s_api.c\"\n", inc_dir, name );
    fprintf( f, "#endif\n" );
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
    fprintf( f, "        .state_size    = 0,\n" );
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

/* <name>.c -- unity build entry for a DYNAMIC (hot-reload DLL) module. */
static void
create_emit_c_dynamic( const char* path, const char* name, const char* inc_dir )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    char NAME[ 128 ];
    str_upper( name, NAME, sizeof( NAME ) );

    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "\n" );
    fprintf( f, "    %s.c -- Unity build entry for the %s module.\n", name, name );
    fprintf( f, "\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include \"orb.h\"\n" );
    fprintf( f, "\n" );
    fprintf( f, "#include \"engine/mod/mod_export.h\"\n" );
    fprintf( f, "#include \"%s/%s_api.h\"\n", inc_dir, name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Unity build\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "/* Implementation files go here:\n" );
    fprintf( f, "   #include \"%s/%s_function.c\" */\n", inc_dir, name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Public API wiring  (must be last -- all implementations must be in scope)\n" );
    fprintf( f, "==============================================================================================*/\n" );
    fprintf( f, "\n" );
    fprintf( f, "#ifndef %s_API_C_PRELUDE\n", NAME );
    fprintf( f, "#include \"%s/%s_api.c\"\n", inc_dir, name );
    fprintf( f, "#endif\n" );
    fprintf( f, "\n" );
    fprintf( f, "/*============================================================================================*/\n" );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <name>_api.c -- state, API struct, lifecycle, and DLL export (dynamic modules only). */
static void
create_emit_api_c_dynamic( const char* path, const char* name )
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
    fprintf( f, "    Cached API pointers\n" );
    fprintf( f, "\n" );
    fprintf( f, "    Declare one per consumed module using its MOD_USE_<NAME> macro (defined in its _api.h),\n" );
    fprintf( f, "    then fetch in init() and reload() with MOD_FETCH_<NAME>:\n" );
    fprintf( f, "\n" );
    fprintf( f, "        MOD_USE_CORE;                                    // file scope\n" );
    fprintf( f, "        if ( !MOD_FETCH_CORE ) return false;             // in init() and reload()\n" );
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
    fprintf( f, "static %s_state_t* g_state = NULL;\n", name );
    fprintf( f, "\n" );
    fprintf( f, "/*==============================================================================================\n" );
    fprintf( f, "    Implementation\n" );
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
    fprintf( f, "    API Struct\n" );
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
    fprintf( f, "    UNUSED( get_api );    /* remove when fetching module APIs */\n" );
    fprintf( f, "    g_state = ( %s_state_t* )raw_state;\n", name );
    fprintf( f, "    return true;\n" );
    fprintf( f, "}\n" );
    fprintf( f, "\n" );
    fprintf( f, "static bool\n" );
    fprintf( f, "%s_reload( void* raw_state, get_api_fn get_api )\n", name );
    fprintf( f, "{\n" );
    fprintf( f, "    UNUSED( get_api );    /* remove when fetching module APIs */\n" );
    fprintf( f, "    g_state = ( %s_state_t* )raw_state;\n", name );
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
    fprintf( f, "        .dep_count     = 0,\n" );
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
    snprintf( dir_fwd, sizeof( dir_fwd ), "%s", dir );
    path_to_fwd( dir_fwd );

    /* Strip leading "source/" to get the include-root-relative path used inside #include. */
    const char* inc_dir = dir_fwd;
    if ( strncmp( dir_fwd, "source/", 7 ) == 0 )
        inc_dir = dir_fwd + 7;

    char NAME[ 128 ];
    str_upper( name, NAME, sizeof( NAME ) );

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

    create_emit_h    ( p_h,     name, NAME, inc_dir, !is_dynamic );
    create_emit_api_h( p_api_h, name, NAME, inc_dir, !is_dynamic );

    if ( is_dynamic )
    {
        create_emit_c_dynamic    ( p_c,     name, inc_dir );
        create_emit_api_c_dynamic( p_api_c, name );
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
    printf( "\n" );

    return true;
}

/*==============================================================================================
    --- Project scaffolding (-create <name> -type project) ---

    Generates a complete standalone game project that builds on this engine. Must run
    from the engine root: the generated files embed both a relative 'engine' path
    (inside orb.targets) and the absolute engine root (.orb_engine, forwarder bat).
==============================================================================================*/

/* Build the 'engine' path for the generated orb.targets. A simple relative -dir
   ("my_game", "projects/my_game") becomes the matching "../" chain so the project
   stays relocatable together with the engine tree. Anything else falls back to the
   absolute engine root. */
static void
create_project_engine_ref( const char* dir_fwd, const char* engine_abs, char* out, size_t size )
{
    if ( !platform_is_abs_path( dir_fwd ) && !strstr( dir_fwd, ".." ) )
    {
        size_t used = 0;
        out[ 0 ] = '\0';
        for ( const char* p = dir_fwd; *p && used + 4 < size; ++p )
        {
            /* One "../" per component; skip empty components from doubled/trailing slashes. */
            if ( p == dir_fwd || ( p[ -1 ] == '/' && *p != '/' ) )
                used += (size_t)snprintf( out + used, size - used, "../" );
        }
        if ( used > 0 ) return;
    }
    snprintf( out, size, "%s", engine_abs );
}

/* <dir>/orb.targets -- engine declaration, game DLL target, and solution.

   The project builds a game module DLL, not an exe: the engine's hosts run it --
   host_game.exe -project <dir> (play) or host_editor.exe -project <dir> (Play/Stop in
   the editor).  The DLL implements runtime/run_project.h and hot-reloads while a host is
   running.  -monolithic is not supported for project DLLs (a mono build produces no
   engine DLLs for the project to pair with). */
static void
create_emit_project_targets( const char* path, const char* name, const char* NAME,
                             const char* engine_ref, const char* engine_abs )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "# orb.targets -- %s\n", name );
    fprintf( f, "#\n" );
    fprintf( f, "# 'engine' declares the ORB installation this project builds on:\n" );
    fprintf( f, "#   - All engine targets are available as deps and build on demand into this\n" );
    fprintf( f, "#     project's bin/ (they are excluded from local build-all/gen/clean).\n" );
    fprintf( f, "#   - Engine headers (engine/sys/sys.h etc.) are on the include path automatically.\n" );
    fprintf( f, "#   - Built-in tools (build_tool, reflect_tool) resolve from the engine root.\n" );
    fprintf( f, "#\n" );
    fprintf( f, "# This project builds %s.dll -- a game module (runtime/run_project.h contract)\n", name );
    fprintf( f, "# run by the engine's hosts.  -monolithic is not supported for project DLLs.\n" );
    fprintf( f, "#\n" );
    fprintf( f, "# Workflow:\n" );
    fprintf( f, "#   bin\\build_tool.bat -gen      generate VS project files (build/proj)\n" );
    fprintf( f, "#   bin\\build_tool.bat           build (add -config Release as needed)\n" );
    fprintf( f, "#   \"%s\\bin\\host_game.exe\"   -project .     play it\n", engine_abs );
    fprintf( f, "#   \"%s\\bin\\host_editor.exe\" -project .     edit it (Play/Stop)\n", engine_abs );
    fprintf( f, "#\n" );
    fprintf( f, "# In Visual Studio, F5 does the same via the 'run' lines below: the startup\n" );
    fprintf( f, "# project launches host_editor; set '%s_play' as startup to launch host_game.\n", name );
    fprintf( f, "\n" );
    fprintf( f, "engine  %s\n", engine_ref );
    fprintf( f, "\n" );
    fprintf( f, "target %s\n", name );
    fprintf( f, "\n" );
    fprintf( f, "    type        dynamic\n" );
    fprintf( f, "    root        src\n" );
    fprintf( f, "    folder      01_%s\n", NAME );
    fprintf( f, "    unit        %s.c\n", name );
    fprintf( f, "    run         host_editor -project .\n" );
    fprintf( f, "\n" );
    fprintf( f, "# F5 launcher: builds %s.dll, runs it standalone under host_game.\n", name );
    fprintf( f, "target %s_play\n", name );
    fprintf( f, "\n" );
    fprintf( f, "    alias       %s\n", name );
    fprintf( f, "    folder      01_%s\n", NAME );
    fprintf( f, "    run         host_game -project .\n" );
    fprintf( f, "\n" );
    fprintf( f, "solution %s\n", name );
    fprintf( f, "\n" );
    fprintf( f, "    out         build/proj\n" );
    fprintf( f, "    startup     %s\n", name );
    fprintf( f, "    add         %s %s_play\n", name, name );
    fprintf( f, "\n" );
    fprintf( f, "    # Engine targets included for source navigation and debugging.\n" );
    fprintf( f, "    add         base sys ref mod app core job\n" );
    fprintf( f, "    add         run rhi draw render game\n" );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <dir>/src/<name>.c|.h -- copied from the canonical project module
   (source/project/sample_game/sample_game.c|.h) with the identifier 'sample_game'
   renamed to the project name. The template is a real engine target built on every
   full engine build, so the emitted code can never drift from the current API. */
static void
create_emit_project_module( const char* path, const char* name, const char* template_file )
{
    static const char k_token[] = "sample_game";
    const size_t      token_len = sizeof( k_token ) - 1;

    char template_path[ PATH_MAX ];
    snprintf( template_path, sizeof( template_path ),
              "source" PATH_SEP "project" PATH_SEP "sample_game" PATH_SEP "%s", template_file );

    platform_mapped_file_t mf;
    if ( !platform_map_file( template_path, &mf ) || !mf.data )
    {
        printf( ORB_INDENT "[orb error] project template not found: %s\n", template_path );
        return;
    }

    FILE* f = create_open_write( path );
    if ( !f ) { platform_unmap_file( &mf ); return; }

    /* Stream the template through, renaming every token hit. CRs are dropped so a
       CRLF checkout still emits clean text (the "w" stream re-adds them on Windows). */
    const char* p   = mf.data;
    const char* end = mf.data + mf.size;
    while ( p < end )
    {
        if ( *p == '\r' ) { ++p; continue; }
        if ( ( size_t )( end - p ) >= token_len && memcmp( p, k_token, token_len ) == 0 )
        {
            fputs( name, f );
            p += token_len;
            continue;
        }
        fputc( *p, f );
        ++p;
    }

    platform_unmap_file( &mf );
    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <dir>/clean_build.bat -- wipe generated outputs and restore the forwarder.
   Reads .orb_engine at runtime so it keeps working if the project moves. */
static void
create_emit_project_clean_bat( const char* path )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "@echo off\n" );
    fprintf( f, "if not exist .orb_engine (\n" );
    fprintf( f, "    echo [orb error] .orb_engine not found. Re-run bootstrap_project.bat to restore.\n" );
    fprintf( f, "    exit /b 1\n" );
    fprintf( f, ")\n" );
    fprintf( f, "set /p ENGINE_ROOT=<.orb_engine\n" );
    fprintf( f, "if exist build rmdir /s /q build\n" );
    fprintf( f, "if exist bin   rmdir /s /q bin\n" );
    fprintf( f, "if not exist bin mkdir bin\n" );
    fprintf( f, "(echo @\"%%ENGINE_ROOT%%\\bin\\build_tool.exe\" %%%%*) > bin\\build_tool.bat\n" );
    fprintf( f, "echo [orb] clean complete. bin\\build_tool.bat restored.\n" );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* <dir>/.gitignore -- generated outputs only; scaffolded sources stay tracked. */
static void
create_emit_project_gitignore( const char* path )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;

    fprintf( f, "bin/\n" );
    fprintf( f, "build/\n" );
    fprintf( f, ".cache/\n" );
    fprintf( f, ".vscode/\n" );
    fprintf( f, ".clangd\n" );
    fprintf( f, ".orb_engine\n" );
    fprintf( f, "compile_commands.json\n" );
    fprintf( f, "*.code-workspace\n" );

    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* Register the project in <engine>/build/.orb_projects -- the machine-local, gitignored
   engine->projects index (the reverse direction of a project's .orb_engine).  One absolute
   path per line; the launcher lists, opens, and prunes it.  Append-if-missing; best effort,
   registration failure never fails the create. */

/* Path equality with Windows semantics: case-insensitive, slash-kind-insensitive. */
static bool
create_registry_path_eq( const char* a, const char* b )
{
    for ( ;; ++a, ++b )
    {
        char ca = ( *a == '\\' ) ? '/' : ( *a >= 'A' && *a <= 'Z' ) ? ( char )( *a + 32 ) : *a;
        char cb = ( *b == '\\' ) ? '/' : ( *b >= 'A' && *b <= 'Z' ) ? ( char )( *b + 32 ) : *b;
        if ( ca != cb )   return false;
        if ( ca == '\0' ) return true;
    }
}

static void
create_register_project( const char* project_abs )
{
    static const char k_reg[] = "build" PATH_SEP ".orb_projects";
    ensure_dir( "build" );

    /* Scan the existing registry for this path (one platform_map_file pass; missing file = empty). */
    platform_mapped_file_t mf;
    if ( platform_map_file( k_reg, &mf ) && mf.data )
    {
        const char* p   = mf.data;
        const char* end = mf.data + mf.size;
        while ( p < end )
        {
            const char* nl  = memchr( p, '\n', ( size_t )( end - p ) );
            size_t      len = nl ? ( size_t )( nl - p ) : ( size_t )( end - p );
            while ( len > 0 && ( p[ len - 1 ] == '\r' || p[ len - 1 ] == ' ' ) ) --len;

            char line[ PATH_MAX ];
            if ( len > 0 && len < sizeof( line ) )
            {
                memcpy( line, p, len );
                line[ len ] = '\0';
                if ( create_registry_path_eq( line, project_abs ) )
                {
                    platform_unmap_file( &mf );
                    return;    /* already registered */
                }
            }
            p = nl ? nl + 1 : end;
        }
        platform_unmap_file( &mf );
    }

    FILE* f = fopen( k_reg, "a" );
    if ( !f )
    {
        printf( ORB_INDENT "[orb warn]  cannot update project registry: %s\n", k_reg );
        return;
    }
    fprintf( f, "%s\n", project_abs );
    fclose( f );
    printf( ORB_INDENT "  registered in %s\n", k_reg );
}

/* ---- Project entry point ---- */

static bool
cmd_create_project( const char* name, const char* dir )
{
    /* Must run from the engine root: the generated project embeds paths to it. */
    if ( !create_file_exists( "orb.targets" ) ||
         !create_file_exists( "source" PATH_SEP "engine" PATH_SEP "mod" PATH_SEP "mod.c" ) )
    {
        printf( ORB_INDENT "[orb error] -type project must run from the engine root"
                           " (orb.targets + source/engine not found here)\n" );
        return false;
    }

    char engine_abs[ PATH_MAX ];
    if ( !platform_fullpath( engine_abs, ".", sizeof( engine_abs ) ) )
    {
        printf( ORB_INDENT "[orb error] cannot resolve the engine root path\n" );
        return false;
    }

    char dir_fwd[ PATH_MAX ];
    snprintf( dir_fwd, sizeof( dir_fwd ), "%s", dir );
    path_to_fwd( dir_fwd );

    char NAME[ 128 ];
    str_upper( name, NAME, sizeof( NAME ) );

    char engine_ref[ PATH_MAX ];
    create_project_engine_ref( dir_fwd, engine_abs, engine_ref, sizeof( engine_ref ) );

    printf( ORB_BANNER "[orb create]  %s  (project)  in %s\n", name, dir );
    printf( ORB_INDENT "  engine  %s\n\n", engine_abs );

    char sub[ PATH_MAX ];
    snprintf( sub, sizeof( sub ), "%s%ssrc", dir, PATH_SEP );
    ensure_dir( sub );
    snprintf( sub, sizeof( sub ), "%s%sbin", dir, PATH_SEP );
    ensure_dir( sub );

    char path[ PATH_MAX ];

    snprintf( path, sizeof( path ), "%s%sorb.targets", dir, PATH_SEP );
    create_emit_project_targets( path, name, NAME, engine_ref, engine_abs );

    snprintf( path, sizeof( path ), "%s%ssrc%s%s.c", dir, PATH_SEP, PATH_SEP, name );
    create_emit_project_module( path, name, "sample_game.c" );

    snprintf( path, sizeof( path ), "%s%ssrc%s%s.h", dir, PATH_SEP, PATH_SEP, name );
    create_emit_project_module( path, name, "sample_game.h" );

    snprintf( path, sizeof( path ), "%s%sclean_build.bat", dir, PATH_SEP );
    create_emit_project_clean_bat( path );

    snprintf( path, sizeof( path ), "%s%s.gitignore", dir, PATH_SEP );
    create_emit_project_gitignore( path );

    /* Machine-local files: always refresh so re-running repairs a moved engine. */

    snprintf( path, sizeof( path ), "%s%s.orb_engine", dir, PATH_SEP );
    {
        FILE* f = fopen( path, "w" );
        if ( f ) { fprintf( f, "%s\n", engine_abs ); fclose( f ); printf( ORB_INDENT "  wrote  %s\n", path ); }
        else       printf( ORB_INDENT "[orb error] cannot create: %s\n", path );
    }

    snprintf( path, sizeof( path ), "%s%sbin%sbuild_tool.bat", dir, PATH_SEP, PATH_SEP );
    {
        FILE* f = fopen( path, "w" );
        if ( f )
        {
            fprintf( f, "@\"%s%sbin%sbuild_tool.exe\" %%*\n", engine_abs, PATH_SEP, PATH_SEP );
            fclose( f );
            printf( ORB_INDENT "  wrote  %s\n", path );
        }
        else printf( ORB_INDENT "[orb error] cannot create: %s\n", path );
    }

    /* Register in the engine's machine-local project index (re-running repairs it too). */
    {
        char project_abs[ PATH_MAX ];
        if ( platform_fullpath( project_abs, dir, sizeof( project_abs ) ) )
            create_register_project( project_abs );
    }

    printf( "\n" );
    printf( ORB_BANNER "Project ready. Next steps (from %s):\n", dir );
    printf( "\n" );
    printf( "    bin\\build_tool.bat -gen      generate VS project files\n" );
    printf( "    bin\\build_tool.bat           build %s.dll\n", name );
    printf( "\n" );
    printf( "    \"%s\\bin\\host_game.exe\"   -project .     play it\n", engine_abs );
    printf( "    \"%s\\bin\\host_editor.exe\" -project .     edit it (Play/Stop)\n", engine_abs );
    printf( "\n" );
    printf( "    Or open build\\proj\\%s_nm.sln and press F5: '%s' debugs in the editor,\n", name, name );
    printf( "    '%s_play' (set as startup) debugs standalone under host_game.\n", name );
    printf( "\n" );

    return true;
}

/*============================================================================================*/
