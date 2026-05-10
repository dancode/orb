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

/*============================================================================================*/