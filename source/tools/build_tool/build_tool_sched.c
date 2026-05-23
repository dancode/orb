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

// Fixed upper bounds. Picked generously vs. the project's actual scale so
// we never have to grow these dynamically. Hitting either MAX_JOBS or
// MAX_REV_DEPS is a hard error — preferable to silently dropping deps.
#define MAX_JOBS       64   // Distinct targets in any single closure.
#define MAX_THREADS    32   // Worker thread cap (build_run_parallel further clamps to logical CPUs).
#define MAX_REV_DEPS   32   // Dependents per target. Inverse fan-out limit.
#define MAX_LOCAL_DEPS 32   // Deps per target captured during add_job recursion.

// Per-target scheduling state. Created once during add_job() and consulted
// by workers and the scheduler's bookkeeping passes.
typedef struct
{
    target_info_t* target;          // The thing to build.
    int            remaining_deps;  // Unfinished deps; reaches 0 → ready to run.
    int            rev_dep_count;
    int            rev_deps[ MAX_REV_DEPS ];   // Indices of jobs that depend on us.
    char           log_path[ BT_PATH_MAX ];    // Per-target build log (cl/link output).
    bool           done;
    bool           failed;
    bool           skipped;   // True when build_target short-circuited (up to date).

} sched_job_t;

// The single global scheduler state. Workers serialize their bookkeeping
// updates on `lock` and sleep on `cv` while waiting for new ready work.
typedef struct
{
    sched_job_t        jobs[ MAX_JOBS ];
    int                job_count;

    // Ready stack: indices into jobs[] for targets whose deps have all
    // finished. LIFO is fine since no caller cares about within-wave order.
    int                ready[ MAX_JOBS ];
    int                ready_count;

    int                in_flight;        // Workers currently inside build_target().
    int                total_remaining;  // Jobs not yet completed.
    bool               any_failed;       // Sticky: one failure stops new dispatches.

    CRITICAL_SECTION   lock;             // Guards every field below `jobs[]`.
    CONDITION_VARIABLE cv;               // Signaled on state changes (new ready or done).

    build_context_t*   ctx;              // Shared base context; workers clone with skip_deps=true.

} sched_t;

// Singleton scheduler instance. Reset by memset at the start of every
// build_run_parallel() call; only one parallel build runs at a time.
static sched_t           g_sched;

// TLS slot holding the active worker's log_path pointer. Read by the vcvars
// module (via sched_log_path()) to redirect child stdio. TLS_OUT_OF_INDEXES
// sentinels "scheduler not initialized yet" — sched_log_path() returns NULL
// in that case, which is exactly the "no redirection" path.
static DWORD             g_sched_log_tls    = TLS_OUT_OF_INDEXES;

// Print lock for atomic per-target log dumps. Initialized lazily on first
// build_run_parallel() call so the serial path doesn't pay for it.
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

/**
 * find_job()
 *
 * Linear search g_sched.jobs[] for an existing job with this target name.
 * Returns -1 if not yet registered. Used by add_job() to make recursive
 * insertion idempotent — a diamond dep graph still produces exactly one
 * job per target.
 */
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
              "%s\\%s\\%s\\_build.log", g_build_dir, g_int_dir, t->name );

    // We do this in two passes so that during dep recursion (which may call
    // back into add_job) we don't have to keep `j` consistent — easier to
    // collect indices first, then wire edges once everyone exists.
    int dep_indices[ MAX_LOCAL_DEPS ];
    int dep_count = 0;

    // Link deps (the .libs this target needs to link against).
    // Recurse first so the dep is fully registered before we record its index.
    for ( int i = 0; t->deps[ i ] && dep_count < MAX_LOCAL_DEPS; ++i )
    {
        target_info_t* dep = find_target( t->deps[ i ] );
        if ( dep )
        {
            int di = add_job( dep );
            if ( di >= 0 ) dep_indices[ dep_count++ ] = di;
        }
    }
    // Tool deps (helper executables that must exist before this target compiles,
    // but that aren't linked in — e.g. a codegen utility).
    for ( int i = 0; t->tool_deps[ i ] && dep_count < MAX_LOCAL_DEPS; ++i )
    {
        target_info_t* tool = find_target( t->tool_deps[ i ] );
        if ( tool )
        {
            int di = add_job( tool );
            if ( di >= 0 ) dep_indices[ dep_count++ ] = di;
        }
    }

    // Implicit dep: every has_reflect target needs the registered reflection
    // tool before it can be compiled. We *also* deduplicate here because the
    // user may have already listed the reflect tool explicitly under deps or
    // tool_deps. A double entry would inflate remaining_deps and the job
    // would never reach 0 → would never become ready → indefinite wait.
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

    // Re-fetch j: nested add_job() calls above may have appended to
    // g_sched.jobs[] but the array is fixed-size, so &g_sched.jobs[idx] is
    // still valid. Re-assigning here makes the index/pointer relationship
    // explicit and survives future changes that might move the array.
    j = &g_sched.jobs[ idx ];
    j->remaining_deps = dep_count;

    // Wire the reverse-dep edges: for each dep, record THIS job's index in
    // the dep's rev_deps list. When the dep finishes, worker_main walks that
    // list and decrements each dependent's remaining_deps counter. The job
    // becomes ready exactly when its last dep finishes — no global toposort
    // needed.
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

/**
 * worker_main()
 *
 * Long-running worker thread body. Each iteration:
 *   1. Acquire the scheduler lock and wait for a ready job (or for the
 *      scheduler to wind down).
 *   2. Pop a ready job, mark it in-flight, drop the lock.
 *   3. Run build_target() with skip_deps=true (we own dep ordering).
 *   4. Take the print lock and dump the per-target log atomically.
 *   5. Reacquire the scheduler lock, decrement counters, promote any
 *      dependents whose final dep just completed, broadcast on the CV.
 *
 * Termination: returns 0 when total_remaining hits 0, or when any_failed
 * is sticky-set and the ready queue has drained.
 */
static unsigned __stdcall
worker_main( void* arg )
{
    ( void )arg;

    for ( ;; )
    {
        EnterCriticalSection( &g_sched.lock );

        // --- 1. Wait for work, or for the build to wind down ---
        // Loop is correct against spurious wakeups; the predicate is the
        // "no ready work yet but more work expected" condition.
        while ( g_sched.ready_count == 0 && g_sched.total_remaining > 0 && !g_sched.any_failed )
        {
            // Cycle detection: nothing in flight, nothing ready, work left
            // means no job will ever produce a wakeup that satisfies the
            // predicate above → deadlock. Set the failure flag and let
            // the exit branch below carry us out.
            if ( g_sched.in_flight == 0 )
            {
                printf( ORB_INDENT "[orb error] dependency cycle detected in build graph (%d targets stuck)\n",
                        g_sched.total_remaining );
                g_sched.any_failed = true;
                WakeAllConditionVariable( &g_sched.cv );
                break;
            }
            SleepConditionVariableCS( &g_sched.cv, &g_sched.lock, INFINITE );
        }

        // Exit conditions: nothing left to do, or a previous failure has
        // fully drained the queue. Wake siblings so they exit too.
        if ( g_sched.total_remaining == 0
             || ( g_sched.any_failed && g_sched.ready_count == 0 ) )
        {
            LeaveCriticalSection( &g_sched.lock );
            WakeAllConditionVariable( &g_sched.cv );
            return 0;
        }

        // --- 2. Pop a ready job ---
        // LIFO via pre-decrement: ready[--ready_count]. Within-wave order
        // doesn't matter for correctness.
        int idx = g_sched.ready[ --g_sched.ready_count ];
        g_sched.in_flight++;
        sched_job_t* j = &g_sched.jobs[ idx ];
        LeaveCriticalSection( &g_sched.lock );

        // --- 3. Run the build under per-target output redirection ---
        // Truncate any stale log from a previous run so the dump below shows
        // only the current build's output.
        FILE* clr = fopen( j->log_path, "w" );
        if ( clr ) fclose( clr );

        // Install this thread's log redirect, force skip_deps so build_target
        // doesn't try to recurse — the scheduler is the sole dep authority.
        TlsSetValue( g_sched_log_tls, ( void* )j->log_path );

        build_context_t local_ctx = *g_sched.ctx;
        local_ctx.skip_deps       = true;
        bool ok                   = build_target( &local_ctx, j->target, &j->skipped );

        TlsSetValue( g_sched_log_tls, NULL );

        // --- 4. Atomically dump this target's full log to the console ---
        // The print lock guarantees no other worker's dump can interleave
        // between our header and the last line of our log.
        //
        // [orb FAILED] is always printed (never gated) — you always need to
        // know something broke. [orb compiled] / [orb skipped] are gated on ORB_OUT_TARGET_RESULT.
        //
        // The log is dumped when the target failed (to show errors/warnings)
        // OR when any compile/link detail flag is active (the log contains the
        // section output printed by build_target_compile/link).
        EnterCriticalSection( &g_print_lock );

        // Dump the per-target log before printing the result tag so the detail
        // sections (compile/link/cmd) appear first and the result reads as a footer.
        // Blank lines are buffered as "pending" and only flushed when a following
        // non-blank line arrives — this preserves intentional mid-block spacing
        // (e.g. between sections and [orb cmd]) while silently dropping trailing
        // blank lines from MSVC output so [orb compiled] runs right after content.

        bool dump_log  = !ok || ( g_out_flags & ( ORB_OUT_ANY_COMPILE | ORB_OUT_ANY_LINK | ORB_OUT_SUMMARY ) );
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
                // pending_blanks discarded here — trailing blanks are suppressed.
                fclose( lf );
            }
        }

        bool show_output = ( g_out_flags & ORB_OUT_TARGET_RESULT );
        bool show_skipped = j->skipped && ( g_out_flags & ORB_OUT_SUMMARY_COMPILE );
        if ( !ok || ( show_output || show_skipped ))
        {
            printf( ORB_INDENT "[orb %s] %s\n", !ok ? "FAILED" : j->skipped ? "skipped" : "completed", j->target->name );
        }
        fflush( stdout );
        LeaveCriticalSection( &g_print_lock );

        // --- 5. Mark completion and unblock dependents ---
        EnterCriticalSection( &g_sched.lock );
        j->done   = true;
        j->failed = !ok;
        g_sched.in_flight--;
        g_sched.total_remaining--;
        if ( !ok ) g_sched.any_failed = true;
        else
        {
            // Walk our reverse-dep edges: each dependent decrements its
            // remaining_deps; when one hits zero, push it onto the ready
            // stack so a sibling worker can pick it up.
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
            printf( ORB_INDENT "[orb error] TlsAlloc failed\n" );
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
        ensure_dir( dir_path );
    }

    if ( thread_count < 1 ) thread_count = 1;
    if ( thread_count > MAX_THREADS ) thread_count = MAX_THREADS;

    if ( g_out_flags & ORB_OUT_SCHEDULER )
        printf( ORB_INDENT "[orb parallel] %d targets, %d worker threads\n",
                g_sched.job_count, thread_count );

    HANDLE threads[ MAX_THREADS ];
    int    spawned = 0;
    for ( int i = 0; i < thread_count; ++i )
    {
        // Store into threads[spawned] so valid handles are always contiguous
        // from [0..spawned-1]; a NULL gap would cause WaitForMultipleObjects
        // to use the wrong handles and leave live workers unjoined.
        HANDLE h = ( HANDLE )_beginthreadex( NULL, 0, worker_main, NULL, 0, NULL );
        if ( h ) threads[ spawned++ ] = h;
    }

    WaitForMultipleObjects( spawned, threads, TRUE, INFINITE );
    for ( int i = 0; i < spawned; ++i ) CloseHandle( threads[ i ] );

    DeleteCriticalSection( &g_sched.lock );

    return !g_sched.any_failed;
}

/*============================================================================================*/
