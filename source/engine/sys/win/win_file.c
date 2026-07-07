/*============================================================================================*/

void
sys_exe_dir( char* out, int size )
{
    /* GetModuleFileNameA returns the full path of the executable, including the filename.
       We need to strip the filename to get the directory. */

    assert( size >= MAX_PATH );
    DWORD len = GetModuleFileNameA( NULL, out, ( DWORD )size );
    if ( len == 0 || len >= ( DWORD )size )
    {
        assert( 0 && "GetModuleFileNameA failed" );
        out[ 0 ] = '\0';
        return;
    }
    /* strip the filename, keep the trailing slash */
    for ( DWORD i = len; i > 0; --i )
    {
        if ( out[ i ] == '\\' || out[ i ] == '/' )
        {
            out[ i ] = '\0';
            return;
        }
    }
    out[ 0 ] = '\0';
}

/*============================================================================================*/

uint64_t
sys_time_ms( void )
{
    /* GetTickCount64 returns the number of milliseconds that have elapsed since the system
       was started. It is not affected by system time changes and has a resolution of around
       10-16 ms, which is sufficient for our debounce timing.

       For higher-resolution timing, QueryPerformanceCounter (sys_tick) can be used. */

    return ( uint64_t )GetTickCount64();
}

/*============================================================================================*/

uint64_t
sys_file_time( const char* path )
{
    /* Returns 0 on error or if the file does not exist. */

    WIN32_FILE_ATTRIBUTE_DATA data;
    if ( !GetFileAttributesExA( path, GetFileExInfoStandard, &data ) )
        return 0;

    ULARGE_INTEGER uli;
    uli.LowPart  = data.ftLastWriteTime.dwLowDateTime;
    uli.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return uli.QuadPart;
}

/*============================================================================================*/

bool
sys_file_copy( const char* src, const char* dst )
{
    /* CopyFileA returns nonzero on success, zero on failure. The last error code can be retrieved
       with GetLastError(). We ignore the error code here since the caller will check for the
       existence of the destination file and handle errors as needed. */

    return CopyFileA( src, dst, FALSE ) != FALSE;
}

/*============================================================================================*/

bool
sys_file_delete( const char* path )
{
    /* DeleteFileA returns nonzero on success, zero on failure. The last error code can be retrieved
       with GetLastError(). We ignore the error code here since the caller will check for the
       existence of the file and handle errors as needed. */

    return DeleteFileA( path ) != 0;
}

/*==============================================================================================
    : Whole-file read / write
==============================================================================================*/

sys_file_data_t
sys_file_read_entire( const char* path )
{
    sys_file_data_t out = { NULL, 0, false };

    HANDLE h = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL );
    if ( h == INVALID_HANDLE_VALUE )
        return out;

    LARGE_INTEGER li;
    if ( !GetFileSizeEx( h, &li ) || li.QuadPart > 0xFFFFFFFFULL )
    {
        /* Reject >4 GB: the size field is u32 and asset files never approach that. */
        CloseHandle( h );
        return out;
    }

    u32   size = ( u32 )li.QuadPart;
    char* buf  = ( char* )malloc( ( size_t )size + 1 ); /* +1 for the hidden NUL terminator */
    if ( !buf )
    {
        CloseHandle( h );
        return out;
    }

    /* ReadFile caps at a DWORD per call; loop to be safe for large files. */
    u32 read_total = 0;
    while ( read_total < size )
    {
        DWORD got = 0;
        if ( !ReadFile( h, buf + read_total, size - read_total, &got, NULL ) || got == 0 )
        {
            free( buf );
            CloseHandle( h );
            return out;
        }
        read_total += got;
    }
    CloseHandle( h );

    buf[ size ] = '\0'; /* convenience terminator, not counted in size */
    out.data    = buf;
    out.size    = size;
    out.ok      = true;
    return out;
}

/*============================================================================================*/

void
sys_file_free( sys_file_data_t* fd )
{
    if ( !fd )
        return;
    free( fd->data );
    fd->data = NULL;
    fd->size = 0;
    fd->ok   = false;
}

/*============================================================================================*/

bool
sys_file_write_entire( const char* path, const void* data, u32 size )
{
    HANDLE h = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL );
    if ( h == INVALID_HANDLE_VALUE )
        return false;

    /* An empty write is a valid truncate-to-zero. */
    u32 written_total = 0;
    while ( written_total < size )
    {
        DWORD put = 0;
        if ( !WriteFile( h, ( const char* )data + written_total, size - written_total, &put, NULL )
             || put == 0 )
        {
            CloseHandle( h );
            return false;
        }
        written_total += put;
    }
    CloseHandle( h );
    return true;
}

/*============================================================================================*/

bool
sys_file_exists( const char* path )
{
    DWORD attr = GetFileAttributesA( path );
    return attr != INVALID_FILE_ATTRIBUTES && !( attr & FILE_ATTRIBUTE_DIRECTORY );
}

/*============================================================================================*/

u32
sys_file_size( const char* path )
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if ( !GetFileAttributesExA( path, GetFileExInfoStandard, &data ) )
        return 0;
    if ( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
        return 0;

    ULARGE_INTEGER uli;
    uli.LowPart  = data.nFileSizeLow;
    uli.HighPart = data.nFileSizeHigh;
    return uli.QuadPart > 0xFFFFFFFFULL ? 0xFFFFFFFFu : ( u32 )uli.QuadPart;
}

/*==============================================================================================
    : Enumerate files matching a glob pattern
==============================================================================================*/

int
sys_file_glob( const char* dir, const char* pattern, sys_glob_fn cb, void* userdata )
{
    char search[ MAX_PATH ];
    snprintf( search, sizeof( search ), "%s\\%s", dir, pattern );

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA( search, &fd );
    if ( h == INVALID_HANDLE_VALUE )
        return 0;

    int count = 0;
    do {
        /* Skip the pseudo-entries the OS always returns. */
        if ( fd.cFileName[ 0 ] == '.' )
            continue;

        /* Skip subdirectories — callers only want files. */
        if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
            continue;

        char full[ MAX_PATH ];
        snprintf( full, sizeof( full ), "%s\\%s", dir, fd.cFileName );

        ++count;
        if ( !cb( fd.cFileName, full, userdata ) )
            break;
    }
    while ( FindNextFileA( h, &fd ) );

    FindClose( h );
    return count;
}

/*==============================================================================================
    : Recursive directory create (mkdir -p)
==============================================================================================*/

bool
sys_dir_make( const char* path )
{
    if ( !path || !path[ 0 ] )
        return false;

    /* Work on a private, backslash-normalized copy; create each ancestor in turn. */
    char tmp[ MAX_PATH ];
    int  n = 0;
    while ( path[ n ] && n < ( int )sizeof( tmp ) - 1 )
    {
        char c    = path[ n ];
        tmp[ n ]  = ( c == '/' ) ? '\\' : c;
        ++n;
    }
    if ( path[ n ] )   /* path did not fit */
        return false;
    tmp[ n ] = '\0';

    for ( int i = 1; i < n; ++i )
    {
        if ( tmp[ i ] == '\\' )
        {
            tmp[ i ] = '\0';
            /* Skip a bare drive root like "C:" -- CreateDirectory would fail on it. */
            if ( !( tmp[ 1 ] == ':' && tmp[ 2 ] == '\0' ) )
                CreateDirectoryA( tmp, NULL ); /* ignores ERROR_ALREADY_EXISTS */
            tmp[ i ] = '\\';
        }
    }
    CreateDirectoryA( tmp, NULL );

    DWORD attr = GetFileAttributesA( tmp );
    return attr != INVALID_FILE_ATTRIBUTES && ( attr & FILE_ATTRIBUTE_DIRECTORY );
}

/*==============================================================================================
    : Recursive file walk
==============================================================================================*/

static int
sys_dir_walk_rec( const char* dir, sys_glob_fn cb, void* userdata, bool* stop )
{
    char search[ MAX_PATH ];
    snprintf( search, sizeof( search ), "%s\\*", dir );

    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA( search, &fd );
    if ( h == INVALID_HANDLE_VALUE )
        return 0;

    int count = 0;
    do {
        /* Skip "." and ".." (and any dot-prefixed entry, matching sys_file_glob). */
        if ( fd.cFileName[ 0 ] == '.' )
            continue;

        char full[ MAX_PATH ];
        snprintf( full, sizeof( full ), "%s\\%s", dir, fd.cFileName );

        if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
        {
            count += sys_dir_walk_rec( full, cb, userdata, stop );
            if ( *stop )
                break;
        }
        else
        {
            ++count;
            if ( !cb( fd.cFileName, full, userdata ) )
            {
                *stop = true;
                break;
            }
        }
    }
    while ( FindNextFileA( h, &fd ) );

    FindClose( h );
    return count;
}

int
sys_dir_walk( const char* root, sys_glob_fn cb, void* userdata )
{
    bool stop = false;
    return sys_dir_walk_rec( root, cb, userdata, &stop );
}

/*============================================================================================*/