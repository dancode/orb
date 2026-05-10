/*==============================================================================================

    win_thread.c - Windows implementation of the thread API.

==============================================================================================*/

/* Thread start trampoline */
/* Adapts the platform thread proc signature to our thread_fn_t. */
/* Allocated on the heap so it safely outlives thread_create's stack frame. */

typedef struct
{
    thread_fn_t fn;
    void*       arg;
} _ThreadStart;

static DWORD WINAPI
_thread_proc( LPVOID param )
{
    _ThreadStart* ts  = ( _ThreadStart* )param;
    thread_fn_t   fn  = ts->fn;
    void*         arg = ts->arg;
    free( ts );
    fn( arg );
    return 0;
}

/*==============================================================================================

    

==============================================================================================*/

thread_t
thread_create( thread_fn_t fn, void* arg, usize stack_bytes )
{
    thread_t t;
    memset( &t, 0, sizeof( t ) );

    _ThreadStart* ts = ( _ThreadStart* )malloc( sizeof( _ThreadStart ) );
    if ( !ts )
        return t;
    ts->fn   = fn;
    ts->arg  = arg;

    HANDLE h = CreateThread( NULL, ( SIZE_T )stack_bytes, _thread_proc, ts, 0, NULL );
    if ( !h )
    {
        free( ts );
        return t;
    }
    memcpy( t._opaque, &h, sizeof( h ) );
    return t;
}

b32
thread_valid( thread_t t )
{
    HANDLE h = NULL;
    memcpy( &h, t._opaque, sizeof( h ) );
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

void
thread_join( thread_t t )
{
    HANDLE h = NULL;
    memcpy( &h, t._opaque, sizeof( h ) );
    WaitForSingleObject( h, INFINITE );
    CloseHandle( h );
}

void
thread_detach( thread_t t )
{
    HANDLE h = NULL;
    memcpy( &h, t._opaque, sizeof( h ) );
    CloseHandle( h ); /* On Windows, CloseHandle on a running thread = detach */
}

thread_id_t
thread_current_id( void )
{
    return ( thread_id_t )GetCurrentThreadId();
}

void
thread_yield( void )
{
    SwitchToThread();
}

void
thread_sleep_ms( u32 ms )
{
    Sleep( ( DWORD )ms );
}

void
thread_set_name( thread_t t, const char* name )
{
/* SetThreadDescription requires Windows 10 1607+ and <processthreadsapi.h> */
#if _WIN32_WINNT >= 0x0A00
    HANDLE h = NULL;
    memcpy( &h, t._opaque, sizeof( h ) );
    /* Convert narrow to wide — simple ASCII path */
    wchar_t wname[ 256 ];
    int     i;
    for ( i = 0; name[ i ] && i < 255; ++i ) wname[ i ] = ( wchar_t )name[ i ];
    wname[ i ] = L'\0';
    SetThreadDescription( h, wname );
#else
    ( void )t;
    ( void )name;
#endif
}

/*============================================================================================*/