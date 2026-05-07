#ifndef sys_H
#define sys_H
/*==============================================================================================

    sys.h

==============================================================================================*/
#include "orb.h"
/*==============================================================================================

    sys tick : high-resolution timer and sleep function

==============================================================================================*/

void sys_tick_init( void );
void sys_tick_exit( void );

f64  sys_tick_reset( void );
f64  sys_tick_seconds( void );
i64  sys_tick_milliseconds( void );
i64  sys_tick_microseconds( void );
i64  sys_tick_nanoseconds( void );
void sys_tick_sleep( i32 milliseconds );

/*==============================================================================================

    sys library : dynamic library loading and symbol lookup

==============================================================================================*/

typedef void* lib_handle_t;

lib_handle_t  library_load( const char* path );
void*         library_get_symbol( lib_handle_t h, const char* s );
int           library_unload( lib_handle_t module );

/*==============================================================================================

    sys_file.c : file time, copy, delete, exe dir

==============================================================================================*/

uint64_t platform_time_ms( void );
uint64_t platform_file_time( const char* path );
bool     platform_copy_file( const char* src, const char* dst );
bool     platform_delete_file( const char* path );
void     platform_exe_dir( char* out, int size );

/*==============================================================================================

    sys_file_watch.c : watch a directory for file changes, /w polling notifications

==============================================================================================*/

/* Callback: Called for each file that changed.
   `filename` — bare filename, no path, valid only for the duration of the call.
   `userdata` — value passed to file_watch_poll(). */
typedef void ( *file_watch_callback_t )( const char* filename, void* userdata );

/* Start watching `dir_path`.  Returns false on failure (check file_watch_last_error). */
bool file_watch_init( const char* dir_path );

/* Drain all pending notifications, invoking `cb` for each changed file.
   Returns the number of notifications delivered (0 if nothing changed). */
int file_watch_poll( file_watch_callback_t cb, void* userdata );

/* Stop watching and release all resources. */
void file_watch_shutdown( void );

/* Human-readable description of the last error, or "" if none. */
const char* file_watch_last_error( void );

/*==============================================================================================

    System : Debug Print

==============================================================================================*/

// void sys_print( const char* msg );
// void sys_printf( const char* fmt, ... );

/*==============================================================================================

    platform_console.c

    Console input is for bootstrap tools, test hosts, and early engine loops.

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
#endif    // sys_H