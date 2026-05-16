/*==============================================================================================

    rs_gen_scan.c - collect .h files from a source directory

==============================================================================================*/

#include "rs_gen_internal.h"

void
rg_scan( const char* source_dir, rg_file_list_t* out )
{
    char all_paths[ RG_MAX_FILES ][ RG_MAX_PATH ];
    int  n = rg_platform_scan_dir( source_dir, all_paths, RG_MAX_FILES );

    out->count = 0;
    for ( int i = 0; i < n && out->count < RG_MAX_FILES; i++ )
    {
        if ( !rg_str_ends_with( all_paths[ i ], ".h" ) )
            continue;
        rg_str_copy( out->paths[ out->count ], all_paths[ i ], RG_MAX_PATH );
        out->count++;
    }
}
