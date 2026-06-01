#if defined( _WIN32 )
/*==============================================================================================

    build_tool_win_thread.c -- Windows threading platform layer for the ORB build tool.

    Wraps every Win32 threading primitive used by the scheduler under platform_
    prefixed names. A future build_tool_posix_thread.c would provide identical
    symbols using pthreads; build_tool.c selects one via an #include branch.

    Types are declared as opaque structs / platform-conditional typedefs in build_tool.h:
        platform_mutex_t            -- _opaque[PLATFORM_MUTEX_BYTES] -> CRITICAL_SECTION
        platform_cond_t             -- _opaque[PLATFORM_COND_BYTES]  -> CONDITION_VARIABLE
        platform_tls_t              -- uint32_t TLS slot index
        PLATFORM_TLS_INVALID        -- 0xFFFFFFFF sentinel
        platform_thread_t           -- void* (Win32 HANDLE)
        PLATFORM_THREAD_ENTRY       -- unsigned __stdcall
        platform_thread_fn_t        -- unsigned (__stdcall *)(void*)

    Functions implemented:
        platform_mutex_init()       -- InitializeCriticalSection
        platform_mutex_lock()       -- EnterCriticalSection
        platform_mutex_unlock()     -- LeaveCriticalSection
        platform_mutex_destroy()    -- DeleteCriticalSection
        platform_cond_init()        -- InitializeConditionVariable
        platform_cond_wait()        -- SleepConditionVariableCS (INFINITE)
        platform_cond_broadcast()   -- WakeAllConditionVariable
        platform_cond_destroy()     -- no-op (CONDITION_VARIABLE needs no teardown)
        platform_tls_alloc()        -- TlsAlloc
        platform_tls_is_valid()     -- slot != TLS_OUT_OF_INDEXES
        platform_tls_get()          -- TlsGetValue
        platform_tls_set()          -- TlsSetValue
        platform_thread_create()    -- _beginthreadex
        platform_threads_join()     -- WaitForMultipleObjects + CloseHandle loop
        platform_lock_create()      -- named cross-process mutex  (CreateMutexA + WaitForSingleObject)
        platform_lock_release()     -- release named mutex        (ReleaseMutex + CloseHandle)

==============================================================================================*/
// clang-format off

#if !defined( _WIN32 )
    #error "build_tool_win_thread.c is only for Windows / MSVC builds"
#endif

/*==============================================================================================
    --- Platform Threading Types ---
    Opaque types (platform_mutex_t, platform_cond_t, platform_tls_t, platform_thread_t,
    PLATFORM_THREAD_ENTRY, platform_thread_fn_t) are declared in build_tool.h.
    Cast helpers below map the opaque _opaque buffer to the real Win32 type.
==============================================================================================*/

_Static_assert( sizeof( CRITICAL_SECTION )   <= PLATFORM_MUTEX_BYTES, "increase PLATFORM_MUTEX_BYTES" );
_Static_assert( sizeof( CONDITION_VARIABLE ) <= PLATFORM_COND_BYTES,  "increase PLATFORM_COND_BYTES"  );

static CRITICAL_SECTION*   mutex_w32( platform_mutex_t* m ) { return ( CRITICAL_SECTION*   )m->_opaque; }
static CONDITION_VARIABLE* cond_w32(  platform_cond_t*  c ) { return ( CONDITION_VARIABLE* )c->_opaque; }

/*==============================================================================================
    --- Mutex ---
==============================================================================================*/

static void
platform_mutex_init( platform_mutex_t* m )
{
    InitializeCriticalSection( mutex_w32( m ) );
}

static void
platform_mutex_lock( platform_mutex_t* m )
{
    EnterCriticalSection( mutex_w32( m ) );
}

static void
platform_mutex_unlock( platform_mutex_t* m )
{
    LeaveCriticalSection( mutex_w32( m ) );
}

static void
platform_mutex_destroy( platform_mutex_t* m )
{
    DeleteCriticalSection( mutex_w32( m ) );
}

/*==============================================================================================
    --- Condition Variable ---
==============================================================================================*/

static void
platform_cond_init( platform_cond_t* c )
{
    InitializeConditionVariable( cond_w32( c ) );
}

/* Atomically releases the mutex and sleeps until platform_cond_broadcast() wakes this thread,
   then reacquires the mutex before returning. */

static void
platform_cond_wait( platform_cond_t* c, platform_mutex_t* m )
{
    SleepConditionVariableCS( cond_w32( c ), mutex_w32( m ), INFINITE );
}

/* Wakes all threads waiting on c. */

static void
platform_cond_broadcast( platform_cond_t* c )
{
    WakeAllConditionVariable( cond_w32( c ) );
}

/* Tears down a condition variable. Win32 CONDITION_VARIABLE needs no explicit
   destruction; provided for API symmetry with the POSIX pthread_cond_destroy. */

static void
platform_cond_destroy( platform_cond_t* c )
{
    ( void )c;
}

/*==============================================================================================
    --- Thread-Local Storage ---
==============================================================================================*/

/* Allocates a TLS slot and writes it to *slot. Returns true on success. */

static bool
platform_tls_alloc( platform_tls_t* slot )
{
    *slot = ( platform_tls_t )TlsAlloc();
    return *slot != PLATFORM_TLS_INVALID;
}

/* Returns true when slot was successfully allocated (i.e. is not the sentinel). */

static bool
platform_tls_is_valid( platform_tls_t slot )
{
    return slot != PLATFORM_TLS_INVALID;
}

static void*
platform_tls_get( platform_tls_t slot )
{
    return TlsGetValue( slot );
}

static void
platform_tls_set( platform_tls_t slot, void* value )
{
    TlsSetValue( slot, value );
}

/*==============================================================================================
    --- Thread Lifecycle ---
==============================================================================================*/

/* Creates and starts an OS thread running fn(arg). Returns NULL on failure. */

static platform_thread_t
platform_thread_create( platform_thread_fn_t fn, void* arg )
{
    return ( platform_thread_t )_beginthreadex( NULL, 0, fn, arg, 0, NULL );
}

/* Blocks until all count threads have exited, then releases their handles. */

static void
platform_threads_join( platform_thread_t* threads, int count )
{
    if ( count <= 0 ) return;
    WaitForMultipleObjects( ( DWORD )count, ( HANDLE* )threads, TRUE, INFINITE );
    for ( int i = 0; i < count; ++i ) CloseHandle( ( HANDLE )threads[ i ] );
}

/*==============================================================================================
    --- Named Cross-Process Mutex ---

    Serializes concurrent build_tool.exe invocations targeting the same artifact.
    Lives in the unprivileged local-session namespace; failure is non-fatal.
==============================================================================================*/

/* Acquires a named mutex, blocking until granted. Returns opaque handle, or NULL on failure. */

static void*
platform_lock_create( const char* name )
{
    HANDLE h = CreateMutexA( NULL, FALSE, name );
    if ( !h ) return NULL;
    WaitForSingleObject( h, INFINITE );
    return ( void* )h;
}

/* Releases and closes a lock returned by platform_lock_create(). NULL is a safe no-op. */

static void
platform_lock_release( void* lock )
{
    if ( !lock ) return;
    ReleaseMutex( ( HANDLE )lock );
    CloseHandle( ( HANDLE )lock );
}

// clang-format on
/*============================================================================================*/
#endif
