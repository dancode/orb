/*==============================================================================================

    build_tool_posix.c -- POSIX platform layer for the ORB build tool.

    Wraps every POSIX libc function that has an MSVC counterpart under a platform_
    prefix. All POSIX-specific includes live here; no other build_tool module
    includes them directly. The Win32 counterpart is build_tool_win.c.

    Functions implemented:
        platform_get_mtime()     -- last-modified time of a file   (stat / st_mtime)
        platform_file_exists()   -- presence check                 (access / F_OK)
        platform_fullpath()      -- resolve to absolute path       (realpath)
        platform_stricmp()       -- case-insensitive compare       (strcasecmp)
        platform_strnicmp()      -- case-insensitive compare (n)   (strncasecmp)
        platform_putenv()        -- set environment variable       (setenv)
        platform_popen()         -- open a pipe to a command       (popen)
        platform_pclose()        -- close a pipe                   (pclose)
        platform_cpu_count()     -- logical processor count        (sysconf)
        platform_mkdir()         -- create a directory             (mkdir)
        platform_find_first()    -- begin directory enumeration    (opendir / readdir / fnmatch)
        platform_find_next()     -- advance directory enumeration  (readdir / fnmatch)
        platform_find_close()    -- end directory enumeration      (closedir)

==============================================================================================*/
// clang-format off

#if defined( _WIN32 )
    #error "build_tool_posix.c is only for POSIX builds"
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <fnmatch.h>
#include <time.h>
#include <stdio.h>

/*==============================================================================================
    --- Mtime ---
==============================================================================================*/

/* Returns the last-modified timestamp of a file, or 0 if missing or unreadable. */

static platform_mtime_t
platform_get_mtime( const char* path )
{
    struct stat s;
    return ( stat( path, &s ) == 0 ) ? ( platform_mtime_t )s.st_mtime : 0;
}

/*==============================================================================================
    --- Existence Check ---
==============================================================================================*/

/* Returns true when the file or directory at path exists and is accessible. */

static bool
platform_file_exists( const char* path )
{
    return access( path, F_OK ) == 0;
}

/*==============================================================================================
    --- Absolute Path ---

    Uses realpath(path, NULL) to let the OS allocate the resolved buffer, avoiding
    the fixed-size stack overflow that realpath(in, buf[512]) would risk against
    a system PATH_MAX of 4096. The result is copied into the caller's sized buffer.
==============================================================================================*/

static bool
platform_fullpath( char* out, const char* in, size_t size )
{
    char* resolved = realpath( in, NULL );
    if ( !resolved ) return false;
    snprintf( out, size, "%s", resolved );
    free( resolved );
    return true;
}

/*==============================================================================================
    --- Case-Insensitive String Comparison ---
==============================================================================================*/

static int
platform_stricmp( const char* a, const char* b )
{
    return strcasecmp( a, b );
}

static int
platform_strnicmp( const char* a, const char* b, size_t n )
{
    return strncasecmp( a, b, n );
}

/*==============================================================================================
    --- Environment Variable ---
==============================================================================================*/

/* Sets an environment variable in the current process. Returns 0 on success. */

static int
platform_putenv( const char* key, const char* value )
{
    return setenv( key, value, 1 );
}

/*==============================================================================================
    --- Pipe ---
==============================================================================================*/

static FILE*
platform_popen( const char* cmd, const char* mode )
{
    return popen( cmd, mode );
}

static int
platform_pclose( FILE* pipe )
{
    return pclose( pipe );
}

/*==============================================================================================
    --- CPU Count ---
==============================================================================================*/

/* Returns the number of logical processors available to the process, clamped to [1, 16]. */

static int
platform_cpu_count( void )
{
    long n = sysconf( _SC_NPROCESSORS_ONLN );
    if ( n < 1  ) n = 1;
    if ( n > 16 ) n = 16;
    return ( int )n;
}

/*==============================================================================================
    --- Make Directory ---
==============================================================================================*/

/* Creates a directory at path. No-op if it already exists. */

static void
platform_mkdir( const char* path )
{
    mkdir( path, 0755 );
}

/*==============================================================================================
    --- Directory Enumeration ---

    The Win32 _findfirst/_findnext API takes a glob pattern like "bin/*.pdb".
    POSIX opendir/readdir iterates all entries; fnmatch selects matching names.
    A heap-allocated context struct carries the open DIR* and filename pattern
    between calls so the opaque platform_find_t handle stays a plain intptr_t.
==============================================================================================*/

typedef struct
{
    DIR* d;
    char dir[ PATH_MAX ];
    char pat[ 256 ];

} posix_find_ctx_t;

/* Begins enumeration matching pattern (e.g. "bin/*.pdb").
   Fills data and returns a valid handle, or PLATFORM_FIND_INVALID if no match. */

static platform_find_t
platform_find_first( const char* pattern, platform_find_data_t* data )
{
    // Split "dir/pat" into separate directory and filename-pattern parts.
    const char* sep = strrchr( pattern, '/' );
    if ( !sep ) sep = strrchr( pattern, '\\' );

    char dir[ PATH_MAX ];
    char pat[ 256 ];

    if ( sep )
    {
        size_t dir_len = ( size_t )( sep - pattern );
        snprintf( dir, sizeof( dir ), "%.*s", ( int )dir_len, pattern );
        snprintf( pat, sizeof( pat ), "%s", sep + 1 );
    }
    else
    {
        snprintf( dir, sizeof( dir ), "." );
        snprintf( pat, sizeof( pat ), "%s", pattern );
    }

    DIR* d = opendir( dir );
    if ( !d ) return PLATFORM_FIND_INVALID;

    posix_find_ctx_t* ctx = malloc( sizeof( posix_find_ctx_t ) );
    if ( !ctx ) { closedir( d ); return PLATFORM_FIND_INVALID; }

    ctx->d = d;
    snprintf( ctx->dir, sizeof( ctx->dir ), "%s", dir );
    snprintf( ctx->pat, sizeof( ctx->pat ), "%s", pat );

    // Advance to the first matching entry.
    struct dirent* ent;
    while ( ( ent = readdir( d ) ) != NULL )
    {
        if ( fnmatch( pat, ent->d_name, 0 ) != 0 ) continue;

        snprintf( data->name, sizeof( data->name ), "%s", ent->d_name );
        char full[ PATH_MAX ];
        snprintf( full, sizeof( full ), "%s/%s", dir, ent->d_name );
        struct stat st;
        data->is_dir = ( stat( full, &st ) == 0 ) && S_ISDIR( st.st_mode );
        return ( platform_find_t )ctx;
    }

    closedir( d );
    free( ctx );
    return PLATFORM_FIND_INVALID;
}

/* Advances to the next matching entry. Returns true and fills data while entries remain. */

static bool
platform_find_next( platform_find_t handle, platform_find_data_t* data )
{
    posix_find_ctx_t* ctx = ( posix_find_ctx_t* )handle;

    struct dirent* ent;
    while ( ( ent = readdir( ctx->d ) ) != NULL )
    {
        if ( fnmatch( ctx->pat, ent->d_name, 0 ) != 0 ) continue;

        snprintf( data->name, sizeof( data->name ), "%s", ent->d_name );
        char full[ PATH_MAX ];
        snprintf( full, sizeof( full ), "%s/%s", ctx->dir, ent->d_name );
        struct stat st;
        data->is_dir = ( stat( full, &st ) == 0 ) && S_ISDIR( st.st_mode );
        return true;
    }

    return false;
}

static void
platform_find_close( platform_find_t handle )
{
    posix_find_ctx_t* ctx = ( posix_find_ctx_t* )handle;
    closedir( ctx->d );
    free( ctx );
}

// clang-format on
/*============================================================================================*/
