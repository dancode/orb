/*==============================================================================================

    reflect_tool_scan.c - collect .h files from a source directory

==============================================================================================*/

#include "reflect_tool_internal.h"

/*--------------------------------------------------------------------------------------------*/
/* Scan a directory for .h files. Uses the platform abstraction to get a list of all files,
then filters to only .h files and writes those into the output list. Paths are normalized     */
 
void
scan( const char* source_dir, file_list_t* out )
{
    static char all_paths[ RT_MAX_FILES ][ RT_MAX_PATH ];
    int  n = platform_scan_dir( source_dir, all_paths, RT_MAX_FILES );

    str_copy( out->source_dir, source_dir, RT_MAX_PATH );
    out->count = 0;
    for ( int i = 0; i < n && out->count < RT_MAX_FILES; i++ )
    {
        if ( !str_ends_with( all_paths[ i ], ".h" ) )
            continue;
        str_copy( out->paths[ out->count ], all_paths[ i ], RT_MAX_PATH );
        out->count++;
    }
}

/*--------------------------------------------------------------------------------------------*/
