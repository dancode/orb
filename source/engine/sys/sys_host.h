/*==============================================================================================

    sys_host.h - Host-only platform services: library loading, timers, file I/O, file watching,
                 threads, mutexes, semaphores, process spawning, system info, and console input.
                 Includes sys.h.

==============================================================================================*/
#ifndef SYS_HOST_H
#define SYS_HOST_H

#include "engine/sys/sys_api.h"

// clang-format off
/*==============================================================================================

    Library - Dynamic library loading and symbol lookup

==============================================================================================*/

typedef void*   lib_handle_t;

//              Load a dynamic library from the specified (os) path. NULL on failure.
lib_handle_t    sys_library_load( const char* path );

//              Get the address of a symbol exported by the library. NULL if not found.
void*           sys_library_get_symbol( lib_handle_t module, const char* name );

//              Unload the library and release resources.
void            sys_library_unload( lib_handle_t module );

/*==============================================================================================

    Tick - High-resolution timer initialisation and direct query

    Timing access through the vtable (sys()->tick_seconds() etc.) is available after
    mod_init_all(). Use these direct calls during early boot or in the host tick loop.

==============================================================================================*/

void sys_tick_init( void );
void sys_tick_exit( void );
f64  sys_tick_seconds( void );
i64  sys_tick_milliseconds( void );
i64  sys_tick_microseconds( void );
i64  sys_tick_nanoseconds( void );
void sys_sleep_milliseconds( i32 milliseconds );

/*==============================================================================================

    File - File operations and timestamps

==============================================================================================*/

void     sys_exe_dir( char* out, int size );
uint64_t sys_time_ms( void );

uint64_t sys_file_time( const char* path );
bool     sys_file_copy( const char* src, const char* dst );
bool     sys_file_delete( const char* path );

/* Callback invoked by sys_file_glob() for each matching file.
   `filename`  — bare filename only, no directory prefix; valid for the call duration.
   `full_path` — absolute path to the file; valid for the call duration.
   `userdata`  — opaque pointer passed through from sys_file_glob().
   Return true to continue iterating, false to stop early. */

typedef bool ( *sys_glob_fn )( const char* filename, const char* full_path, void* userdata );

/* Enumerate every file in `dir` whose name matches `pattern` (supports * and ? wildcards).
   Calls `cb` once per match; stops early if `cb` returns false.
   Returns the total number of files delivered to `cb`. */

int sys_file_glob( const char* dir, const char* pattern, sys_glob_fn cb, void* userdata );

/*==============================================================================================

    File Watch - Directory change polling with debounced notifications

==============================================================================================*/

/* Callback: Called for each file that changed.
   filename - bare filename, no path, valid only for the duration of the call.
   userdata - value passed to sys_filewatch_poll(). */

typedef void ( *file_watch_callback_t )( const char* filename, void* userdata );

/* Start watching `dir_path`. Returns false on failure (check sys_filewatch_last_error). */

bool sys_filewatch_init( const char* dir_path );

/* Drain all pending notifications, invoking `cb` for each changed file.
   Returns the number of notifications delivered (0 if nothing changed). */

int sys_filewatch_poll( file_watch_callback_t cb, void* userdata );

/* Stop watching and release all resources. */

void sys_filewatch_shutdown( void );

/* Human-readable description of the last error, or "" if none. */

const char* sys_filewatch_last_error( void );

/*==============================================================================================

    Thread - Create and manage threads

==============================================================================================*/

// Create a new thread running fn(arg).
// stack_bytes: 0 = platform default (typically 1-8 MB).
// Returns a valid thread_t on success; call thread_valid() to check.
thread_t thread_create( thread_fn_t fn, void* arg, usize stack_bytes );

// Returns 1 if the thread_t handle is valid (was created successfully).
b32 thread_valid( thread_t t );

// Block until thread finishes. Releases resources. Do not join a detached thread.
void thread_join( thread_t t );

// Let the thread run independently. Resources released automatically on exit.
// After detach, the thread_t handle is invalid.
void thread_detach( thread_t t );

// ID of the calling thread.
thread_id_t thread_current_id( void );

// Yield the rest of the current time slice to another ready thread.
void thread_yield( void );

// Sleep for at least ms milliseconds (may sleep longer on low-resolution systems).
void thread_sleep_ms( u32 ms );

// Set a debug name visible in the debugger/profiler (best-effort, no-op on unsupported).
void thread_set_name( thread_t t, const char* name );

/*==============================================================================================

    Mutex - Mutual exclusion lock for synchronizing access to shared resources

==============================================================================================*/

// Initialise. Must be called before any other mutex_* call.
void mutex_init( mutex_t* m );

// Free OS resources. mutex_t must be unlocked.
void mutex_destroy( mutex_t* m );

// Acquire. Blocks until available.
void mutex_lock( mutex_t* m );

// Non-blocking acquire. Returns 1 on success, 0 if already locked.
b32 mutex_try_lock( mutex_t* m );

// Release. Must be called by the thread that called mutex_lock.
void mutex_unlock( mutex_t* m );

/*==============================================================================================

    Semaphore - Counting semaphore for signalling between threads

==============================================================================================*/

// Initialise with initial count.
// count = 0: all waiters block until sema_post is called.
void sema_init( sema_t* s, u32 initial_count );

// Destroy and release OS resources.
void sema_destroy( sema_t* s );

// Decrement (block if count is 0 until another thread posts).
void sema_wait( sema_t* s );

// Non-blocking decrement. Returns 1 if decremented, 0 if count was already 0.
b32 sema_try_wait( sema_t* s );

// Increment by count (wakes count waiting threads).
void sema_post( sema_t* s, u32 count );

/*==============================================================================================

    Atomic - Platform-independent atomic operations

    Use these functions to perform lock-free thread synchronization. All operations enforce
    appropriate memory barriers to guarantee visibility and ordering across threads.

==============================================================================================*/

// Atomically increments the 32-bit integer at target.
// Returns the resulting incremented value.
i32  sys_atomic_increment( volatile i32* target );

// Atomically decrements the 32-bit integer at target.
// Returns the resulting decremented value.
i32  sys_atomic_decrement( volatile i32* target );

// Atomically adds value to target.
// Returns the original value of target prior to the addition.
i32  sys_atomic_exchange_add( volatile i32* target, i32 value );

// Atomically compares target with comperand. If they are equal, target is set to exchange.
// Returns the original value of target prior to the comparison.
i32  sys_atomic_compare_exchange( volatile i32* target, i32 exchange, i32 comperand );

// Performs a thread-safe atomic read of target enforcing read memory barriers.
i32  sys_atomic_read( volatile i32* target );

// Performs a thread-safe atomic write to target enforcing write memory barriers.
void sys_atomic_write( volatile i32* target, i32 value );


/*==============================================================================================

    Process - Synchronous child process execution with optional output capture

    Used by the hot-reload build invoker. Output capture uses a temp file to avoid
    pipe deadlocks with long build logs.

==============================================================================================*/

typedef struct sys_process_result_s
{
    bool started;         /* true if the process actually launched */
    int  exit_code;       /* process exit code (0 = success by convention) */
    f64  elapsed_seconds; /* wall time spent waiting for the child */

} sys_process_result_t;

/* Run a command synchronously. Stdout/stderr are inherited (printed to the host's console).
   `working_dir` may be NULL to inherit the caller's CWD.
   Returns false only if the process could not be launched at all. */
bool sys_process_run( const char* command_line, const char* working_dir, sys_process_result_t* result );

/* Same, but captures combined stdout+stderr into `out_buffer`. Truncated to
   `out_buffer_size - 1` bytes plus terminating NUL. `out_written` is optional. */
bool sys_process_run_capture( const char*           command_line,
                              const char*           working_dir,
                              char*                 out_buffer,
                              int                   out_buffer_size,
                              int*                  out_written,
                              sys_process_result_t* result );

/*==============================================================================================

    System Information - CPU count, process ID, local date/time

==============================================================================================*/

// Total logical CPU count (hardware threads, including hyperthreads).
u32 sys_cpu_count( void );

// Physical core count (logical / SMT factor -- approximation).
u32 sys_physical_core_count( void );

// Current process ID.
u32 sys_process_id( void );

typedef struct
{
    u32 year, month, day;     /* 1-based */
    u32 hour, minute, second; /* local time */
    u32 millisecond;
} SysDateTime;

void sys_datetime_local( SysDateTime* dt );

/*==============================================================================================
    Console Input — Simple key polling for bootstrap tools and early engine loops.

    Distinct from app input (windowed, raw input, controllers). Useful before the
    app/window system exists.
==============================================================================================*/

typedef enum sys_key_e
{
    PLATFORM_KEY_NONE = 0,

    PLATFORM_KEY_A,  PLATFORM_KEY_B,  PLATFORM_KEY_C,  PLATFORM_KEY_D,
    PLATFORM_KEY_E,  PLATFORM_KEY_F,  PLATFORM_KEY_G,  PLATFORM_KEY_H,
    PLATFORM_KEY_I,  PLATFORM_KEY_J,  PLATFORM_KEY_K,  PLATFORM_KEY_L,
    PLATFORM_KEY_M,  PLATFORM_KEY_N,  PLATFORM_KEY_O,  PLATFORM_KEY_P,
    PLATFORM_KEY_Q,  PLATFORM_KEY_R,  PLATFORM_KEY_S,  PLATFORM_KEY_T,
    PLATFORM_KEY_U,  PLATFORM_KEY_V,  PLATFORM_KEY_W,  PLATFORM_KEY_X,
    PLATFORM_KEY_Y,  PLATFORM_KEY_Z,

    PLATFORM_KEY_0,  PLATFORM_KEY_1,  PLATFORM_KEY_2,  PLATFORM_KEY_3,
    PLATFORM_KEY_4,  PLATFORM_KEY_5,  PLATFORM_KEY_6,  PLATFORM_KEY_7,
    PLATFORM_KEY_8,  PLATFORM_KEY_9,

    PLATFORM_KEY_ESCAPE,
    PLATFORM_KEY_ENTER,
    PLATFORM_KEY_SPACE,

    PLATFORM_KEY_F1,  PLATFORM_KEY_F2,  PLATFORM_KEY_F3,  PLATFORM_KEY_F4,
    PLATFORM_KEY_F5,  PLATFORM_KEY_F6,  PLATFORM_KEY_F7,  PLATFORM_KEY_F8,
    PLATFORM_KEY_F9,  PLATFORM_KEY_F10, PLATFORM_KEY_F11, PLATFORM_KEY_F12,

    PLATFORM_KEY_COUNT

} sys_key_t;


bool sys_console_input_init( void );
void sys_console_input_shutdown( void );
void sys_console_input_poll( void ); /* call once per frame to update key state */

bool sys_key_down( sys_key_t key );     /* true while the key is currently held.    */
bool sys_key_pressed( sys_key_t key );  /* true on the up->down transition only     */
bool sys_key_released( sys_key_t key ); /* true on the down->up transition only     */


/*==============================================================================================
    Module Descriptor

    Used by the host to register the sys module:
        mod_static_load( "sys", sys_get_mod_desc() );
    or via the build-mode-transparent macro:
        mod_static( sys );
==============================================================================================*/

mod_desc_t* sys_get_mod_desc( void );

// clang-format on
/*============================================================================================*/
#endif    // SYS_HOST_H
