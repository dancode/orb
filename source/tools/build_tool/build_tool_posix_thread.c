#if !defined( _WIN32 )
/*==============================================================================================

    build_tool_posix_thread.c -- POSIX threading platform layer for the ORB build tool.

    Wraps every pthreads primitive used by the scheduler under platform_ prefixed names.
    The Win32 counterpart is build_tool_win_thread.c; both files expose identical symbols.

    Types are declared as opaque structs / platform-conditional typedefs in build_tool.h:
        platform_mutex_t            -- _opaque[PLATFORM_MUTEX_BYTES] -> pthread_mutex_t
        platform_cond_t             -- _opaque[PLATFORM_COND_BYTES]  -> pthread_cond_t
        platform_tls_t              -- uint32_t TLS slot index (cast to pthread_key_t)
        PLATFORM_TLS_INVALID        -- all-bits-set sentinel (~0u)
        platform_thread_t           -- void* (heap-allocated pthread_t)
        PLATFORM_THREAD_ENTRY       -- void*
        platform_thread_fn_t        -- void*(*)(void*)

    Functions implemented:
        platform_mutex_init()       -- pthread_mutex_init
        platform_mutex_lock()       -- pthread_mutex_lock
        platform_mutex_unlock()     -- pthread_mutex_unlock
        platform_mutex_destroy()    -- pthread_mutex_destroy
        platform_cond_init()        -- pthread_cond_init
        platform_cond_wait()        -- pthread_cond_wait (INFINITE equivalent)
        platform_cond_broadcast()   -- pthread_cond_broadcast
        platform_cond_destroy()     -- pthread_cond_destroy
        platform_tls_alloc()        -- pthread_key_create
        platform_tls_is_valid()     -- key != PLATFORM_TLS_INVALID
        platform_tls_get()          -- pthread_getspecific
        platform_tls_set()          -- pthread_setspecific
        platform_thread_create()    -- pthread_create  (pthread_t heap-allocated)
        platform_threads_join()     -- pthread_join loop + free
        platform_lock_create()      -- flock on /tmp/orb_lock_<name>  (open + flock LOCK_EX)
        platform_lock_release()     -- release flock                  (flock LOCK_UN + close)

==============================================================================================*/
// clang-format off

#if defined( _WIN32 )
    #error "build_tool_posix_thread.c is only for POSIX builds"
#endif

#if !defined(_WIN32)
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#endif 

/*==============================================================================================
    --- Platform Threading Types ---
    Opaque types (platform_mutex_t, platform_cond_t, platform_tls_t, platform_thread_t,
    PLATFORM_THREAD_ENTRY, platform_thread_fn_t) are declared in build_tool.h.
    Cast helpers below map the opaque _opaque buffer to the real pthreads type.
==============================================================================================*/

_Static_assert( sizeof( pthread_mutex_t ) <= PLATFORM_MUTEX_BYTES, "increase PLATFORM_MUTEX_BYTES" );
_Static_assert( sizeof( pthread_cond_t  ) <= PLATFORM_COND_BYTES,  "increase PLATFORM_COND_BYTES"  );

static pthread_mutex_t* mutex_posix( platform_mutex_t* m ) { return ( pthread_mutex_t* )m->_opaque; }
static pthread_cond_t*  cond_posix(  platform_cond_t*  c ) { return ( pthread_cond_t*  )c->_opaque; }

/*==============================================================================================
    --- Mutex ---
==============================================================================================*/

static void
platform_mutex_init( platform_mutex_t* m )
{
    pthread_mutex_init( mutex_posix( m ), NULL );
}

static void
platform_mutex_lock( platform_mutex_t* m )
{
    pthread_mutex_lock( mutex_posix( m ) );
}

static void
platform_mutex_unlock( platform_mutex_t* m )
{
    pthread_mutex_unlock( mutex_posix( m ) );
}

static void
platform_mutex_destroy( platform_mutex_t* m )
{
    pthread_mutex_destroy( mutex_posix( m ) );
}

/*==============================================================================================
    --- Condition Variable ---
==============================================================================================*/

static void
platform_cond_init( platform_cond_t* c )
{
    pthread_cond_init( cond_posix( c ), NULL );
}

/* Atomically releases the mutex and sleeps until platform_cond_broadcast() wakes this thread,
   then reacquires the mutex before returning. */

static void
platform_cond_wait( platform_cond_t* c, platform_mutex_t* m )
{
    pthread_cond_wait( cond_posix( c ), mutex_posix( m ) );
}

/* Wakes all threads waiting on c. */

static void
platform_cond_broadcast( platform_cond_t* c )
{
    pthread_cond_broadcast( cond_posix( c ) );
}

/* Tears down a condition variable, releasing any kernel resources it holds. */

static void
platform_cond_destroy( platform_cond_t* c )
{
    pthread_cond_destroy( cond_posix( c ) );
}

/*==============================================================================================
    --- Thread-Local Storage ---
==============================================================================================*/

/* Allocates a TLS key and writes it to *slot. Returns true on success. */

static bool
platform_tls_alloc( platform_tls_t* slot )
{
    *slot = PLATFORM_TLS_INVALID;
    pthread_key_t key;
    if ( pthread_key_create( &key, NULL ) != 0 ) return false;
    *slot = ( platform_tls_t )key;
    return true;
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
    return pthread_getspecific( ( pthread_key_t )slot );
}

static void
platform_tls_set( platform_tls_t slot, void* value )
{
    pthread_setspecific( ( pthread_key_t )slot, value );
}

/*==============================================================================================
    --- Thread Lifecycle ---

    pthread_t is not pointer-sized on all platforms (e.g. unsigned long on Linux x64).
    To keep platform_thread_t as void*, each thread handle is heap-allocated and freed
    here in the join loop. Callers never dereference the opaque pointer directly.
==============================================================================================*/

/* Creates and starts an OS thread running fn(arg). Returns NULL on failure. */

static platform_thread_t
platform_thread_create( platform_thread_fn_t fn, void* arg )
{
    pthread_t* t = malloc( sizeof( pthread_t ) );
    if ( !t ) return NULL;
    if ( pthread_create( t, NULL, fn, arg ) != 0 ) { free( t ); return NULL; }
    return ( platform_thread_t )t;
}

/* Blocks until all count threads have exited, then releases their handles. */

static void
platform_threads_join( platform_thread_t* threads, int count )
{
    for ( int i = 0; i < count; ++i )
    {
        if ( !threads[ i ] ) continue;
        pthread_join( *( pthread_t* )threads[ i ], NULL );
        free( threads[ i ] );
        threads[ i ] = NULL;
    }
}

/*==============================================================================================
    --- Named Cross-Process File Lock ---

    Serializes concurrent build_tool invocations targeting the same artifact using
    flock() on a per-target file in /tmp. Unlike POSIX named semaphores, flock() locks
    are kernel-tracked per file descriptor: if the process dies for any reason (crash,
    OOM kill, SIGKILL) the OS releases all locks automatically. Named semaphores have no
    abandoned state -- a crash after sem_wait leaves the semaphore locked forever.

    The lockfile persists in /tmp between runs (one empty file per target name) but is
    reused on subsequent builds; it is never truncated and carries no content.
==============================================================================================*/

/* Acquires a flock on /tmp/orb_lock_<name>, blocking until granted.
   Returns the open fd cast to void*, or NULL on failure. */

static void*
platform_lock_create( const char* name )
{
    char lock_path[ PATH_MAX ];
    snprintf( lock_path, sizeof( lock_path ), "/tmp/orb_lock_%s", name );

    int fd = open( lock_path, O_CREAT | O_RDWR, 0600 );
    if ( fd < 0 ) return NULL;
    if ( flock( fd, LOCK_EX ) != 0 ) { close( fd ); return NULL; }
    return ( void* )( intptr_t )fd;
}

/* Releases and closes a lock returned by platform_lock_create(). NULL is a safe no-op. */

static void
platform_lock_release( void* lock )
{
    if ( !lock ) return;
    int fd = ( int )( intptr_t )lock;
    flock( fd, LOCK_UN );
    close( fd );
}

// clang-format on
/*============================================================================================*/
#endif
