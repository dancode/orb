/*==============================================================================================

    sys.h - Platform services: timers, file I/O, dynamic libraries, process spawning,
            file watching, and console input.

    sys is always statically linked into the host executable — it is never in a DLL.

==============================================================================================*/
#ifndef SYS_H
#define SYS_H

#include "orb.h"
/*==============================================================================================

    Library - Dynamic library loading and symbol lookup

==============================================================================================*/

typedef void* lib_handle_t;

lib_handle_t  sys_library_load( const char* path );
void*         sys_library_get_symbol( lib_handle_t module, const char* name );
void          sys_library_unload( lib_handle_t module );

/*==============================================================================================

    Tick - High-resolution timer and sleep

==============================================================================================*/

void sys_tick_init( void );
void sys_tick_exit( void );
f64  sys_tick_reset( void );
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

/*==============================================================================================

    File Watch - Directory change polling with debounced notifications

==============================================================================================*/

/* Callback: Called for each file that changed.
   filename - bare filename, no path, valid only for the duration of the call.
   userdata - value passed to sys_filewatch_poll(). */
typedef void ( *file_watch_callback_t )( const char* filename, void* userdata );

/* Start watching `dir_path`.  Returns false on failure (check sys_filewatch_last_error). */

bool sys_filewatch_init( const char* dir_path );

/* Drain all pending notifications, invoking `cb` for each changed file.
   Returns the number of notifications delivered (0 if nothing changed). */
int sys_filewatch_poll( file_watch_callback_t cb, void* userdata );

/* Stop watching and release all resources. */
void sys_filewatch_shutdown( void );

/* Human-readable description of the last error, or "" if none. */
const char* sys_filewatch_last_error( void );

/*==============================================================================================

    Thread - Create and manage threads, with a simple cross-platform API.

==============================================================================================*/

/* thread handle  (opaque — contains a HANDLE or pthread_t)
   HANDLE     = 8 bytes on x64 Windows
   pthread_t  = 8 bytes on x64 Linux/Mac */

#define THREAD_HANDLE_BYTES 8

/* thread function signature */
typedef void ( *thread_fn_t )( void* arg );

typedef struct
{
    u8 _opaque[ THREAD_HANDLE_BYTES ];

} thread_t;

/* thread id (u64 is wide enough for all platforms) */
typedef u64 thread_id_t;

// Create a new thread running fn(arg).
// stack_bytes: 0 = platform default (typically 1–8 MB).
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

    Mutex - Create and manage threads, with a simple cross-platform API.

    CRITICAL_SECTION on x64 Windows  = 40 bytes
    pthread_mutex_t  on x64 Linux    = 40 bytes
    pthread_mutex_t  on x64 macOS    = 56 bytes
    Use 64 bytes for all — verified by _Static_assert in mutex.c.

==============================================================================================*/

#define MUTEX_BYTES 64

typedef struct
{
    u8 _opaque[ MUTEX_BYTES ];
} mutex_t;

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
    Console Input — Simple key polling for bootstrap tools and early engine loops.

    Distinct from app input (windowed, raw input, controllers). Useful before the
    app/window system exists.
==============================================================================================*/
// clang-format off

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

// clang-format on

bool sys_console_input_init( void );
void sys_console_input_shutdown( void );
void sys_console_input_poll( void ); /* call once per frame to update key state */

bool sys_key_down( sys_key_t key );     /* true while the key is currently held. */
bool sys_key_pressed( sys_key_t key );  /* true on the up→down transition only */
bool sys_key_released( sys_key_t key ); /* true on the down→up transition only */

/*============================================================================================*/
#endif    // SYS_H