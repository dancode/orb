#if defined( _WIN32 )
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
        platform_time_ms()       -- monotonic millisecond counter  (GetTickCount64)
        platform_enable_ansi_color() -- enable VT processing on stdout (SetConsoleMode)
        platform_cpu_count()     -- logical processor count         (GetSystemInfo)
        platform_mkdir()         -- create a directory             (CreateDirectoryA)
        platform_find_first()    -- begin directory enumeration    (_findfirst)
        platform_find_next()     -- advance directory enumeration  (_findnext)
        platform_find_close()    -- end directory enumeration      (_findclose)

==============================================================================================*/
// clang-format off

#include <windows.h>
#include <direct.h>
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
    --- Monotonic Timer ---
==============================================================================================*/

/* Returns a monotonically increasing millisecond counter. Used for build timing.
   GetTickCount64 is wall-clock but monotonic and sufficient for sub-minute intervals. */

static uint64_t
platform_time_ms( void )
{
    return GetTickCount64();
}

/*==============================================================================================
    --- ANSI Color ---
==============================================================================================*/

/* Enables ANSI escape code processing on the process stdout handle.
   Fails silently when stdout is redirected to a file or pipe (GetConsoleMode returns false). */

static bool
platform_enable_ansi_color( void )
{
    HANDLE h = GetStdHandle( STD_OUTPUT_HANDLE );
    if ( h == INVALID_HANDLE_VALUE ) return false;
    DWORD mode = 0;
    if ( !GetConsoleMode( h, &mode ) ) return false;
    return SetConsoleMode( h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING ) != 0;
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

/*==============================================================================================
    --- Memory-Mapped File ---
==============================================================================================*/

/* Creates or truncates a zero-byte file at path. Used to write config stamp files. */

static void
platform_touch_file( const char* path )
{
    HANDLE h = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL );
    if ( h != INVALID_HANDLE_VALUE )
        CloseHandle( h );
}

/* Deletes a file at path. Silent no-op if the file does not exist. */

static void
platform_delete_file( const char* path )
{
    DeleteFileA( path );
}

/* Maps path into the process address space for read-only pointer access.
   Returns true on success. On false the file does not exist or could not be mapped.
   Empty files return true with data=NULL and size=0 -- no mapping is created.
   Call platform_unmap_file() to release resources when done. */

static bool
platform_map_file( const char* path, platform_mapped_file_t* out )
{
    out->data  = NULL;
    out->size  = 0;
    out->_file = NULL;
    out->_map  = NULL;

    HANDLE hFile = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if ( hFile == INVALID_HANDLE_VALUE )
        return false;

    LARGE_INTEGER sz;
    if ( !GetFileSizeEx( hFile, &sz ) )
    {
        CloseHandle( hFile );
        return false;
    }

    if ( sz.QuadPart == 0 )
    {
        // Empty file: valid, nothing to iterate.
        CloseHandle( hFile );
        return true;
    }

    HANDLE hMap = CreateFileMappingA( hFile, NULL, PAGE_READONLY, 0, 0, NULL );
    if ( !hMap )
    {
        CloseHandle( hFile );
        return false;
    }

    const char* data = (const char*)MapViewOfFile( hMap, FILE_MAP_READ, 0, 0, 0 );
    if ( !data )
    {
        CloseHandle( hMap );
        CloseHandle( hFile );
        return false;
    }

    out->data  = data;
    out->size  = (size_t)sz.QuadPart;
    out->_file = (void*)hFile;
    out->_map  = (void*)hMap;
    return true;
}

/* Releases handles acquired by platform_map_file(). Safe to call on a zeroed struct. */

static void
platform_unmap_file( platform_mapped_file_t* m )
{
    if ( m->data  ) UnmapViewOfFile( (void*)m->data );
    if ( m->_map  ) CloseHandle( (HANDLE)m->_map );
    if ( m->_file ) CloseHandle( (HANDLE)m->_file );
    m->data  = NULL;
    m->size  = 0;
    m->_file = NULL;
    m->_map  = NULL;
}

// clang-format on
/*============================================================================================*/
#endif