#ifndef CONSOLE_API_H
#define CONSOLE_API_H
/*==============================================================================================

    runtime_service/console/console_api.h -- Developer console service API struct and gateway.

    Include this in DLL .c files that call the console service through the vtable.  Host
    executables and sandboxes include console_host.h instead.

    Function groups (all called through the console() vtable):
        Frame  : frame (host loop, near input()->frame) / emit (host gui build, after on_gui)
        State  : set_open / toggle / is_open

==============================================================================================*/

#include "runtime_service/console/console.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct console_api_s
{
    /* Per-frame housekeeping -- the HOST calls this once per frame, near input()->frame():
       polls the grave/escape toggle and ticks the post-submit redraw pin (queued command
       output lands the frame AFTER Enter, so the emit is held open briefly or the retained-
       cache clean-frame skip keeps the new scrollback offscreen).  Emits NO widgets. */
    void ( *frame )( f32 dt );

    /* Emit the drop-down over viewport vp.  The HOST calls this INSIDE the gui frame build
       (after desc->on_gui, before ctx_end); a no-op while the console is closed.  vp gives
       the drawable width the drop-down spans (typically run_host_vp()). */
    void ( *emit )( f32 dt, gui_vp_t vp );

    /* Programmatic open/close + query.  Opening queues input focus and snaps the scrollback
       to the live tail; toggle flips the state; is_open reports it. */
    void ( *set_open )( bool open );
    void ( *toggle   )( void );
    bool ( *is_open  )( void );

} console_api_t;

/*============================================================================================*/

#if ( defined( BUILD_STATIC ) || defined( CONSOLE_STATIC ) ) && !defined( MOD_HOST_DYNAMIC_SERVICES )
    MOD_GATEWAY_STATIC( console_api_t, console )
    #define MOD_USE_CONSOLE     /* static: gateway returns pointer to global struct directly */
    #define MOD_FETCH_CONSOLE   true
#else
    MOD_GATEWAY_DYNAMIC( console_api_t, console )
    #define MOD_USE_CONSOLE     MOD_DEFINE_API_PTR( console_api_t, console )
    #define MOD_FETCH_CONSOLE   MOD_FETCH_API( console_api_t, console )
#endif

// clang-format on
/*============================================================================================*/
#endif    // CONSOLE_API_H
