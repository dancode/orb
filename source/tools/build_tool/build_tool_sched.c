/*==============================================================================================

    build_tool_sched.c -- Parallel target scheduler.

    A topological worker pool that runs independent build targets concurrently.
    Each worker thread pulls a target whose dependencies have all finished,
    calls build_target() with skip_deps=true on it, and on completion releases
    any dependents whose final unresolved dep was this one.

    Threading model:
    - Workers are OS threads inside this same build_tool.exe process, not
      child processes. We already imported vcvars into our own environment
      (see build_tool_vcvars.c), and g_targets[] is immutable after startup,
      so threads share everything for free with no IPC.
    - The per-target named mutex from build_lock_target() still applies, so
      a separate build_tool.exe invocation racing the same target on the CLI
      will still serialize against this in-process worker.

    Output handling:
    - Each worker writes a per-target log file under the obj dir. While the
      worker is active, every build_run_cmd / build_run_cmd_capture_deps call
      it makes redirects to that file (via a TLS-stored path read by the
      vcvars module). When the target finishes, the worker dumps its log to
      stdout in one atomic block under a global print lock. No interleaving.

    Cycle detection:
    - If at any point in_flight==0 && ready==0 && remaining>0, the graph has
      a cycle. We abort instead of deadlocking on the condition variable.

==============================================================================================*/
#include <process.h>

#define MAX_JOBS     64
#define MAX_THREADS  32
#define MAX_REV_DEPS 32
#define MAX_LOCAL_DEPS 32

typedef struct
{
    target_info_t* target;
    int            remaining_deps;
    int            rev_dep_count;
    int            rev_deps[ MAX_REV_DEPS ];
    char           log_path[ BT_PATH_MAX ];
    bool           done;
    bool           failed;

} sched_job_t;

typedef struct
{
    sched_job_t        jobs[ MAX_JOBS ];
    int                job_count;
    int                ready[ MAX_JOBS ];
    int                ready_count;
    int                in_flight;
    int                total_remaining;
    bool               any_failed;
    CRITICAL_SECTION   lock;
    CONDITION_VARIABLE cv;
    build_context_t*   ctx;

} sched_t;

static sched_t           g_sched;
static DWORD             g_sched_log_tls    = TLS_OUT_OF_INDEXES;
static CRITICAL_SECTION  g_print_lock;
static bool              g_print_lock_inited = false;

/*============================================================================================*/
// --- TLS log path hook (consumed by build_tool_vcvars.c) ---

/**
 * sched_log_path()
 *
 * Returns the active per-thread log path, or NULL if the calling thread is
 * not inside a scheduler worker. The vcvars module reads this in
 * build_run_cmd / build_run_cmd_capture_deps to redirect child-process
 * output away from the shared stdout.
 */
const char*
sched_log_path( void )
{
    if ( g_sched_log_tls == TLS_OUT_OF_INDEXES ) return NULL;
    return ( const char* )TlsGetValue( g_sched_log_tls );
}

/*============================================================================================*/
// --- Job graph construction ---

static int
find_job( const char* name )
{
    for ( int i = 0; i < g_sched.job_count; ++i )
        if ( strcmp( g_sched.jobs[ i ].target->name, name ) == 0 ) return i;
    return -1;
}

/**
 * add_job()
 *
 * Idempotently registers a target as a job, recursing through its link and
 * tool dependencies first so the dep set is closed under reachability.
 * Returns the new (or existing) job index, or -1 on overflow.
 *
 * After children are registered we wire reverse-dep edges: each completed
 * dep notifies its dependents to decrement remaining_deps. This is what
 * lets the worker pool consume the graph in topological order without ever
 * computing an explicit sort.
 */
static int
add_job( target_info_t* t )
{
    int existing = find_job( t->name );
    if ( existing >= 0 ) return existing;
    if ( g_sched.job_count >= MAX_JOBS )
    {
        printf( "Error: scheduler job table full (MAX_JOBS=%d)\n", MAX_JOBS );
        return -1;
    }

    int idx = g_sched.job_count++;
    sched_job_t* j = &g_sched.jobs[ idx ];
    j->target         = t;
    j->remaining_deps = 0;
    j->rev_dep_count  = 0;
    j->done           = false;
    j->failed         = false;
    snprintf( j->log_path, sizeof( j->log_path ),
              "%s\\%s\\%s\\_build.log", g_build_dir, g_int_dir, t->name );

    // Collect dep indices first; wire reverse edges in a second pass below.
    int dep_indices[ MAX_LOCAL_DEPS ];
    int dep_count = 0;

    for ( int i = 0; i < t->dep_count && dep_count < MAX_LOCAL_DEPS; ++i )
    {
        for ( int k = 0; k < g_target_count; ++k )
        {
            if ( strcmp( g_targets[ k ].name, t->deps[ i ] ) == 0 )
            {
                int di = add_job( &g_targets[ k ] );
                if ( di >= 0 ) dep_indices[ dep_count++ ] = di;
                break;
            }
        }
    }
    for ( int i = 0; i < t->tool_dep_count && dep_count < MAX_LOCAL_DEPS; ++i )
    {
        for ( int k = 0; k < g_target_count; ++k )
        {
            if ( strcmp( g_targets[ k ].name, t->tool_deps[ i ] ) == 0 )
            {
                int di = add_job( &g_targets[ k ] );
                if ( di >= 0 ) dep_indices[ dep_count++ ] = di;
                break;
            }
        }
    }

    // Re-fetch j: the array is fixed-size so the pointer is still valid, but
    // be explicit about the index-vs-pointer dance to avoid surprises later.
    j = &g_sched.jobs[ idx ];
    j->remaining_deps = dep_count;

    for ( int i = 0; i < dep_count; ++i )
    {
        sched_job_t* dj = &g_sched.jobs[ dep_indices[ i ] ];
        if ( dj->rev_dep_count < MAX_REV_DEPS )
            dj->rev_deps[ dj->rev_dep_count++ ] = idx;
    }

    return idx;
}

/*============================================================================================*/
// --- Worker thread ---

static unsigned __stdcall
worker_main( void* arg )
{
    ( void )arg;

    for ( ;; )
    {
        EnterCriticalSection( &g_sched.lock );

        // Wait for work, or for the build to wind down.
        while ( g_sched.ready_count == 0 && g_sched.total_remaining > 0 && !g_sched.any_failed )
        {
            // Cycle detection: nothing in flight, nothing ready, work left → deadlock.
            if ( g_sched.in_flight == 0 )
            {
                printf( "Error: dependency cycle detected in build graph (%d targets stuck).\n",
                        g_sched.total_remaining );
                g_sched.any_failed = true;
                WakeAllConditionVariable( &g_sched.cv );
                break;
            }
            SleepConditionVariableCS( &g_sched.cv, &g_sched.lock, INFINITE );
        }

        // Exit conditions: all done, or a previous failure has fully drained.
        if ( g_sched.total_remaining == 0
             || ( g_sched.any_failed && g_sched.ready_count == 0 ) )
        {
            LeaveCriticalSection( &g_sched.lock );
            WakeAllConditionVariable( &g_sched.cv );
            return 0;
        }

        int idx = g_sched.ready[ --g_sched.ready_count ];
        g_sched.in_flight++;
        sched_job_t* j = &g_sched.jobs[ idx ];
        LeaveCriticalSection( &g_sched.lock );

        // Truncate any stale log from a previous run.
        FILE* clr = fopen( j->log_path, "w" );
        if ( clr ) fclose( clr );

        // Install this thread's log redirect, force skip_deps so build_target
        // doesn't try to recurse — the scheduler is the sole dep authority.
        TlsSetValue( g_sched_log_tls, ( void* )j->log_path );

        build_context_t local_ctx = *g_sched.ctx;
        local_ctx.skip_deps       = true;
        bool ok                   = build_target( &local_ctx, j->target );

        TlsSetValue( g_sched_log_tls, NULL );

        // Atomically dump this target's full log to the console.
        EnterCriticalSection( &g_print_lock );
        printf( "\n=== %s : %s ===\n", j->target->name, ok ? "OK" : "FAILED" );
        FILE* lf = fopen( j->log_path, "r" );
        if ( lf )
        {
            char line[ 4096 ];
            while ( fgets( line, sizeof( line ), lf ) ) fputs( line, stdout );
            fclose( lf );
        }
        fflush( stdout );
        LeaveCriticalSection( &g_print_lock );

        // Mark completion and unblock dependents.
        EnterCriticalSection( &g_sched.lock );
        j->done   = true;
        j->failed = !ok;
        g_sched.in_flight--;
        g_sched.total_remaining--;
        if ( !ok ) g_sched.any_failed = true;
        else
        {
            for ( int i = 0; i < j->rev_dep_count; ++i )
            {
                int ri = j->rev_deps[ i ];
                if ( --g_sched.jobs[ ri ].remaining_deps == 0 )
                    g_sched.ready[ g_sched.ready_count++ ] = ri;
            }
        }
        WakeAllConditionVariable( &g_sched.cv );
        LeaveCriticalSection( &g_sched.lock );
    }
}

/*============================================================================================*/
// --- Public entry ---

/**
 * build_run_parallel()
 *
 * Builds the transitive closure of `root` (or all g_targets if NULL) using
 * up to thread_count concurrent workers. Returns true if every target
 * finished successfully.
 *
 * The closure is constructed via add_job() recursion before any workers
 * spawn; after that the scheduler is purely event-driven (ready set +
 * reverse-dep edges).
 */
bool
build_run_parallel( build_context_t* ctx, target_info_t* root, int thread_count )
{
    memset( &g_sched, 0, sizeof( g_sched ) );
    g_sched.ctx = ctx;
    InitializeCriticalSection( &g_sched.lock );
    InitializeConditionVariable( &g_sched.cv );

    if ( !g_print_lock_inited )
    {
        InitializeCriticalSection( &g_print_lock );
        g_print_lock_inited = true;
    }
    if ( g_sched_log_tls == TLS_OUT_OF_INDEXES )
    {
        g_sched_log_tls = TlsAlloc();
        if ( g_sched_log_tls == TLS_OUT_OF_INDEXES )
        {
            printf( "Error: TlsAlloc failed.\n" );
            DeleteCriticalSection( &g_sched.lock );
            return false;
        }
    }

    // Build the job graph.
    if ( root )
    {
        if ( add_job( root ) < 0 )
        {
            DeleteCriticalSection( &g_sched.lock );
            return false;
        }
    }
    else
    {
        for ( int i = 0; i < g_target_count; ++i ) add_job( &g_targets[ i ] );
    }

    g_sched.total_remaining = g_sched.job_count;

    // Seed ready set with all zero-dep jobs.
    for ( int i = 0; i < g_sched.job_count; ++i )
    {
        if ( g_sched.jobs[ i ].remaining_deps == 0 )
            g_sched.ready[ g_sched.ready_count++ ] = i;
    }

    // Pre-create obj dirs so the per-target log file opens cleanly. Workers
    // would otherwise race on mkdir.
    for ( int i = 0; i < g_sched.job_count; ++i )
    {
        char dir_path[ BT_PATH_MAX ];
        snprintf( dir_path, sizeof( dir_path ), "%s\\%s\\%s",
                  g_build_dir, g_int_dir, g_sched.jobs[ i ].target->name );
        if ( _access( dir_path, 0 ) != 0 )
        {
            char mk[ BT_PATH_MAX ];
            snprintf( mk, sizeof( mk ), "mkdir %s >nul 2>nul", dir_path );
            system( mk );
        }
    }

    if ( thread_count < 1 ) thread_count = 1;
    if ( thread_count > MAX_THREADS ) thread_count = MAX_THREADS;

    printf( "\n--- Parallel build: %d targets, %d worker threads ---\n",
            g_sched.job_count, thread_count );

    HANDLE threads[ MAX_THREADS ];
    int    spawned = 0;
    for ( int i = 0; i < thread_count; ++i )
    {
        threads[ i ] = ( HANDLE )_beginthreadex( NULL, 0, worker_main, NULL, 0, NULL );
        if ( threads[ i ] ) ++spawned;
    }

    WaitForMultipleObjects( spawned, threads, TRUE, INFINITE );
    for ( int i = 0; i < spawned; ++i ) CloseHandle( threads[ i ] );

    DeleteCriticalSection( &g_sched.lock );

    return !g_sched.any_failed;
}

/*============================================================================================*/
