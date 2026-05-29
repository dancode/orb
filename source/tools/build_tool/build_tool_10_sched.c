/*==============================================================================================

    build_tool_10_sched.c -- Parallel target scheduler.

    A topological worker pool that runs independent build targets concurrently.
    Each worker thread pulls a target whose dependencies have all finished,
    calls build_target() with skip_deps=true on it, and on completion releases
    any dependents whose final unresolved dep was this one.

    Threading model:
      Workers are OS threads inside this same build_tool.exe process, not child
      processes. We already imported vcvars into our own environment (03_env),
      and g_targets[] is immutable after startup, so threads share everything
      with no IPC. The per-target named mutex from build_lock_target() still
      applies, so a separate build_tool.exe invocation racing the same target
      on the CLI will still serialize against this in-process worker.

    Output handling:
      Each worker writes a per-target log file under the obj dir. While the
      worker is active, every build_run_cmd / build_run_cmd_capture_includes call
      redirects to that file (via a TLS-stored path read by sched_log_path()).
      When the target finishes, the worker dumps its log to stdout in one atomic
      block under a global print lock -- no interleaving.

    Cycle detection:
      If at any point in_flight==0 && ready==0 && remaining>0, the graph has
      a cycle. We abort instead of deadlocking on the condition variable.

==============================================================================================*/
// clang-format off
 
// Fixed upper bounds. Hitting either MAX_JOBS or MAX_REV_DEPS is a hard error --
// preferable to silently dropping deps and causing scheduler races.

#define MAX_JOBS       64   // Distinct targets in any single closure.
#define MAX_THREADS    32   // Worker thread cap (further clamped to logical CPUs).
#define MAX_REV_DEPS   32   // Dependents per target. Inverse fan-out limit.
#define MAX_LOCAL_DEPS 32   // Deps per target captured during add_job recursion.

// Per-target scheduling state. Created once during add_job() and consulted
// by workers and the scheduler's bookkeeping passes.

typedef struct
{
    target_info_t*      target;                     // The thing to build.
    int                 remaining_deps;             // Unfinished deps; reaches 0 -> ready to run.
    int                 rev_dep_count;              // Number of dependents in rev_deps[].
    int                 rev_deps[ MAX_REV_DEPS ];   // Indices of jobs that depend on us.
    char                log_path[ PATH_MAX ];       // Per-target build log (cl/link output).
    bool                done;                       // True when build_target() returns (success or failure).
    bool                failed;                     // True when build_target() returns failure or any dep failed.
    bool                skipped;                    // True when build_target short-circuited.
    uint64_t            elapsed_ms;                 // Compile+link wall time; 0 when skipped.

} sched_job_t;

// The single global scheduler state. Workers serialize their bookkeeping
// updates on `lock` and sleep on `cv` while waiting for new ready work.

typedef struct
{
    sched_job_t         jobs[ MAX_JOBS ];           // Indexed by job ID. Immutable after add_job() finishes.
    int                 job_count;                  // Number of valid entries in jobs[].

    int                 ready[ MAX_JOBS ];          // LIFO stack of jobs whose deps all finished.
    int                 ready_count;                // Number of valid entries in ready[].

    int                 in_flight;                  // Workers currently inside build_target().
    int                 total_remaining;            // Jobs not yet completed.
    bool                any_failed;                 // Sticky: one failure stops new dispatches.

    platform_mutex_t    lock;                       // Guards every field below jobs[].
    platform_cond_t     cv;                         // Signaled on state changes.

    build_context_t*    ctx;                        // Shared base context; workers clone with skip_deps+skip_tool_deps=true.

} sched_t;

// Singleton scheduler instance. Reset by memset at the start of every
// build_run_parallel() call; only one parallel build runs at a time.

static sched_t           g_sched;

// TLS slot holding the active worker's log_path pointer. Read by sched_log_path()
// to redirect child stdio. PLATFORM_TLS_INVALID sentinels "not initialized" --
// sched_log_path() returns NULL in that case, which is the "no redirection" path.

static platform_tls_t    g_sched_log_tls    = PLATFORM_TLS_INVALID;

// Print lock for atomic per-target log dumps. Initialized lazily on first
// build_run_parallel() call so the serial path doesn't pay for it.

static platform_mutex_t  g_print_lock;
static bool              g_print_lock_inited = false;

/*==============================================================================================
    --- TLS Log Path Hook ---

    Returns the active per-thread log path, or NULL if the calling thread is not
    inside a scheduler worker. Called by 04_log, 05_spawn, and 08_exec to redirect
    child-process output away from the shared stdout during parallel builds.
==============================================================================================*/

const char*
sched_log_path( void )
{
    if ( !platform_tls_is_valid( g_sched_log_tls ) ) return NULL;
    return ( const char* )platform_tls_get( g_sched_log_tls );
}

/*==============================================================================================
    --- Job Graph Construction ---
==============================================================================================*/

// Linear search g_sched.jobs[] for an existing job with this target name.
// Returns -1 if not yet registered. Makes recursive add_job insertion idempotent --
// a diamond dep graph still produces exactly one job per target.

static int
find_job( const char* name )
{
    for ( int i = 0; i < g_sched.job_count; ++i )
        if ( strcmp( g_sched.jobs[ i ].target->name, name ) == 0 ) return i;
    return -1;
}

// Idempotently registers a target as a job, recursing through its link and
// tool dependencies first so the dep set is closed under reachability.
// After children are registered, wire reverse-dep edges so each completed dep
// notifies its dependents to decrement remaining_deps. Returns job index or -1 on overflow.

static int
add_job( target_info_t* t )
{
    int existing = find_job( t->name );
    if ( existing >= 0 ) return existing;
    if ( g_sched.job_count >= MAX_JOBS )
    {
        printf( ORB_INDENT "[orb error] scheduler job table full (MAX_JOBS=%d)\n", MAX_JOBS );
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
              "%s" PATH_SEP "%s" PATH_SEP "%s" PATH_SEP "_build.log", g_build_dir, g_int_dir, t->name );

    // Two-pass approach: collect dep indices first (via recursion), then wire
    // edges once everyone is registered -- avoids aliasing issues during recursion.
    int dep_indices[ MAX_LOCAL_DEPS ];
    int dep_count = 0;
    int i         = 0;

    // Link deps.
    for ( i = 0; t->deps[ i ] && dep_count < MAX_LOCAL_DEPS; ++i )
    {
        target_info_t* dep = find_target( t->deps[ i ] );
        if ( dep )
        {
            int di = add_job( dep );
            if ( di >= 0 ) dep_indices[ dep_count++ ] = di;
        }
    }
    if ( t->deps[ i ] )
    {
        printf( ORB_INDENT "[orb error] '%s' has too many link deps (MAX_LOCAL_DEPS=%d);"
                " raise MAX_LOCAL_DEPS to avoid scheduler race conditions\n",
                t->name, MAX_LOCAL_DEPS );
        return -1;
    }

    // Tool deps.
    for ( i = 0; t->tool_deps[ i ] && dep_count < MAX_LOCAL_DEPS; ++i )
    {
        target_info_t* tool = find_target( t->tool_deps[ i ] );
        if ( tool )
        {
            int di = add_job( tool );
            if ( di >= 0 ) dep_indices[ dep_count++ ] = di;
        }
    }
    if ( t->tool_deps[ i ] )
    {
        printf( ORB_INDENT "[orb error] '%s' has too many combined deps (MAX_LOCAL_DEPS=%d);"
                " raise MAX_LOCAL_DEPS to avoid scheduler race conditions\n",
                t->name, MAX_LOCAL_DEPS );
        return -1;
    }

    // Implicit reflect tool dep -- deduplicated to avoid inflating remaining_deps.
    if ( t->has_reflect && dep_count >= MAX_LOCAL_DEPS )
    {
        printf( ORB_INDENT "[orb error] '%s' dep table full (MAX_LOCAL_DEPS=%d);"
                " reflect tool dep cannot be registered -- raise MAX_LOCAL_DEPS\n",
                t->name, MAX_LOCAL_DEPS );
        return -1;
    }
    if ( t->has_reflect && dep_count < MAX_LOCAL_DEPS )
    {
        target_info_t* rt = find_reflect_tool();
        if ( rt )
        {
            int di = add_job( rt );
            if ( di >= 0 )
            {
                bool dup = false;
                for ( int d = 0; d < dep_count; ++d ) if ( dep_indices[ d ] == di ) { dup = true; break; }
                if ( !dup ) dep_indices[ dep_count++ ] = di;
            }
        }
    }

    // Re-fetch j: nested add_job() calls may have appended to g_sched.jobs[]
    // but the array is fixed-size so &g_sched.jobs[idx] is still valid.
    j = &g_sched.jobs[ idx ];
    j->remaining_deps = dep_count;

    // Wire reverse-dep edges. When a dep finishes, worker_main decrements each
    // dependent's remaining_deps. The job becomes ready when that count hits 0.
    for ( int d = 0; d < dep_count; ++d )
    {
        sched_job_t* dj = &g_sched.jobs[ dep_indices[ d ] ];
        if ( dj->rev_dep_count < MAX_REV_DEPS )
            dj->rev_deps[ dj->rev_dep_count++ ] = idx;
    }

    return idx;
}

/*==============================================================================================
    --- Worker Thread ---

    Long-running worker thread body. Each iteration:
      1. Acquire the scheduler lock and wait for a ready job (or wind-down).
      2. Pop a ready job, mark it in-flight, drop the lock.
      3. Run build_target() with skip_deps=true (scheduler owns dep ordering).
      4. Take the print lock and dump the per-target log atomically.
      5. Reacquire the scheduler lock, decrement counters, promote any
         dependents whose final dep just finished, broadcast on the CV.
==============================================================================================*/

static PLATFORM_THREAD_ENTRY
worker_main( void* arg )
{
    ( void )arg;

    for ( ;; )
    {
        platform_mutex_lock( &g_sched.lock );

        // 1. Wait for work, or for the build to wind down.
        while ( g_sched.ready_count == 0 && g_sched.total_remaining > 0 && !g_sched.any_failed )
        {
            // Cycle detection: nothing in flight, nothing ready, work remaining
            // means no job will ever produce a wakeup -> deadlock. Abort.
            if ( g_sched.in_flight == 0 )
            {
                printf( ORB_INDENT "[orb error] dependency cycle detected in build graph (%d targets stuck)\n",
                        g_sched.total_remaining );
                g_sched.any_failed = true;
                platform_cond_broadcast( &g_sched.cv );
                break;
            }
            platform_cond_wait( &g_sched.cv, &g_sched.lock );
        }

        // Exit when done or after a failure has drained the ready queue.
        if ( g_sched.total_remaining == 0
             || ( g_sched.any_failed && g_sched.ready_count == 0 ) )
        {
            platform_mutex_unlock( &g_sched.lock );
            platform_cond_broadcast( &g_sched.cv );
            return 0;
        }

        // 2. Pop a ready job (LIFO -- within-wave order doesn't matter for correctness).
        int idx = g_sched.ready[ --g_sched.ready_count ];
        g_sched.in_flight++;
        sched_job_t* j = &g_sched.jobs[ idx ];
        platform_mutex_unlock( &g_sched.lock );

        // 3. Run the build under per-target output redirection.
        // Truncate any stale log from a previous run first.
        FILE* clr = fopen( j->log_path, "w" );
        if ( clr ) fclose( clr );

        platform_tls_set( g_sched_log_tls, ( void* )j->log_path );

        build_context_t local_ctx      = *g_sched.ctx;
        local_ctx.skip_deps            = true;
        local_ctx.skip_tool_deps       = true;
        bool ok                        = build_target( &local_ctx, j->target, &j->skipped, &j->elapsed_ms );

        platform_tls_set( g_sched_log_tls, NULL );

        // 4. Atomically dump this target's full log to the console.
        // The print lock guarantees no other worker's dump can interleave
        // between our header and the last line of our log.
        platform_mutex_lock( &g_print_lock );

        // Dump the per-target log before the result tag so detail sections
        // (compile/link/cmd) appear first and the result reads as a footer.
        // Trailing blank lines are suppressed; intentional mid-block blank
        // lines are preserved.
        bool dump_log = !ok || ( g_out_flags & ( ORB_OUT_ANY_COMPILE | ORB_OUT_ANY_LINK | ORB_OUT_SUMMARY ) );
        bool had_output = false;
        if ( dump_log )
        {
            FILE* lf = fopen( j->log_path, "r" );
            if ( lf )
            {
                char line[ 4096 ];
                int  pending_blanks = 0;
                while ( fgets( line, sizeof( line ), lf ) )
                {
                    if ( is_msvc_source_echo( line ) ) continue;
                    bool is_blank = ( line[ 0 ] == '\n' || ( line[ 0 ] == '\r' && line[ 1 ] == '\n' ) );
                    if ( is_blank ) { if ( had_output ) pending_blanks++; continue; }
                    for ( int b = 0; b < pending_blanks; b++ ) fputc( '\n', stdout );
                    pending_blanks = 0;
                    fputs( line, stdout );
                    had_output = true;
                }
                fclose( lf );
            }
        }

        bool show_output  = ( g_out_flags & ORB_OUT_TARGET_RESULT );
        bool show_skipped = j->skipped && ( g_out_flags & ORB_OUT_SUMMARY_COMPILE );
        if ( !ok || show_output || show_skipped )
        {
            printf( ORB_INDENT "[orb %s] %s\n",
                    !ok ? "FAILED" : j->skipped ? "skipped" : "completed",
                    j->target->name );
        }
        fflush( stdout );
        platform_mutex_unlock( &g_print_lock );

        // 5. Mark completion and unblock dependents.
        platform_mutex_lock( &g_sched.lock );
        j->done   = true;
        j->failed = !ok;
        g_sched.in_flight--;
        g_sched.total_remaining--;
        if ( !ok ) g_sched.any_failed = true;
        else
        {
            for ( int ri = 0; ri < j->rev_dep_count; ++ri )
            {
                int rj = j->rev_deps[ ri ];
                if ( --g_sched.jobs[ rj ].remaining_deps == 0 )
                    g_sched.ready[ g_sched.ready_count++ ] = rj;
            }
        }
        platform_cond_broadcast( &g_sched.cv );
        platform_mutex_unlock( &g_sched.lock );
    }
}

/*==============================================================================================

    build_run_parallel()

    Builds the transitive closure of `root` (or all g_targets if NULL) using up to
    thread_count concurrent workers. Returns true iff all targets succeeded.

    The closure is constructed via add_job() before any workers spawn. After that
    the scheduler is purely event-driven via the ready set and reverse-dep edges.

==============================================================================================*/

bool
build_run_parallel( build_context_t* ctx, target_info_t* root, int thread_count )
{
    memset( &g_sched, 0, sizeof( g_sched ) );
    g_sched.ctx = ctx;

    // Initialize the threading primitives

    platform_mutex_init( &g_sched.lock );
    platform_cond_init( &g_sched.cv );

    if ( !g_print_lock_inited )
    {
        platform_mutex_init( &g_print_lock );
        g_print_lock_inited = true;
    }
    if ( !platform_tls_is_valid( g_sched_log_tls ) )
    {
        if ( !platform_tls_alloc( &g_sched_log_tls ) )
        {
            printf( ORB_INDENT "[orb error] TLS allocation failed\n" );
            platform_mutex_destroy( &g_sched.lock );
            return false;
        }
    }

    // Build the job graph.

    if ( root )
    {
        /* single target */

        if ( add_job( root ) < 0 )
        {
            platform_mutex_destroy( &g_sched.lock );
            return false;
        }
    }
    else
    {
        /* all local (non-imported) targets */

        for ( int i = 0; i < g_target_count; ++i )
            if ( !g_targets[ i ].is_external )
                add_job( &g_targets[ i ] );
    }

    g_sched.total_remaining = g_sched.job_count;

    // Seed the ready set with all zero-dep jobs.
    for ( int i = 0; i < g_sched.job_count; ++i )
    {
        if ( g_sched.jobs[ i ].remaining_deps == 0 )
             g_sched.ready[ g_sched.ready_count++ ] = i;
    }

    // Pre-create obj dirs so per-target log file opens cleanly. 
    // Workers would otherwise race on mkdir.

    for ( int i = 0; i < g_sched.job_count; ++i )
    {
        char dir_path[ PATH_MAX ];
        snprintf( dir_path, sizeof( dir_path ), "%s" PATH_SEP "%s" PATH_SEP "%s",
                  g_build_dir, g_int_dir, g_sched.jobs[ i ].target->name );
        ensure_dir( dir_path );
    }

    if ( thread_count < 1 ) thread_count = 1;
    if ( thread_count > MAX_THREADS ) thread_count = MAX_THREADS;

    if ( g_out_flags & ORB_OUT_SCHEDULER )
        printf( ORB_INDENT "[orb parallel] %d targets, %d worker threads\n",
                g_sched.job_count, thread_count );

    /* Spin up worker threads and wait for completion. Workers run until the ready queue is
       empty and all in-flight jobs finish, or any job fails. */

    platform_thread_t threads[ MAX_THREADS ];
    int               spawned = 0;
    for ( int i = 0; i < thread_count; ++i )
    {
        platform_thread_t h = platform_thread_create( worker_main, NULL );
        if ( h ) threads[ spawned++ ] = h;
    }

    platform_threads_join( threads, spawned );

    platform_mutex_destroy( &g_sched.lock );

    // Timing summary: sort built targets slowest-first and print a table.
    // Skipped targets (elapsed_ms == 0) are excluded -- they did no work.
    if ( g_out_flags & ORB_OUT_TIMING )
    {
        // Collect indices of targets that actually built.
        int order[ MAX_JOBS ];
        int order_count = 0;
        for ( int i = 0; i < g_sched.job_count; ++i )
            if ( g_sched.jobs[ i ].elapsed_ms > 0 )
                order[ order_count++ ] = i;

        if ( order_count > 0 )
        {
            // Insertion sort descending by elapsed_ms.
            for ( int i = 1; i < order_count; ++i )
            {
                int key = order[ i ];
                int j   = i - 1;
                while ( j >= 0 && g_sched.jobs[ order[ j ] ].elapsed_ms < g_sched.jobs[ key ].elapsed_ms )
                {
                    order[ j + 1 ] = order[ j ];
                    --j;
                }
                order[ j + 1 ] = key;
            }

            // Measure longest name for column alignment.
            int max_name = 4;
            for ( int i = 0; i < order_count; ++i )
            {
                int n = ( int )strlen( g_sched.jobs[ order[ i ] ].target->name );
                if ( n > max_name ) max_name = n;
            }

            uint64_t total_ms = 0;
            printf( ORB_BANNER "\n[orb timing]\n" );
            for ( int i = 0; i < order_count; ++i )
            {
                const sched_job_t* j = &g_sched.jobs[ order[ i ] ];
                total_ms += j->elapsed_ms;
                char t[ 16 ];
                if ( j->elapsed_ms < 1000 )
                    snprintf( t, sizeof( t ), "%llums", ( unsigned long long )j->elapsed_ms );
                else
                    snprintf( t, sizeof( t ), "%.1fs", j->elapsed_ms / 1000.0 );
                printf( ORB_INDENT "%-6s  %s\n", t, j->target->name );
            }
            char total_t[ 16 ];
            if ( total_ms < 1000 )
                snprintf( total_t, sizeof( total_t ), "%llums", ( unsigned long long )total_ms );
            else
                snprintf( total_t, sizeof( total_t ), "%.1fs", total_ms / 1000.0 );
            printf( ORB_INDENT "------\n" );
            printf( ORB_INDENT "%-6s  total\n", total_t );
            printf( "\n" );
        }
    }

    return !g_sched.any_failed;
}

/*============================================================================================*/
// clang-format on