/*==============================================================================================

    file_watch_win32.c

    Windows implementation of file_watch.h using ReadDirectoryChangesW
    with overlapped (non-blocking) I/O.

    Design notes
    ------------

    - GetOverlappedResult is called with bWait=FALSE so poll() returns
      immediately when nothing has changed — zero filesystem overhead on
      quiet frames.

    - We watch FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME
      so we catch both in-place writes and rename-into-place (the MSVC/Clang
      linker pattern).

    - After draining a batch of notifications the async read is re-primed
      automatically so the next batch is never missed.

==============================================================================================*/
/*==============================================================================================
    Internal state
==============================================================================================*/

/* ReadDirectoryChangesW requires the buffer to be DWORD-aligned and large
   enough to hold at least one FILE_NOTIFY_INFORMATION record. 1 KB covers
   many simultaneous changes; overflow is handled gracefully (we just re-prime
   and wait for the next delivery). */

#define WATCH_BUF_SIZE 1024

typedef struct
{
    HANDLE     dir;                   /* directory handle            */
    OVERLAPPED ovl;                   /* overlapped I/O structure    */
    HANDLE     event;                 /* manual-reset event for ovl  */
    BYTE       buf[ WATCH_BUF_SIZE ]; /* notification buffer         */
    bool       read_pending;          /* async read is in flight      */
    char       last_error[ 256 ];

} file_watch_state_t;

static file_watch_state_t g_fw = { 0 };

/*==============================================================================================
    Internal helpers
==============================================================================================*/

static void
fw_set_error( const char* fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    vsnprintf( g_fw.last_error, sizeof( g_fw.last_error ), fmt, ap );
    va_end( ap );
}

/* Submit the next async read.  Must be called after init and after each
   successful GetOverlappedResult to keep the watch armed. */
static bool
fw_prime_read( void )
{
    ResetEvent( g_fw.event );

    BOOL ok = ReadDirectoryChangesW( g_fw.dir, g_fw.buf, sizeof( g_fw.buf ), FALSE, /* do not recurse into subdirs */
                                     FILE_NOTIFY_CHANGE_LAST_WRITE |   /* DLL overwritten in place    */
                                         FILE_NOTIFY_CHANGE_FILE_NAME, /* DLL renamed into place      */
                                     NULL,              /* bytes returned (unused with overlapped) */
                                     &g_fw.ovl, NULL ); /* no completion routine        */

    if ( !ok )
    {
        fw_set_error( "ReadDirectoryChangesW failed (0x%lx)", GetLastError() );
        return false;
    }

    g_fw.read_pending = true;
    return true;
}

/*==============================================================================================
    Public API
==============================================================================================*/

bool
file_watch_init( const char* dir_path )
{
    memset( &g_fw, 0, sizeof( g_fw ) );

    /* Manual-reset event so we control exactly when it clears. */
    g_fw.event = CreateEvent( NULL, TRUE, FALSE, NULL );
    if ( !g_fw.event )
    {
        fw_set_error( "CreateEvent failed (0x%lx)", GetLastError() );
        return false;
    }

    g_fw.ovl.hEvent = g_fw.event;

    /* Open the directory itself (not a file within it). */
    g_fw.dir = CreateFileA( dir_path, FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | /* required to open a directory */
                                FILE_FLAG_OVERLAPPED,
                            NULL );

    if ( g_fw.dir == INVALID_HANDLE_VALUE )
    {
        fw_set_error( "CreateFile on dir failed (0x%lx): %s", GetLastError(), dir_path );
        CloseHandle( g_fw.event );
        g_fw.event = NULL;
        return false;
    }

    /* Arm the first async read. */
    if ( !fw_prime_read() )
    {
        CloseHandle( g_fw.dir );
        CloseHandle( g_fw.event );
        g_fw.dir   = INVALID_HANDLE_VALUE;
        g_fw.event = NULL;
        return false;
    }

    return true;
}

int
file_watch_poll( file_watch_callback_t cb, void* userdata )
{
    if ( g_fw.dir == INVALID_HANDLE_VALUE || !g_fw.read_pending )
        return 0;

    /* Non-blocking check — returns immediately if the read is not done. */
    DWORD bytes = 0;
    if ( !GetOverlappedResult( g_fw.dir, &g_fw.ovl, &bytes, FALSE ) )
    {
        DWORD err = GetLastError();
        if ( err == ERROR_IO_INCOMPLETE )
            return 0; /* nothing ready this frame — normal case */

        /* ERROR_NOTIFY_ENUM_DIR means the buffer overflowed.
           Re-prime and continue — we lost this batch but won't miss future ones. */
        if ( err == ERROR_NOTIFY_ENUM_DIR )
        {
            fw_prime_read();
            return 0;
        }

        fw_set_error( "GetOverlappedResult failed (0x%lx)", err );
        g_fw.read_pending = false;
        return 0;
    }

    /* ------------------------------------------------------------------ */
    /* Walk the notification records in the buffer.                       */
    /* ------------------------------------------------------------------ */

    int   count = 0;
    BYTE* ptr   = g_fw.buf;

    for ( ;; )
    {
        FILE_NOTIFY_INFORMATION* info = ( FILE_NOTIFY_INFORMATION* )ptr;

        /* Convert the wide filename to a narrow string.
           FileNameLength is in bytes, not characters. */
        int  wlen = ( int )( info->FileNameLength / sizeof( WCHAR ) );
        char fname[ MAX_PATH ];
        int nlen = WideCharToMultiByte( CP_ACP, 0, info->FileName, wlen, fname, sizeof( fname ) - 1, NULL, NULL );
        if ( nlen > 0 )
        {
            fname[ nlen ] = '\0';
            cb( fname, userdata );
            ++count;
        }

        if ( info->NextEntryOffset == 0 )
            break;

        ptr += info->NextEntryOffset;
    }

    /* Re-arm for the next batch. */
    g_fw.read_pending = false;
    fw_prime_read();

    return count;
}

void
file_watch_shutdown( void )
{
    if ( g_fw.dir != INVALID_HANDLE_VALUE )
    {
        CancelIo( g_fw.dir );
        CloseHandle( g_fw.dir );
        g_fw.dir = INVALID_HANDLE_VALUE;
    }
    if ( g_fw.event )
    {
        CloseHandle( g_fw.event );
        g_fw.event = NULL;
    }
    g_fw.read_pending = false;
}

const char*
file_watch_last_error( void )
{
    return g_fw.last_error;
}

/*============================================================================================*/