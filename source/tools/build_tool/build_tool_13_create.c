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

    Every emitted file is a k_tpl_* string literal expanded by create_expand(): the template
    reads as the file it produces, so it can be compared by eye against a real module.

    Project scaffolding creates a complete standalone game project that builds on this
    engine (run from the engine root; -dir defaults to <name>).  The project builds a
    GAME MODULE DLL (runtime/run_project.h contract) that the engine's hosts load and run:
    host_game.exe -project <dir> / host_editor.exe -project <dir>.
        <dir>/orb.targets      -- 'engine' declaration + game DLL target + solution
        <dir>/src/             -- a full copy of source/project/sample_game/ with the
                                  identifier 'sample_game' renamed to <name>: <name>.c|.h
                                  plus game_ui.c|.h, the game kit they build on. The
                                  template is a compiled engine target, so it cannot rot
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

/* Opens `path` for writing. Returns NULL if it already exists or cannot be created. */
static FILE*
create_open_write( const char* path )
{
    if ( platform_file_exists( path ) )
    {
        printf( ORB_INDENT "[orb warn]  already exists, skipping: %s\n", path );
        return NULL;
    }
    FILE* f = fopen( path, "w" );
    if ( !f )
        printf( ORB_INDENT "[orb error] cannot create: %s\n", path );
    return f;
}

/* Closes an emitted file and reports it in the create log. */
static void
create_close( FILE* f, const char* path )
{
    fclose( f );
    printf( ORB_INDENT "  wrote  %s\n", path );
}

/* ---- Template expansion ---- */

/* One $key -> value binding for create_expand(). */
typedef struct create_sub_s
{
    const char* key;    // the name after '$', without the sigil
    const char* val;    // text written in its place

} create_sub_t;

/* Pass a create_sub_t array and its length to a create_write_tpl* call. */
#define CREATE_SUBS( a )    ( a ), ( int )( sizeof( a ) / sizeof( ( a )[ 0 ] ) )

/* Writes tpl to f, replacing each "$key" from subs with its value.

   Keys are matched longest-first, so one key being a prefix of another resolves to the
   longer one. Text after a match is copied verbatim, which is what lets "$NAME_H" expand
   to "<NAME>_H" without a delimiter. "$$" emits a literal '$'.

   An unmatched "$<letter>" is copied through and warned about: a mistyped key would
   otherwise land silently in the scaffolded file. */
static void
create_expand( FILE* f, const char* tpl, const create_sub_t* subs, int sub_count )
{
    for ( const char* p = tpl; *p; )
    {
        if ( *p != '$' )     { fputc( *p,  f ); ++p;     continue; }
        if ( p[ 1 ] == '$' ) { fputc( '$', f ); p += 2; continue; }

        int    best     = -1;
        size_t best_len = 0;
        for ( int i = 0; i < sub_count; ++i )
        {
            size_t len = strlen( subs[ i ].key );
            if ( len <= best_len ) continue;
            if ( strncmp( p + 1, subs[ i ].key, len ) == 0 ) { best = i; best_len = len; }
        }

        if ( best < 0 )
        {
            char c = p[ 1 ];
            if ( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) )
                printf( ORB_INDENT "[orb warn]  unknown template key near: %.24s\n", p );
            fputc( *p, f );
            ++p;
            continue;
        }

        fputs( subs[ best ].val, f );
        p += 1 + best_len;
    }
}

/* Expands tpl into a new file at path. Skips the write if the file already exists. */
static void
create_write_tpl( const char* path, const char* tpl, const create_sub_t* subs, int sub_count )
{
    FILE* f = create_open_write( path );
    if ( !f ) return;
    create_expand( f, tpl, subs, sub_count );
    create_close( f, path );
}

/* Same, but overwrites an existing file. Used for the machine-local files that must be
   refreshed on every run so re-creating an existing project repairs a moved engine root. */
static void
create_write_tpl_force( const char* path, const char* tpl, const create_sub_t* subs, int sub_count )
{
    FILE* f = fopen( path, "w" );
    if ( !f )
    {
        printf( ORB_INDENT "[orb error] cannot create: %s\n", path );
        return;
    }
    create_expand( f, tpl, subs, sub_count );
    create_close( f, path );
}

/*==============================================================================================
    Module templates

    Substitutions:
        $name   module identifier            $NAME   the same, upper-cased
        $inc    include-root-relative dir     $hint   <name>.h "which header to include" line
        $link   <name>_api.h link-mode note
==============================================================================================*/

/* <name>.h -- pure types, no vtable, no function declarations. */
static const char k_tpl_h[] =
"#ifndef $NAME_H\n"
"#define $NAME_H\n"
"/*==============================================================================================\n"
"\n"
"    $inc/$name.h -- $name module types.\n"
"    Include in DLL modules that use $name through the vtable ($name()->...).\n"
"$hint\n"
"\n"
"==============================================================================================*/\n"
"\n"
"#include \"orb.h\"\n"
"\n"
"/*==============================================================================================\n"
"    Types\n"
"==============================================================================================*/\n"
"\n"
"/* TODO: add $name-specific types here */\n"
"\n"
"/*============================================================================================*/\n"
"#endif    // $NAME_H\n";

/* <name>_api.h -- API struct and dual-mode gateway macros. */
static const char k_tpl_api_h[] =
"#ifndef $NAME_API_H\n"
"#define $NAME_API_H\n"
"/*==============================================================================================\n"
"\n"
"    $inc/$name_api.h -- $name module API struct and gateway macro.\n"
"    $link.\n"
"\n"
"==============================================================================================*/\n"
"\n"
"#include \"$inc/$name.h\"\n"
"#include \"engine/mod/mod_import.h\"\n"
"\n"
"/*==============================================================================================\n"
"    API Struct\n"
"==============================================================================================*/\n"
"\n"
"typedef struct $name_api_s\n"
"{\n"
"    void ( *tick )( float dt );    /* TODO: replace with real API functions */\n"
"\n"
"} $name_api_t;\n"
"\n"
"/*============================================================================================*/\n"
"\n"
"#if defined( BUILD_STATIC ) || defined( $NAME_STATIC )\n"
"    MOD_GATEWAY_STATIC( $name_api_t, $name )\n"
"    #define MOD_USE_$NAME    /* static build */\n"
"    #define MOD_FETCH_$NAME  true\n"
"#else\n"
"    MOD_GATEWAY_DYNAMIC( $name_api_t, $name )\n"
"    #define MOD_USE_$NAME    MOD_DEFINE_API_PTR( $name_api_t, $name )\n"
"    #define MOD_FETCH_$NAME  MOD_FETCH_API( $name_api_t, $name )\n"
"#endif\n"
"\n"
"/*============================================================================================*/\n"
"#endif    // $NAME_API_H\n";

/* <name>_host.h -- direct-call function declarations (static modules only). */
static const char k_tpl_host_h[] =
"#ifndef $NAME_HOST_H\n"
"#define $NAME_HOST_H\n"
"/*==============================================================================================\n"
"\n"
"    $inc/$name_host.h -- Host-only $name services.\n"
"    Includes $name_api.h.\n"
"\n"
"==============================================================================================*/\n"
"\n"
"#include \"$inc/$name_api.h\"\n"
"\n"
"/*==============================================================================================\n"
"    Module Descriptor\n"
"\n"
"    Used by the host to register the $name module:\n"
"        mod_static_load( \"$name\", $name_get_mod_desc() );\n"
"    or via the build-mode-transparent macro:\n"
"        mod_load( $name );\n"
"\n"
"==============================================================================================*/\n"
"\n"
"mod_desc_t* $name_get_mod_desc( void );\n"
"\n"
"/*==============================================================================================\n"
"    Direct-call functions (host and sandbox use only)\n"
"==============================================================================================*/\n"
"\n"
"void $name_tick( float dt );    /* TODO: replace with real direct-call functions */\n"
"\n"
"/*============================================================================================*/\n"
"#endif    // $NAME_HOST_H\n";

/* <name>.c -- unity build entry for a STATIC module. */
static const char k_tpl_c_static[] =
"/*==============================================================================================\n"
"\n"
"    $name.c -- Unity build entry for the $name module.\n"
"\n"
"==============================================================================================*/\n"
"\n"
"#include \"orb.h\"\n"
"\n"
"#include \"engine/mod/mod_export.h\"\n"
"#include \"$inc/$name_host.h\"\n"
"\n"
"/*==============================================================================================\n"
"    Platform units\n"
"==============================================================================================*/\n"
"\n"
"/* Platform-specific implementation files go here:\n"
"   #include \"win/win_$name.c\" */\n"
"\n"
"/*==============================================================================================\n"
"    Unity build\n"
"==============================================================================================*/\n"
"\n"
"/* Implementation files go here:\n"
"   #include \"$inc/$name_function.c\" */\n"
"\n"
"/*==============================================================================================\n"
"    Public API wiring  (must be last -- all implementations must be in scope)\n"
"==============================================================================================*/\n"
"\n"
"#ifndef $NAME_API_C_PRELUDE\n"
"#include \"$inc/$name_api.c\"\n"
"#endif\n"
"\n"
"/*============================================================================================*/\n";

/* <name>_api.c -- API struct wiring and mod_desc_t lifecycle (static modules only). */
static const char k_tpl_api_c_static[] =
"/*==============================================================================================\n"
"\n"
"    $name_api.c -- $name module wiring.\n"
"    Implements the $name_api_t vtable struct and the mod_desc_t lifecycle descriptor.\n"
"\n"
"==============================================================================================*/\n"
"\n"
"/*==============================================================================================\n"
"    Implementation\n"
"==============================================================================================*/\n"
"\n"
"static void\n"
"$name_tick_impl( float dt )\n"
"{\n"
"    ( void )dt;    /* TODO */\n"
"}\n"
"\n"
"/*==============================================================================================\n"
"    API Struct\n"
"==============================================================================================*/\n"
"\n"
"const $name_api_t g_$name_api_struct = {\n"
"    .tick = $name_tick_impl,\n"
"};\n"
"\n"
"/*==============================================================================================\n"
"    Direct-call wrappers (declared in $name_host.h)\n"
"==============================================================================================*/\n"
"\n"
"void\n"
"$name_tick( float dt )\n"
"{\n"
"    $name_tick_impl( dt );\n"
"}\n"
"\n"
"/*==============================================================================================\n"
"    Lifecycle\n"
"==============================================================================================*/\n"
"\n"
"static bool\n"
"$name_mod_init( void* raw_state, get_api_fn get_api )\n"
"{\n"
"    UNUSED( get_api );\n"
"    UNUSED( raw_state );\n"
"    return true;\n"
"}\n"
"\n"
"static void\n"
"$name_mod_exit( void* raw_state )\n"
"{\n"
"    UNUSED( raw_state );\n"
"}\n"
"\n"
"/*==============================================================================================\n"
"    Module descriptor\n"
"==============================================================================================*/\n"
"\n"
"mod_desc_t*\n"
"$name_get_mod_desc( void )\n"
"{\n"
"    static mod_desc_t desc = {\n"
"        .version       = 1,\n"
"        .state_size    = 0,\n"
"        .func_api_size = sizeof( $name_api_t ),\n"
"        .func_api      = &g_$name_api_struct,\n"
"        .dep_count     = 0,\n"
"        .init          = $name_mod_init,\n"
"        .exit          = $name_mod_exit,\n"
"        .reload        = NULL,\n"
"    };\n"
"    return &desc;\n"
"}\n"
"\n"
"/*============================================================================================*/\n";

/* <name>.c -- unity build entry for a DYNAMIC (hot-reload DLL) module. */
static const char k_tpl_c_dynamic[] =
"/*==============================================================================================\n"
"\n"
"    $name.c -- Unity build entry for the $name module.\n"
"\n"
"==============================================================================================*/\n"
"\n"
"#include \"orb.h\"\n"
"\n"
"#include \"engine/mod/mod_export.h\"\n"
"#include \"$inc/$name_api.h\"\n"
"\n"
"/*==============================================================================================\n"
"    Unity build\n"
"==============================================================================================*/\n"
"\n"
"/* Implementation files go here:\n"
"   #include \"$inc/$name_function.c\" */\n"
"\n"
"/*==============================================================================================\n"
"    Public API wiring  (must be last -- all implementations must be in scope)\n"
"==============================================================================================*/\n"
"\n"
"#ifndef $NAME_API_C_PRELUDE\n"
"#include \"$inc/$name_api.c\"\n"
"#endif\n"
"\n"
"/*============================================================================================*/\n";

/* <name>_api.c -- state, API struct, lifecycle, and DLL export (dynamic modules only). */
static const char k_tpl_api_c_dynamic[] =
"/*==============================================================================================\n"
"\n"
"    $name_api.c -- $name module wiring.\n"
"    Implements the $name_api_t vtable struct and the mod_desc_t lifecycle descriptor.\n"
"\n"
"==============================================================================================*/\n"
"\n"
"/*==============================================================================================\n"
"    Cached API pointers\n"
"\n"
"    Declare one per consumed module using its MOD_USE_<NAME> macro (defined in its _api.h),\n"
"    then fetch in init() and reload() with MOD_FETCH_<NAME>:\n"
"\n"
"        MOD_USE_CORE;                                    // file scope\n"
"        if ( !MOD_FETCH_CORE ) return false;             // in init() and reload()\n"
"==============================================================================================*/\n"
"\n"
"/*==============================================================================================\n"
"    Persistent state (allocated by the module system; preserved across hot-reloads)\n"
"==============================================================================================*/\n"
"\n"
"typedef struct $name_state_s\n"
"{\n"
"    int32_t placeholder;    /* replace with real state fields */\n"
"\n"
"} $name_state_t;\n"
"\n"
"static $name_state_t* g_state = NULL;\n"
"\n"
"/*==============================================================================================\n"
"    Implementation\n"
"==============================================================================================*/\n"
"\n"
"static void\n"
"$name_tick_impl( float dt )\n"
"{\n"
"    if ( !g_state ) return;\n"
"    ( void )dt;    /* TODO */\n"
"}\n"
"\n"
"/*==============================================================================================\n"
"    API Struct\n"
"==============================================================================================*/\n"
"\n"
"const $name_api_t g_$name_api_struct = {\n"
"    .tick = $name_tick_impl,\n"
"};\n"
"\n"
"/*==============================================================================================\n"
"    Lifecycle\n"
"==============================================================================================*/\n"
"\n"
"static bool\n"
"$name_init( void* raw_state, get_api_fn get_api )\n"
"{\n"
"    UNUSED( get_api );    /* remove when fetching module APIs */\n"
"    g_state = ( $name_state_t* )raw_state;\n"
"    return true;\n"
"}\n"
"\n"
"static bool\n"
"$name_reload( void* raw_state, get_api_fn get_api )\n"
"{\n"
"    UNUSED( get_api );    /* remove when fetching module APIs */\n"
"    g_state = ( $name_state_t* )raw_state;\n"
"    return true;\n"
"}\n"
"\n"
"static void\n"
"$name_exit( void* raw_state )\n"
"{\n"
"    UNUSED( raw_state );\n"
"}\n"
"\n"
"/*==============================================================================================\n"
"    Module descriptor\n"
"==============================================================================================*/\n"
"\n"
"mod_desc_t*\n"
"$name_get_mod_desc( void )\n"
"{\n"
"    static mod_desc_t desc = {\n"
"        .version       = 1,\n"
"        .state_size    = sizeof( $name_state_t ),\n"
"        .func_api_size = sizeof( $name_api_t ),\n"
"        .func_api      = &g_$name_api_struct,\n"
"        .dep_count     = 0,\n"
"        .init          = $name_init,\n"
"        .exit          = $name_exit,\n"
"        .reload        = $name_reload,\n"
"    };\n"
"    return &desc;\n"
"}\n"
"\n"
"MOD_DEFINE_EXPORTS( $name )\n"
"\n"
"/*============================================================================================*/\n";

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

    /* The two lines that differ between a static and a dynamic module. */
    char hint[ 160 ];
    snprintf( hint, sizeof( hint ),
              is_dynamic ? "    Include %s_api.h in the module's own .c files."
                         : "    Include %s_host.h instead for direct-call access (host, sandbox).",
              name );

    const char* link = is_dynamic
        ? "hot-reloadable DLL; BUILD_STATIC switches to static gateway"
        : "always statically linked into the host";

    const create_sub_t subs[] = {
        { "name", name    },
        { "NAME", NAME    },
        { "inc",  inc_dir },
        { "hint", hint    },
        { "link", link    },
    };

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

    create_write_tpl( p_h,     k_tpl_h,     CREATE_SUBS( subs ) );
    create_write_tpl( p_api_h, k_tpl_api_h, CREATE_SUBS( subs ) );

    if ( is_dynamic )
    {
        create_write_tpl( p_c,     k_tpl_c_dynamic,     CREATE_SUBS( subs ) );
        create_write_tpl( p_api_c, k_tpl_api_c_dynamic, CREATE_SUBS( subs ) );
    }
    else
    {
        create_write_tpl( p_host_h, k_tpl_host_h,       CREATE_SUBS( subs ) );
        create_write_tpl( p_c,      k_tpl_c_static,     CREATE_SUBS( subs ) );
        create_write_tpl( p_api_c,  k_tpl_api_c_static, CREATE_SUBS( subs ) );
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

/*==============================================================================================
    Project templates

    Substitutions:
        $name        project identifier        $NAME         the same, upper-cased
        $engine_ref  orb.targets engine path   $engine_abs   absolute engine root
==============================================================================================*/

/* <dir>/orb.targets -- engine declaration, game DLL target, and solution.

   The project builds a game module DLL, not an exe: the engine's hosts run it --
   host_game.exe -project <dir> (play) or host_editor.exe -project <dir> (Play/Stop in
   the editor).  The DLL implements runtime/run_project.h and hot-reloads while a host is
   running.  -monolithic is not supported for project DLLs (a mono build produces no
   engine DLLs for the project to pair with). */
static const char k_tpl_project_targets[] =
"# orb.targets -- $name\n"
"#\n"
"# 'engine' declares the ORB installation this project builds on:\n"
"#   - All engine targets are available as deps and build on demand into this\n"
"#     project's bin/ (they are excluded from local build-all/gen/clean).\n"
"#   - Engine headers (engine/sys/sys.h etc.) are on the include path automatically.\n"
"#   - Built-in tools (build_tool, reflect_tool) resolve from the engine root.\n"
"#\n"
"# This project builds $name.dll -- a game module (runtime/run_project.h contract)\n"
"# run by the engine's hosts.  -monolithic is not supported for project DLLs.\n"
"#\n"
"# Workflow:\n"
"#   bin\\build_tool.bat -gen      generate VS project files (build/proj)\n"
"#   bin\\build_tool.bat           build (add -config Release as needed)\n"
"#   \"$engine_abs\\bin\\host_game.exe\"   -project .     play it\n"
"#   \"$engine_abs\\bin\\host_editor.exe\" -project .     edit it (Play/Stop)\n"
"#\n"
"# In Visual Studio, F5 does the same via the 'run' lines below: the startup\n"
"# project launches host_editor; set '$name_play' as startup to launch host_game.\n"
"\n"
"engine  $engine_ref\n"
"\n"
"target $name\n"
"\n"
"    type        dynamic\n"
"    root        src\n"
"    folder      01_$NAME\n"
"    unit        $name.c\n"
"    unit        game_ui.c   # the game kit: HUD over gui's element tier\n"
"    run         host_editor -project .\n"
"\n"
"# F5 launcher: builds $name.dll, runs it standalone under host_game.\n"
"target $name_play\n"
"\n"
"    alias       $name\n"
"    folder      01_$NAME\n"
"    run         host_game -project .\n"
"\n"
"solution $name\n"
"\n"
"    out         build/proj\n"
"    startup     $name\n"
"    add         $name $name_play\n"
"\n"
"    # Engine targets included for source navigation and debugging.\n"
"    add         base sys ref mod app core job\n"
"    add         run rhi draw render game\n";

/* <dir>/clean_build.bat -- wipe generated outputs and restore the forwarder.
   Reads .orb_engine at runtime so it keeps working if the project moves.  The doubled
   '%%' is batch's own escape, reaching bin\build_tool.bat as a single '%'. */
static const char k_tpl_project_clean_bat[] =
"@echo off\n"
"if not exist .orb_engine (\n"
"    echo [orb error] .orb_engine not found. Re-run bootstrap_project.bat to restore.\n"
"    exit /b 1\n"
")\n"
"set /p ENGINE_ROOT=<.orb_engine\n"
"if exist build rmdir /s /q build\n"
"if exist bin   rmdir /s /q bin\n"
"if not exist bin mkdir bin\n"
"(echo @\"%ENGINE_ROOT%\\bin\\build_tool.exe\" %%*) > bin\\build_tool.bat\n"
"echo [orb] clean complete. bin\\build_tool.bat restored.\n";

/* <dir>/.gitignore -- generated outputs only; scaffolded sources stay tracked. */
static const char k_tpl_project_gitignore[] =
"bin/\n"
"build/\n"
".cache/\n"
".vscode/\n"
".clangd\n"
".orb_engine\n"
"compile_commands.json\n"
"*.code-workspace\n";

/* <dir>/.orb_engine -- absolute engine root, one line. Machine-local. */
static const char k_tpl_project_engine_file[] =
"$engine_abs\n";

/* <dir>/bin/build_tool.bat -- forwarder to the engine's build_tool.exe. Machine-local. */
static const char k_tpl_project_forwarder[] =
"@\"$engine_abs" PATH_SEP "bin" PATH_SEP "build_tool.exe\" %*\n";

/*  The identifier every file under source/project/sample_game/ is written in terms of.
    It is substituted in both file contents and emitted file names. */

#define CREATE_TOKEN     "sample_game"
#define CREATE_TOKEN_LEN ( sizeof( CREATE_TOKEN ) - 1 )

/*  Every file copied into a new project, in emit order. The emitted name is the template
    name with CREATE_TOKEN substituted, so sample_game.c becomes <name>.c while game_ui.c
    keeps its name. sample_game is the benchmark minimal-but-real game: a new project is a
    full copy of it, not a subset, so the two stay at parity until sample_game is
    deliberately forked. Adding a file here also needs a 'unit' line for it in
    k_tpl_project_targets if it compiles as its own translation unit. */

static const char* k_project_template_files[] = {
    CREATE_TOKEN ".c",
    CREATE_TOKEN ".h",
    "game_ui.c",
    "game_ui.h",
};

/* <dir>/src/<file> -- copied from the canonical project module
   (source/project/sample_game/) with the identifier 'sample_game' renamed to the project
   name. The template is a real engine target built on every full engine build, so the
   emitted code can never drift from the current API.

   Two rewrites are applied while streaming, longest pattern first:

     "project/sample_game/"  ->  ""      the engine tree reaches the game kit header
                                         through an include root; a scaffolded project
                                         keeps every source file flat in <dir>/src, so
                                         the same include becomes a sibling one.
     "sample_game"           ->  <name>  the module identifier and file names.

   Order matters: the path pattern contains the identifier and must be tested first. */
static void
create_emit_project_module( const char* path, const char* name, const char* template_file )
{
    const struct { const char* from; const char* to; } subs[] = {
        { "project/" CREATE_TOKEN "/", ""   },
        { CREATE_TOKEN,                name },
    };
    const int sub_count = ( int )( sizeof( subs ) / sizeof( subs[ 0 ] ) );

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

    /* Stream the template through, applying the first rewrite that matches at each
       position. CRs are dropped so a CRLF checkout still emits clean text (the "w"
       stream re-adds them on Windows). */
    const char* p   = mf.data;
    const char* end = mf.data + mf.size;
    while ( p < end )
    {
        if ( *p == '\r' ) { ++p; continue; }

        bool matched = false;
        for ( int i = 0; i < sub_count && !matched; ++i )
        {
            size_t len = strlen( subs[ i ].from );
            if ( ( size_t )( end - p ) < len || memcmp( p, subs[ i ].from, len ) != 0 )
                continue;
            fputs( subs[ i ].to, f );
            p += len;
            matched = true;
        }
        if ( matched )
            continue;

        fputc( *p, f );
        ++p;
    }

    platform_unmap_file( &mf );
    create_close( f, path );
}

/* Register the project in <engine>/build/.orb_projects -- the machine-local, gitignored
   engine->projects index (the reverse direction of a project's .orb_engine).  One absolute
   path per line; the launcher lists, opens, and prunes it.  Append-if-missing; best effort,
   registration failure never fails the create. */

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
                if ( path_eq( line, project_abs ) )
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
    if ( !platform_file_exists( "orb.targets" ) ||
         !platform_file_exists( "source" PATH_SEP "engine" PATH_SEP "mod" PATH_SEP "mod.c" ) )
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

    const create_sub_t subs[] = {
        { "name",       name       },
        { "NAME",       NAME       },
        { "engine_ref", engine_ref },
        { "engine_abs", engine_abs },
    };

    printf( ORB_BANNER "[orb create]  %s  (project)  in %s\n", name, dir );
    printf( ORB_INDENT "  engine  %s\n\n", engine_abs );

    char sub[ PATH_MAX ];
    snprintf( sub, sizeof( sub ), "%s%ssrc", dir, PATH_SEP );
    ensure_dir( sub );
    snprintf( sub, sizeof( sub ), "%s%sbin", dir, PATH_SEP );
    ensure_dir( sub );

    char path[ PATH_MAX ];

    snprintf( path, sizeof( path ), "%s%sorb.targets", dir, PATH_SEP );
    create_write_tpl( path, k_tpl_project_targets, CREATE_SUBS( subs ) );

    for ( int i = 0; i < ( int )( sizeof( k_project_template_files ) /
                                  sizeof( k_project_template_files[ 0 ] ) ); ++i )
    {
        const char* tf = k_project_template_files[ i ];

        // Name-carrying files (sample_game.c|.h) are renamed; the rest keep their names.
        char out_name[ 128 ];
        if ( strncmp( tf, CREATE_TOKEN, CREATE_TOKEN_LEN ) == 0 )
            snprintf( out_name, sizeof( out_name ), "%s%s", name, tf + CREATE_TOKEN_LEN );
        else
            snprintf( out_name, sizeof( out_name ), "%s", tf );

        snprintf( path, sizeof( path ), "%s%ssrc%s%s", dir, PATH_SEP, PATH_SEP, out_name );
        create_emit_project_module( path, name, tf );
    }

    snprintf( path, sizeof( path ), "%s%sclean_build.bat", dir, PATH_SEP );
    create_write_tpl( path, k_tpl_project_clean_bat, CREATE_SUBS( subs ) );

    snprintf( path, sizeof( path ), "%s%s.gitignore", dir, PATH_SEP );
    create_write_tpl( path, k_tpl_project_gitignore, CREATE_SUBS( subs ) );

    /* Machine-local files: always refresh so re-running repairs a moved engine. */

    snprintf( path, sizeof( path ), "%s%s.orb_engine", dir, PATH_SEP );
    create_write_tpl_force( path, k_tpl_project_engine_file, CREATE_SUBS( subs ) );

    snprintf( path, sizeof( path ), "%s%sbin%sbuild_tool.bat", dir, PATH_SEP, PATH_SEP );
    create_write_tpl_force( path, k_tpl_project_forwarder, CREATE_SUBS( subs ) );

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
