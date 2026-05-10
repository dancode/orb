#ifndef SYS_H
#define SYS_H
/*==============================================================================================

    sys.h : System API for platform services (except app/window and input, which are app.h)

    Platform abstraction and Tier 1 host service: always linked into the exe, never a DLL.

    Why these are direct functions, not a function-pointer struct
    -------------------------------------------------------------
    sys is consumed by the module system itself (mod.c).  mod.c needs to call file_watch
    and sys_library_load BEFORE any module has been initialized — which means the gateway
    pointer cache cannot be populated yet.  So sys is exposed as plain extern functions
    and gets statically linked into every exe alongside mod.c.

    Modules outside of mod.c rarely need to call sys directly — they go through core or
    a higher-level service.

==============================================================================================*/
#include "orb.h"
/*==============================================================================================

    Library : Dynamic library loading and symbol lookup

==============================================================================================*/

typedef void* lib_handle_t;

lib_handle_t  sys_library_load( const char* path );
void*         sys_library_get_symbol( lib_handle_t module, const char* name );
void          sys_library_unload( lib_handle_t module );

/*==============================================================================================

    Tick : High-resolution timer and sleep function

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

    File : File operations and timestamps

==============================================================================================*/

void     sys_exe_dir( char* out, int size );
uint64_t sys_time_ms( void );

uint64_t sys_file_time( const char* path );
bool     sys_file_copy( const char* src, const char* dst );
bool     sys_file_delete( const char* path );

/*==============================================================================================

    File Watch : Watch a directory for file changes, with polling notifications

==============================================================================================*/

/* Callback: Called for each file that changed.
   `filename` — bare filename, no path, valid only for the duration of the call.
   `userdata` — value passed to sys_filewatch_poll(). */
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

    Process : Spawn external processes synchronously, with optional output capture.

    Used by the runtime build invoker to call cmake, but generally useful for any
    "shell out and wait" operation. Output capture is implemented via a temp file on
    Windows to avoid pipe-deadlock with long build logs.

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

    Print :

==============================================================================================*/

// void sys_print( const char* msg );
// void sys_printf( const char* fmt, ... );

/*==============================================================================================

    Console Input : For bootstrap tools, test hosts, and early engine loops.

    It is intentionally not the same thing as app input.

    app input:
        - window-focused
        - raw input / mouse / controller / text input
        - game/editor runtime input

    sys console input:
        - console-focused
        - simple key polling
        - useful before window/app systems exist

==============================================================================================*/

#define CONFIG_CONSOLE
#ifdef CONFIG_CONSOLE

    // void sys_console_open();
    // void sys_console_close();
    // i32  sys_console_read_line( i8* buffer, i32 buffer_size );

#endif

typedef enum platform_key_t
{
    PLATFORM_KEY_NONE = 0,

    PLATFORM_KEY_A,
    PLATFORM_KEY_B,
    PLATFORM_KEY_C,
    PLATFORM_KEY_D,
    PLATFORM_KEY_E,
    PLATFORM_KEY_F,
    PLATFORM_KEY_G,
    PLATFORM_KEY_H,
    PLATFORM_KEY_I,
    PLATFORM_KEY_J,
    PLATFORM_KEY_K,
    PLATFORM_KEY_L,
    PLATFORM_KEY_M,
    PLATFORM_KEY_N,
    PLATFORM_KEY_O,
    PLATFORM_KEY_P,
    PLATFORM_KEY_Q,
    PLATFORM_KEY_R,
    PLATFORM_KEY_S,
    PLATFORM_KEY_T,
    PLATFORM_KEY_U,
    PLATFORM_KEY_V,
    PLATFORM_KEY_W,
    PLATFORM_KEY_X,
    PLATFORM_KEY_Y,
    PLATFORM_KEY_Z,

    PLATFORM_KEY_0,
    PLATFORM_KEY_1,
    PLATFORM_KEY_2,
    PLATFORM_KEY_3,
    PLATFORM_KEY_4,
    PLATFORM_KEY_5,
    PLATFORM_KEY_6,
    PLATFORM_KEY_7,
    PLATFORM_KEY_8,
    PLATFORM_KEY_9,

    PLATFORM_KEY_ESCAPE,
    PLATFORM_KEY_ENTER,
    PLATFORM_KEY_SPACE,

    PLATFORM_KEY_F1,
    PLATFORM_KEY_F2,
    PLATFORM_KEY_F3,
    PLATFORM_KEY_F4,
    PLATFORM_KEY_F5,
    PLATFORM_KEY_F6,
    PLATFORM_KEY_F7,
    PLATFORM_KEY_F8,
    PLATFORM_KEY_F9,
    PLATFORM_KEY_F10,
    PLATFORM_KEY_F11,
    PLATFORM_KEY_F12,

    PLATFORM_KEY_COUNT
} sys_key_t;

bool sys_console_input_init( void );
void sys_console_input_shutdown( void );

/* Poll once per frame / loop iteration. updates: key_down, key_pressed, key_released */
void sys_console_input_poll( void );

/* True while the key is currently held. */
bool sys_key_down( sys_key_t key );

/* True only on the transition from up -> down.
   This is what you want for one-shot commands like reload, compile, quit. */
bool sys_key_pressed( sys_key_t key );

/* True only on the transition from down -> up. */
bool sys_key_released( sys_key_t key );

/*============================================================================================*/
#endif    // SYS_H