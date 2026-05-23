/*==============================================================================================

    reflect_tool.c - Reflection code generation tool (unity build root)

    Usage:
        reflect_tool <source_dir> <output_dir> <module_name>

    Scans <source_dir> for RS_STRUCT / RS_ENUM / RS_BITSET annotated headers and
    writes <output_dir>/<module_name>.generated.h/c with rs_ registration stubs.

    This tool is called as a pre-compile step for every module that uses reflection.
    It is designed for minimal startup cost: no allocator init, no config files,
    no dynamic libraries. Straight args-in, files-out.

    This file is the unity build root. All translation units are included here.

    NOTE: This tool is standalone C11. It does NOT include orb.h or any engine headers.

    Example: F:\orb\source\engine\core F:\orb\build\generated core

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
    Entry point
----------------------------------------------------------------------------------------------*/

int
main( int argc, char** argv )
{
    const char* source_dir;
    const char* output_dir;
    const char* module_name;

    // Storage for debug paths - lives for the duration of main
    char dbg_src[ RT_MAX_PATH ];
    char dbg_out[ RT_MAX_PATH ];

    if ( argc < 4 )
    {
        if ( RELEASE )
        {
            fprintf( stderr, "usage: reflect_tool <source_dir> <output_dir> <module_name>\n" );
            return 1;
        }
        else
        {
            // Derive paths from the exe location so this works regardless of working directory.
            // exe lives at  <root>/build/bin/  so:
            //   source  ->  <exedir>/../../<source_sub>
            //   output  ->  <exedir>/../generated
            char exe_dir[ RT_MAX_PATH ];
            platform_exe_dir( exe_dir, RT_MAX_PATH );

            str_copy( dbg_src, exe_dir, RT_MAX_PATH );
            str_cat( dbg_src, "/../" RT_DEBUG_SOURCE_SUB, RT_MAX_PATH );

            str_copy( dbg_out, exe_dir, RT_MAX_PATH );
            str_cat( dbg_out, "/../build/generated", RT_MAX_PATH );

            source_dir  = dbg_src;
            output_dir  = dbg_out;
            module_name = RT_DEBUG_MODULE;

            printf( "[reflect_tool] no args -- debug: %s\n  src: %s\n  out: %s\n", module_name, source_dir,
                    output_dir );
        }
    }
    else
    {
        source_dir  = argv[ 1 ];
        output_dir  = argv[ 2 ];
        module_name = argv[ 3 ];
    }

    platform_mkdir( output_dir );

    /* Parse data is large (worst-case ~12MB); keep it out of the stack. */

    static file_list_t  files;
    static parse_data_t data;
    memset( &files, 0, sizeof files );
    memset( &data, 0, sizeof data );

    /* Scan the source directory for files */

    scan( source_dir, &files );

    /* Parse each file for RS_STRUCT / RS_ENUM / RS_BITSET declarations and build the AST. */

    parse( &files, &data );

    /* Generate the .h/.c files with rs_ registration code. */

    if ( output( output_dir, module_name, &data ) == false )
    {
        return 1;
    }
    
    /* Show results and exit. In release, this is the only output on success. */

    if ( data.module_api.has_module )
        printf( "[reflect_tool] %s: %d struct(s), %d enum(s), module API: %d fn(s)\n",
                module_name, data.struct_count, data.enum_count, data.module_api.func_count );
    else
        printf( "[reflect_tool] %s: %d struct(s), %d enum(s)\n",
                module_name, data.struct_count, data.enum_count );

    return 0; /* Success */
}

/*--------------------------------------------------------------------------------------------*/