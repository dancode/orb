/*==============================================================================================

    build_tool_win.c -- Windows / MSVC platform layer for the ORB build tool.

    Wraps every MSVC CRT function that has a POSIX counterpart under a platform_
    prefix. All MSVC-specific includes live here; no other build_tool module
    includes them directly. A future build_tool_posix.c would provide the same
    symbols using POSIX APIs.

    Functions implemented:
        platform_get_mtime()     -- last-modified time of a file   (_stat64)
        platform_file_exists()   -- presence check                 (_access)
        platform_fullpath()      -- resolve to absolute path       (_fullpath)
        platform_stricmp()       -- case-insensitive compare       (_stricmp)
        platform_strnicmp()      -- case-insensitive compare (n)   (_strnicmp)
        platform_putenv()        -- set environment variable       (_putenv_s)
        platform_popen()         -- open a pipe to a command       (_popen)
        platform_pclose()        -- close a pipe                   (_pclose)
        platform_cpu_count()     -- logical processor count         (GetSystemInfo)
        platform_mkdir()         -- create a directory             (CreateDirectoryA)
        platform_find_first()    -- begin directory enumeration    (_findfirst)
        platform_find_next()     -- advance directory enumeration  (_findnext)
        platform_find_close()    -- end directory enumeration      (_findclose)

==============================================================================================*/
// clang-format off

#if !defined( _WIN32 )
    #error "build_tool_win.c is only for Windows / MSVC builds"
#endif

#include <sys/stat.h>
#include <io.h>
#include <process.h>
#include <time.h>

/*==============================================================================================
    --- Mtime ---
==============================================================================================*/

/* Returns the last-modified timestamp of a file, or 0 if missing or unreadable. */

static platform_mtime_t
platform_get_mtime( const char* path )
{
    struct __stat64 s;
    return ( _stat64( path, &s ) == 0 ) ? ( platform_mtime_t )s.st_mtime : 0;
}

/*==============================================================================================
    --- Existence Check ---
==============================================================================================*/

/* Returns true when the file or directory at path exists and is accessible. */

static bool
platform_file_exists( const char* path )
{
    return _access( path, 0 ) == 0;
}

/*==============================================================================================
    --- Absolute Path ---
==============================================================================================*/

/* Resolves in to an absolute canonical path and writes it to out (size bytes).
   Returns true on success; false leaves out in an indeterminate state. */

static bool
platform_fullpath( char* out, const char* in, size_t size )
{
    return _fullpath( out, in, size ) != NULL;
}

/*==============================================================================================
    --- Case-Insensitive String Comparison ---
==============================================================================================*/

static int
platform_stricmp( const char* a, const char* b )
{
    return _stricmp( a, b );
}

static int
platform_strnicmp( const char* a, const char* b, size_t n )
{
    return _strnicmp( a, b, n );
}

/*==============================================================================================
    --- Environment Variable ---
==============================================================================================*/

/* Sets an environment variable in the current process. Returns 0 on success. */

static int
platform_putenv( const char* key, const char* value )
{
    return _putenv_s( key, value );
}

/*==============================================================================================
    --- Pipe ---
==============================================================================================*/

static FILE*
platform_popen( const char* cmd, const char* mode )
{
    return _popen( cmd, mode );
}

static int
platform_pclose( FILE* pipe )
{
    return _pclose( pipe );
}

/*==============================================================================================
    --- CPU Count ---
==============================================================================================*/

/* Returns the number of logical processors available to the process, clamped to [1, 32]. */

static int
platform_cpu_count( void )
{
    SYSTEM_INFO si;
    GetSystemInfo( &si );
    int n = (int)si.dwNumberOfProcessors;
    if ( n < 1  ) n = 1;
    if ( n > 32 ) n = 32;
    return n;
}

/*==============================================================================================
    --- Make Directory ---
==============================================================================================*/

/* Creates a directory at path. No-op if it already exists. */

static void
platform_mkdir( const char* path )
{
    CreateDirectoryA( path, NULL );
}

/*==============================================================================================
    --- Directory Enumeration ---
==============================================================================================*/

/* Begins enumeration matching pattern (e.g. "bin\\*.pdb").
   Fills data and returns a valid handle, or PLATFORM_FIND_INVALID if no match. */

static platform_find_t
platform_find_first( const char* pattern, platform_find_data_t* data )
{
    struct _finddata_t fd;
    intptr_t h = _findfirst( pattern, &fd );
    if ( h == -1 ) return PLATFORM_FIND_INVALID;
    snprintf( data->name, sizeof( data->name ), "%s", fd.name );
    data->is_dir = ( fd.attrib & _A_SUBDIR ) != 0;
    return h;
}

/* Advances to the next entry. Returns true and fills data while entries remain. */

static bool
platform_find_next( platform_find_t handle, platform_find_data_t* data )
{
    struct _finddata_t fd;
    if ( _findnext( handle, &fd ) != 0 ) return false;
    snprintf( data->name, sizeof( data->name ), "%s", fd.name );
    data->is_dir = ( fd.attrib & _A_SUBDIR ) != 0;
    return true;
}

static void
platform_find_close( platform_find_t handle )
{
    _findclose( handle );
}

// clang-format on
/*============================================================================================*/
