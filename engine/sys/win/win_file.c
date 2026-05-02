/*============================================================================================*/

uint64_t
platform_time_ms( void )
{
    return ( uint64_t )GetTickCount64();
}

/*============================================================================================*/

uint64_t
platform_file_time( const char* path )
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
platform_copy_file( const char* src, const char* dst )
{
    return CopyFileA( src, dst, FALSE ) != FALSE;
}

/*============================================================================================*/

bool
platform_delete_file( const char* path )
{
    return DeleteFileA( path ) != 0;
}

/*============================================================================================*/

void
platform_exe_dir( char* out, int size )
{
    assert( size >= MAX_PATH );
    DWORD len = GetModuleFileNameA( NULL, out, ( DWORD )size );
    if ( len == 0 || len == ( DWORD )size )
    {
        assert( 0 && "GetModuleFileNameA failed" );
        return;
    }
    /* strip the filename, keep the trailing slash */
    for ( DWORD i = len; i > 0; --i )
    {
        if ( out[ i ] == '\\' || out[ i ] == '/' )
        {
            out[ i ] = '\0';
            break;
        }
    }
}

/*============================================================================================*/