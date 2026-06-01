/*==============================================================================================

    reflect_tool.c - Reflection code generation tool (unity build root)

    Usage:
        reflect_tool -src <source_dir> [-out <output_dir>] [-name <module_name>] [-silent]
        reflect_tool <source_dir> [<output_dir> [<module_name>]] [-silent]   (legacy positional)

    Scans <source_dir> for REF_STRUCT / REF_ENUM / REF_BITSET annotated headers and
    writes <output_dir>/<module_name>.generated.h/c with REF_ registration stubs.

    Defaults:
        -out   build/generated           (relative to CWD)
        -name  last path component of -src

    This tool is called as a pre-compile step for every module that uses reflection.
    It is designed for minimal startup cost: no allocator init, no config files,
    no dynamic libraries. Straight args-in, files-out.

    This file is the unity build root. All translation units are included here.

    NOTE: This tool is standalone C11. It does NOT include orb.h or any engine headers.

==============================================================================================*/

#include "orb.h"
#include "reflect_tool_internal.h"

#include "reflect_tool_std.c"
#include "reflect_tool_platform.c"
#include "reflect_tool_scan.c"
#include "reflect_tool_lex.c"
#include "reflect_tool_attr.c"
#include "reflect_tool_parse.c"
#include "reflect_tool_output.c"

/*----------------------------------------------------------------------------------------------
    Debug overrides - used when running reflect_tool directly from the IDE with no args.
    Set RT_DEBUG_MODULE and RT_DEBUG_SOURCE_SUB to the module you want to step through.
    Paths are resolved relative to the executable directory (e.g. build/bin/reflect_tool.exe).
----------------------------------------------------------------------------------------------*/

#define RT_DEBUG_MODULE     "core"
#define RT_DEBUG_SOURCE_SUB "source/engine/core"

/*----------------------------------------------------------------------------------------------
    Helpers
----------------------------------------------------------------------------------------------*/

/* True when argv[i] has a following value that is not itself a flag. */
static bool
arg_has_value( int argc, char** argv, int i )
{
    return i + 1 < argc && argv[ i + 1 ][ 0 ] != '-';
}

/* Return a pointer to the last path component of s (after the final / or \). */
static const char*
path_basename( const char* s )
{
    const char* p = s;
    for ( const char* c = s; *c; ++c )
        if ( *c == '/' || *c == '\\' )
            p = c + 1;
    return p;
}

/*----------------------------------------------------------------------------------------------
    Entry point
----------------------------------------------------------------------------------------------*/

int
main( int argc, char** argv )
{
    const char* source_dir  = NULL;
    const char* output_dir  = NULL;
    const char* module_name = NULL;
    bool        silent      = false;
    bool        show_help   = false;

    /* Storage for debug and inferred paths -- lives for the duration of main. */
    char dbg_src [ RT_MAX_PATH ];
    char dbg_out [ RT_MAX_PATH ];
    char inf_name[ RT_MAX_NAME ];

    /* --- Arg parsing: named flags take priority; positional fallback follows --- */

    /* First pass: scan for named flags. */
    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-src"    ) == 0 && arg_has_value( argc, argv, i ) ) source_dir  = argv[ ++i ];
        if ( strcmp( argv[ i ], "-out"    ) == 0 && arg_has_value( argc, argv, i ) ) output_dir  = argv[ ++i ];
        if ( strcmp( argv[ i ], "-name"   ) == 0 && arg_has_value( argc, argv, i ) ) module_name = argv[ ++i ];
        if ( strcmp( argv[ i ], "-silent" ) == 0 ) silent    = true;
        if ( strcmp( argv[ i ], "-help"   ) == 0 ) show_help = true;
        if ( strcmp( argv[ i ], "-h"      ) == 0 ) show_help = true;
    }

    if ( show_help )
    {
        printf( "usage: reflect_tool -src <source_dir> [-out <output_dir>] [-name <module_name>] [-silent]\n" );
        printf( "  -src    directory to scan for REF_STRUCT / REF_ENUM / REF_BITSET annotations\n" );
        printf( "  -out    output directory for generated files (default: build/generated)\n" );
        printf( "  -name   base name for generated files (default: last component of -src)\n" );
        printf( "  -silent suppress summary output\n" );
        return 0;
    }

    /* Second pass: positional fallback when no named flags were used.
       Positional order: <source_dir> [<output_dir> [<module_name>]].
       Tokens starting with '-' are flags; skip them and their value (if any). */

    // if ( !source_dir )
    // {
    //     int pos = 0;
    //     for ( int i = 1; i < argc; ++i )
    //     {
    //         if ( argv[ i ][ 0 ] == '-' )
    //         {
    //             if ( arg_has_value( argc, argv, i ) ) ++i; /* skip value-taking flag's value */
    //             continue;
    //         }
    //         if      ( pos == 0 ) { source_dir  = argv[ i ]; pos++; }
    //         else if ( pos == 1 ) { output_dir  = argv[ i ]; pos++; }
    //         else if ( pos == 2 ) { module_name = argv[ i ]; pos++; }
    //     }
    // }

    /* --- Defaults and validation --- */

    if ( !source_dir )
    {
        if ( RELEASE )
        {
            fprintf( stderr, "reflect_tool: -src is required\n" );
            fprintf( stderr, "usage: reflect_tool -src <source_dir> [-out <output_dir>] [-name <module_name>] [-silent]\n" );
            return 1;
        }
        else
        {
            /* Derive paths from exe location so debug from IDE F5 works from any CWD.
               exe lives at <root>/bin/  ->  source at <root>/<source_sub>, out at <root>/build/generated */

            char exe_dir[ RT_MAX_PATH ];
            platform_exe_dir( exe_dir, RT_MAX_PATH );

            str_copy( dbg_src, exe_dir, RT_MAX_PATH );
            str_cat( dbg_src, "/../" RT_DEBUG_SOURCE_SUB, RT_MAX_PATH );

            str_copy( dbg_out, exe_dir, RT_MAX_PATH );
            str_cat( dbg_out, "/../build/generated", RT_MAX_PATH );

            source_dir  = dbg_src;
            output_dir  = dbg_out;
            module_name = RT_DEBUG_MODULE;

            printf( "[reflect_tool] no args -- debug: %s\n  src: %s\n  out: %s\n",
                    module_name, source_dir, output_dir );
        }
    }

    if ( !output_dir  ) output_dir  = "build/generated";
    if ( !module_name )
    {
        /* Infer from the last path component of source_dir. */
        str_copy( inf_name, path_basename( source_dir ), RT_MAX_NAME );
        module_name = inf_name;
    }

    platform_mkdir( output_dir );

    /* Parse data is large (worst-case ~12MB); keep it out of the stack. */

    static file_list_t  files;
    static parse_data_t data;
    memset( &files, 0, sizeof files );
    memset( &data, 0, sizeof data );

    /* Scan the source directory for files */

    scan( source_dir, &files );

    /* Parse each file for REF_STRUCT / REF_ENUM / REF_BITSET declarations and build the AST. */

    parse( &files, &data );

    /* Generate the .h/.c files with REF_ registration code. */

    if ( output( output_dir, module_name, &data ) == false )
    {
        return 1;
    }
    
    /* Show results and exit. Suppressed when invoked with -silent (build_tool omits it
       unless ORB_OUT_REFLECT is set). */

    if ( !silent )
    {
        if ( data.module_api.has_module )
            printf( "[reflect_tool] %s: %d struct(s), %d enum(s), %d func sig(s), module API: %d fn(s)\n",
                    module_name, data.struct_count, data.enum_count, data.func_count,
                    data.module_api.func_count );
        else
            printf( "[reflect_tool] %s: %d struct(s), %d enum(s), %d func sig(s)\n",
                    module_name, data.struct_count, data.enum_count, data.func_count );
    }

    return 0; /* Success */
}

/*--------------------------------------------------------------------------------------------*/