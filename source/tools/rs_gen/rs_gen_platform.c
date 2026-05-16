/*==============================================================================================

    rs_gen_platform.c - platform abstractions for rs_gen (mkdir + directory scan)

==============================================================================================*/

#include "rs_gen_internal.h"

/*----------------------------------------------------------------------------------------------
    Windows
----------------------------------------------------------------------------------------------*/

#ifdef _WIN32

#    include <windows.h>
#    include <direct.h>
#    include <stdint.h>

void
rg_platform_mkdir( const char* path )
{
    _mkdir( path );
}

int
rg_platform_scan_dir( const char* dir, char out_paths[][ RG_MAX_PATH ], int max_files )
{
    char pattern[ RG_MAX_PATH ];
    rg_str_copy( pattern, dir, RG_MAX_PATH );
    rg_str_cat( pattern, "/*", RG_MAX_PATH );

    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA( pattern, &fd );
    if ( h == INVALID_HANDLE_VALUE )
        return 0;

    int count = 0;
    do
    {
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

/*----------------------------------------------------------------------------------------------
    POSIX
----------------------------------------------------------------------------------------------*/

#else

#    include <dirent.h>
#    include <sys/stat.h>
#    include <unistd.h>

void
rg_platform_mkdir( const char* path )
{
    mkdir( path, 0755 );
}

int
rg_platform_scan_dir( const char* dir, char out_paths[][ RG_MAX_PATH ], int max_files )
{
    DIR* d = opendir( dir );
    if ( !d )
        return 0;

    int            count = 0;
    struct dirent* e;
    while ( ( e = readdir( d ) ) && count < max_files )
    {
        if ( e->d_name[ 0 ] == '.' )
            continue;
        rg_str_copy( out_paths[ count ], dir, RG_MAX_PATH );
        rg_str_cat( out_paths[ count ], "/", RG_MAX_PATH );
        rg_str_cat( out_paths[ count ], e->d_name, RG_MAX_PATH );
        count++;
    }

    closedir( d );
    return count;
}

#endif

/*----------------------------------------------------------------------------------------------
    exe dir - shared impl (strips filename from full exe path, normalises to forward slashes)
----------------------------------------------------------------------------------------------*/

static void
rg_strip_filename( char* path )
{
    int last = -1;
    for ( int i = 0; path[ i ]; i++ )
        if ( path[ i ] == '/' || path[ i ] == '\\' )
            last = i;
    if ( last >= 0 )
        path[ last ] = '\0';
}

void
rg_platform_exe_dir( char* out, int max )
{
#ifdef _WIN32
    GetModuleFileNameA( NULL, out, (DWORD)max );
    rg_strip_filename( out );
#else
    // Linux: /proc/self/exe; macOS falls back to empty string (debug path will still work
    // if the binary runs from the build/bin directory).
    ssize_t len = readlink( "/proc/self/exe", out, (size_t)( max - 1 ) );
    if ( len > 0 )
    {
        out[ len ] = '\0';
        rg_strip_filename( out );
    }
    else
    {
        out[ 0 ] = '.';
        out[ 1 ] = '\0';
    }
#endif

    // Normalise backslashes so path concatenation is uniform
    for ( int i = 0; out[ i ]; i++ )
        if ( out[ i ] == '\\' )
            out[ i ] = '/';
}

