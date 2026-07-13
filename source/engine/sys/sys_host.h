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
void sys_wait_for_os_events_ms( i32 timeout_ms );   /* block until input or timeout; editor idle sleep */

/*==============================================================================================

    File - File operations and timestamps

==============================================================================================*/

void     sys_exe_dir( char* out, int size );
uint64_t sys_time_ms( void );

uint64_t sys_file_time( const char* path );
bool     sys_file_copy( const char* src, const char* dst );
bool     sys_file_delete( const char* path );

/* Whole-file read result. `data` is a malloc'd buffer with a hidden trailing NUL byte, so
   text payloads (shaders, manifests) can be treated as a C string directly; `size` is the
   real byte count and EXCLUDES that terminator. On failure: ok=false, data=NULL, size=0.
   Release with sys_file_free() (safe to call on a failed result). */
typedef struct sys_file_data_s
{
    void* data;
    u32   size;
    bool  ok;
} sys_file_data_t;

//              Read an entire file into a fresh buffer. See sys_file_data_t for ownership.
sys_file_data_t sys_file_read_entire( const char* path );

//              Free the buffer owned by a read result and zero the struct.
void            sys_file_free( sys_file_data_t* fd );

//              Overwrite (or create) `path` with `size` bytes. Returns false on any error.
bool            sys_file_write_entire( const char* path, const void* data, u32 size );

//              True if the path names an existing file (not a directory).
bool            sys_file_exists( const char* path );

//              Byte size of `path`, or 0 if missing/empty/error (use sys_file_exists to
//              disambiguate an empty file from a missing one).
u32             sys_file_size( const char* path );

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

/* Recursively create `path` and any missing parent directories (like `mkdir -p`). Returns true
   if the directory exists on return -- whether it was created now or already present. Idempotent;
   safe to call on an existing directory. */

bool sys_dir_make( const char* path );

/* Recursively enumerate every file under `root`, descending into all subdirectories. Calls `cb`
   once per file (see sys_glob_fn) with its bare name and absolute path; subdirectories are
   traversed but never reported themselves. Stops early if `cb` returns false. Returns the total
   number of files delivered to `cb`. */

int sys_dir_walk( const char* root, sys_glob_fn cb, void* userdata );

/*==============================================================================================

    Fonts - OS-installed font lookup

==============================================================================================*/

/* Resolve an installed font's friendly name (e.g. "Cascadia Mono") to an absolute file path via
   the OS font registry. Matching is tolerant of case, spaces, punctuation, and an optional
   trailing "Regular" style word ("Cascadia Mono" matches "Cascadia Mono Regular"). Writes the
   resolved path to `out` and returns true on a match; returns false when the font is not found or
   the platform has no font registry (POSIX). */

bool sys_font_resolve_name( const char* name, char* out, int out_size );

/* Enumerate installed fonts from the OS font registry. `cb` is called once per bakeable face
   (.ttf/.otf/.ttc that resolves to a file on disk) with the friendly name (e.g. "Cascadia Mono
   Regular") as `filename` and the absolute file path as `full_path`; return false to stop early.
   Returns the number of faces delivered. Returns 0 on platforms with no font registry (POSIX). */

int sys_font_enumerate( sys_glob_fn cb, void* userdata );

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

// Atomically adds value to the 64-bit integer at target.
// Returns the original value of target prior to the addition.
i64  sys_atomic_exchange_add_64( volatile i64* target, i64 value );

// Atomically compares the 64-bit target with comperand. If equal, target is set to exchange.
// Returns the original value of target prior to the comparison.
i64  sys_atomic_compare_exchange_64( volatile i64* target, i64 exchange, i64 comperand );

// Performs a thread-safe atomic read of the 64-bit target enforcing read memory barriers.
i64  sys_atomic_read_64( volatile i64* target );

// Performs a thread-safe atomic write to the 64-bit target enforcing write memory barriers.
void sys_atomic_write_64( volatile i64* target, i64 value );


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

    Crash - Fatal exception capture: OS hook, minidump, backtrace, symbolization

    Mechanism layer only. Hosts normally use core_crash_install() (engine/core/debug/crash.h),
    which installs a report-writing callback through sys_crash_install().

==============================================================================================*/

typedef struct sys_crash_info_s
{
    u32   code;            /* OS exception code (see sys_crash_code_str) */
    void* address;         /* faulting instruction address */
    void* os_exception;    /* EXCEPTION_POINTERS* on Windows; NULL outside a crash */

} sys_crash_info_t;

/* Called on the faulting thread after an unhandled exception, before the process dies.
   Keep it allocation-free and re-entrancy-safe: the heap and other threads' locks may be
   in any state. */
typedef void ( *sys_crash_fn )( const sys_crash_info_t* info, void* user );

/* Install `cb` as the unhandled-exception hook (one per process; last install wins).
   Serialized: only the first faulting thread reports. With a debugger attached the
   exception is passed on after `cb` so the debugger still breaks at the crash site. */
void sys_crash_install( sys_crash_fn cb, void* user );

/* Write a minidump to `path`. Inside a crash callback pass its `info` so the dump records
   the faulting context; pass NULL for a live snapshot of the current process state. */
bool sys_crash_minidump( const char* path, const sys_crash_info_t* info );

/* Fill `frames` with up to `max` return addresses and return the count. With `info` the
   walk starts at the faulting instruction; with NULL it captures the calling thread. */
int sys_crash_backtrace( const sys_crash_info_t* info, void** frames, int max );

/* Resolve one address to "module!function + 0xNN  [file:line]" (best effort -- degrades to
   a raw address as symbol info thins out). Returns false when no symbol was found. */
bool sys_crash_symbolize( void* addr, char* out, int out_size );

/* Static name for an OS exception code, e.g. "ACCESS_VIOLATION". Never NULL. */
const char* sys_crash_code_str( u32 code );

/*==============================================================================================

    Socket - Non-blocking UDP sockets (types in sys.h)

    The raw datagram layer under engine/net. All sockets are non-blocking; byte-order
    conversion lives entirely inside this layer. UDP only -- game traffic never wants TCP.

==============================================================================================*/

// Start the OS socket subsystem (WSAStartup on Windows). Refcounted; pair with shutdown.
bool sys_net_init( void );

// Release the OS socket subsystem when the last refcount drops.
void sys_net_shutdown( void );

/* Open a non-blocking UDP socket bound to `bind_addr`. NULL binds 0.0.0.0 with an ephemeral
   port (client use); port 0 in a non-NULL address picks an ephemeral port on that interface.
   The address family of `bind_addr` selects IPv4 or IPv6 (one family per socket).
   Returns SYS_SOCKET_INVALID on failure. */
sys_socket_t sys_socket_open_udp( const sys_addr_t* bind_addr );

// Close the socket. Safe to call with SYS_SOCKET_INVALID.
void sys_socket_close( sys_socket_t s );

// Fetch the socket's bound address -- resolves the actual port after an ephemeral bind.
bool sys_socket_bound_addr( sys_socket_t s, sys_addr_t* out );

/* Send one datagram to `to`. Returns bytes sent, 0 if the OS send buffer was full (the
   datagram is dropped -- normal UDP behavior, treat like wire loss), or -1 on error. */
int sys_socket_send( sys_socket_t s, const sys_addr_t* to, const void* data, int size );

/* Receive one datagram. Returns its byte size, 0 if nothing is pending, or -1 on error.
   `from` (optional) receives the sender address. Datagrams larger than `cap` are dropped;
   ICMP port-unreachable resets are swallowed. */
int sys_socket_recv( sys_socket_t s, sys_addr_t* from, void* buf, int cap );

/* Parse "1.2.3.4", "1.2.3.4:5000", "::1", "fe80::1", or "[::1]:5000" into an address.
   Missing port parses as 0. Returns false on malformed input. */
bool sys_addr_parse( const char* str, sys_addr_t* out );

// Format an address as parsed above ("[...]:port" for IPv6); port omitted when 0.
void sys_addr_to_string( const sys_addr_t* a, char* out, int size );

// True when family, address bytes, and port all match.
bool sys_addr_equal( const sys_addr_t* a, const sys_addr_t* b );

// Convenience IPv4 constructor: sys_addr_ipv4( 127, 0, 0, 1, 5000 ).
sys_addr_t sys_addr_ipv4( u8 a, u8 b, u8 c, u8 d, u16 port );

/* Resolve a hostname via DNS (getaddrinfo), preferring IPv4 results. BLOCKING -- may stall
   for seconds; call from a worker thread if the caller cannot hitch. */
bool sys_addr_resolve( const char* hostname, u16 port, sys_addr_t* out );

/*==============================================================================================
    Console Input — Simple key polling for bootstrap tools and early engine loops.

    Distinct from app input (windowed, raw input, controllers). Useful before the
    app/window system exists.
==============================================================================================*/

/* Value order MUST mirror app_key_t (engine/app/app.h) for the shared range -- letters,
   digits, F-keys, then ESCAPE/ENTER/SPACE -- so the two tables agree on constants.
   run_host.c _Static_asserts the pinning. */
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

    PLATFORM_KEY_F1,  PLATFORM_KEY_F2,  PLATFORM_KEY_F3,  PLATFORM_KEY_F4,
    PLATFORM_KEY_F5,  PLATFORM_KEY_F6,  PLATFORM_KEY_F7,  PLATFORM_KEY_F8,
    PLATFORM_KEY_F9,  PLATFORM_KEY_F10, PLATFORM_KEY_F11, PLATFORM_KEY_F12,

    PLATFORM_KEY_ESCAPE,
    PLATFORM_KEY_ENTER,
    PLATFORM_KEY_SPACE,

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
