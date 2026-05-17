/*==============================================================================================

    rs_gen.c - Reflection code generation tool (unity build root)

    Usage:
        build_reflect <source_dir> <output_dir> <module_name>

    Scans <source_dir> for RS_STRUCT / RS_ENUM / RS_BITSET annotated headers and
    writes <output_dir>/<module_name>.generated.h/c with rs_ registration stubs.

    This tool is called as a pre-compile step for every module that uses reflection.
    It is designed for minimal startup cost: no allocator init, no config files,
    no dynamic libraries. Straight args-in, files-out.

    This file is the unity build root. All translation units are included here.

    NOTE: This tool is standalone C11. It does NOT include orb.h or any engine headers.

==============================================================================================*/
#include "orb.h"

#include "rs_gen_internal.h"

#include "rs_gen_std.c"
#include "rs_gen_platform.c"
#include "rs_gen_scan.c"
#include "rs_gen_lex.c"
#include "rs_gen_attr.c"
#include "rs_gen_parse.c"
#include "rs_gen_output.c"

/*----------------------------------------------------------------------------------------------
    Debug overrides - used when running build_reflect directly from the IDE with no args.
    Set RG_DEBUG_MODULE and RG_DEBUG_SOURCE_SUB to the module you want to step through.
    Paths are resolved relative to the executable directory (e.g. build/bin/build_reflect.exe).
----------------------------------------------------------------------------------------------*/

#define RG_DEBUG_MODULE     "engine_core"
#define RG_DEBUG_SOURCE_SUB "source/engine/core"


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
    char dbg_src[ RG_MAX_PATH ];
    char dbg_out[ RG_MAX_PATH ];

    int release = RELEASE;

    if ( argc < 4 )
    {
        if ( release )
        {
            fprintf( stderr, "usage: build_reflect <source_dir> <output_dir> <module_name>\n" );
            return 1;
        }
        else
        {
            // Derive paths from the exe location so this works regardless of working directory.
            // exe lives at  <root>/build/bin/  so:
            //   source  ->  <exedir>/../../<source_sub>
            //   output  ->  <exedir>/../generated
            char exe_dir[ RG_MAX_PATH ];
            rg_platform_exe_dir( exe_dir, RG_MAX_PATH );

            rg_str_copy( dbg_src, exe_dir, RG_MAX_PATH );
            rg_str_cat( dbg_src, "/../../" RG_DEBUG_SOURCE_SUB, RG_MAX_PATH );

            rg_str_copy( dbg_out, exe_dir, RG_MAX_PATH );
            rg_str_cat( dbg_out, "/../generated", RG_MAX_PATH );

            source_dir  = dbg_src;
            output_dir  = dbg_out;
            module_name = RG_DEBUG_MODULE;

            printf( "[build_reflect] no args -- debug: %s\n  src: %s\n  out: %s\n", module_name, source_dir,
                    output_dir );
        }
    }
    else
    {
        source_dir  = argv[ 1 ];
        output_dir  = argv[ 2 ];
        module_name = argv[ 3 ];
    }

    rg_platform_mkdir( output_dir );

    /* Parse data is large (worst-case ~12MB); keep it out of the stack. */
    static rg_file_list_t  files;
    static rg_parse_data_t data;
    memset( &files, 0, sizeof files );
    memset( &data, 0, sizeof data );

    rg_scan( source_dir, &files );
    rg_parse( &files, &data );

    if ( !rg_output( output_dir, module_name, &data ) )
        return 1;

    printf( "[build_reflect] %s: %d struct(s), %d enum(s)\n", module_name, data.struct_count, data.enum_count );
    return 0;
}

/*--------------------------------------------------------------------------------------------*/