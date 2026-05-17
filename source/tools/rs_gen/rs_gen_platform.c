/*==============================================================================================

    rs_gen_platform.c - platform abstractions for rs_gen (mkdir + directory scan + exe path)

==============================================================================================*/

#include "rs_gen_internal.h"

/*----------------------------------------------------------------------------------------------
    Windows
----------------------------------------------------------------------------------------------*/

#ifdef _WIN32

    #define NOMINMAX
    #define WIN32_LEAN_AND_MEAN
    #define WIN32_EXTRA_LEAN
    #define VC_EXTRALEAN

    #include <windows.h>

/*--------------------------------------------------------------------------------------------*/
/* Create a directory if it doesn't already exist.                                            */

void
rg_platform_mkdir( const char* path )
{
    if ( !CreateDirectoryA( path, NULL ) )
    {
        /* returns failure if the directory already exists, so ignore that case. */
        DWORD err = GetLastError();
        if ( err != ERROR_ALREADY_EXISTS )
            fprintf( stderr, "[build_reflect] warning: cannot create directory '%s' (err %lu)\n", path,
                     ( unsigned long )err );
    }
}

/*--------------------------------------------------------------------------------------------*/
/* Scan a directory for files. Returns the number of files found, and writes up to max_files
   full paths into out_paths. Paths are normalized to use forward slashes.                    */

int
rg_platform_scan_dir( const char* dir, char out_paths[][ RG_MAX_PATH ], int max_files )
{
    char pattern[ RG_MAX_PATH ];
    rg_str_copy( pattern, dir, RG_MAX_PATH );
    rg_str_cat( pattern, "/*", RG_MAX_PATH );

    WIN32_FIND_DATAA fd;

    /* if the directory doesn't exist or some other error occurs, return 0 files found. */

    HANDLE h = FindFirstFileA( pattern, &fd );
    if ( h == INVALID_HANDLE_VALUE )
        return 0;

    int count = 0;
    do {
        if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
            continue;
        if ( count >= max_files )
            break;
        rg_str_copy( out_paths[ count ], dir, RG_MAX_PATH );
        rg_str_cat( out_paths[ count ], "/", RG_MAX_PATH );
        rg_str_cat( out_paths[ count ], fd.cFileName, RG_MAX_PATH );
        count++;
    }
    while ( FindNextFileA( h, &fd ) );

    FindClose( h );
    return count;
}

/*--------------------------------------------------------------------------------------------*/
/* Get the directory of the executable. Paths are normalized to use forward slashes.          */

void
rg_platform_exe_dir( char* out, int max )
{
    GetModuleFileNameA( NULL, out, ( DWORD )max );
    /* Normalize backslashes and strip filename in a single pass. */
    int last_sep = -1;
    for ( int i = 0; out[ i ]; i++ )
    {
        if ( out[ i ] == '\\' )
            out[ i ] = '/';
        if ( out[ i ] == '/' )
            last_sep = i;
    }
    if ( last_sep >= 0 )
        out[ last_sep ] = '\0';
}

/*----------------------------------------------------------------------------------------------
    POSIX
----------------------------------------------------------------------------------------------*/

#else

    #error "rs_gen: platform not implemented"

/*--------------------------------------------------------------------------------------------*/
#endif
