/*==============================================================================================

    win_sema.c - Windows implementation of the semaphore API.

==============================================================================================*/

_Static_assert( sizeof( HANDLE ) <= SEMA_BYTES, "HANDLE larger than sema_t._opaque; increase SEMA_BYTES" );

static HANDLE
_sema_handle( sema_t* s )
{
    HANDLE h;
    memcpy( &h, s->_opaque, sizeof( h ) );
    return h;
}

void
sema_init( sema_t* s, u32 initial_count )
{
    HANDLE h = CreateSemaphoreExW( NULL, ( LONG )initial_count, 0x7FFFFFFF, NULL, 0, SEMAPHORE_ALL_ACCESS );
    memset( s->_opaque, 0, SEMA_BYTES );
    memcpy( s->_opaque, &h, sizeof( h ) );
}

void
sema_destroy( sema_t* s )
{
    CloseHandle( _sema_handle( s ) );
    memset( s->_opaque, 0, SEMA_BYTES );
}

void
sema_wait( sema_t* s )
{
    WaitForSingleObjectEx( _sema_handle( s ), INFINITE, FALSE );
}

b32
sema_try_wait( sema_t* s )
{
    return WaitForSingleObjectEx( _sema_handle( s ), 0, FALSE ) == WAIT_OBJECT_0;
}

void
sema_post( sema_t* s, u32 count )
{
    ReleaseSemaphore( _sema_handle( s ), ( LONG )count, NULL );
}

/*============================================================================================*/