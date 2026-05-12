/*==============================================================================================

    win_mutex.c - Windows implementation of the mutex API.

==============================================================================================*/

_Static_assert( sizeof( CRITICAL_SECTION ) <= MUTEX_BYTES,
                "CRITICAL_SECTION larger than mutex_t._opaque; increase MUTEX_BYTES" );

void
mutex_init( mutex_t* m )
{
    InitializeCriticalSection( ( CRITICAL_SECTION* )m->_opaque );
}

void
mutex_destroy( mutex_t* m )
{
    DeleteCriticalSection( ( CRITICAL_SECTION* )m->_opaque );
}

void
mutex_lock( mutex_t* m )
{
    EnterCriticalSection( ( CRITICAL_SECTION* )m->_opaque );
}

b32
mutex_try_lock( mutex_t* m )
{
    return TryEnterCriticalSection( ( CRITICAL_SECTION* )m->_opaque ) != 0;
}

void
mutex_unlock( mutex_t* m )
{
    LeaveCriticalSection( ( CRITICAL_SECTION* )m->_opaque );
}

/*============================================================================================*/