#ifndef HOST_H
#define HOST_H
/*==============================================================================================

    runtime/host/host.h — single-entry runtime host for every executable in the engine.

    Every exe converges on one call:

        int main( int argc, char** argv ) {
            return rt_host_main( &k_desc, argc, argv );
        }

    The conventional game loop
    --------------------------
    rt_host.c drives a direct, named-module loop. It knows about the engine-level
    modules it manages (app, render) and calls them explicitly each frame. It does
    NOT iterate the dep graph generically — each module call is intentional.

        [pump OS events] ← app_api()->pump_events() — false = window closed
        [console poll]   ← sys, if RT_HOST_CONSOLE
        [on_update]      ← desc callback — host-level logic, game bootstrap
        [render]         ← render_api()->draw() — when render is loaded
        [hot-reload]     ← mod_check_reloads + flush, if RT_HOST_HOT_RELOAD

    Callbacks
    ---------
    on_ready  : called once after mod_init_all(), before the first frame.
                Use for HOST_FETCH_API calls and one-time setup.
    on_update : called every frame, after input, before render.
                The host's explicit update point — drives gameplay, tools, tests.
    Quit
    ----
    Windowed:  app_api()->pump_events() returning false breaks the loop.
    Headless:  rt_host_quit() sets a flag checked at the top of each frame.
    Both paths lead to the same clean shutdown: mod_system_exit() in reverse dep order.

==============================================================================================*/

#include "orb.h"
// #include "engine/mod/mod_api.h"

/*============================================================================================*/

enum    // RT_HOST_FLAGS
{
    RT_HOST_HOT_RELOAD = 1 << 0, /* poll DLL changes + flush each frame  */
    RT_HOST_CONSOLE    = 1 << 1, /* sys_console_input_init / poll / shutdown  */
};

/*============================================================================================*/

typedef enum rt_loop_mode_e
{
    RT_LOOP_NONE, /* host inits, then returns — caller drives ticks manually   */
    RT_LOOP_ONCE, /* one full tick, then exit (tools, asset processors)        */
    RT_LOOP_RUN,  /* run until rt_host_quit() or pump_events() returns false   */

} rt_loop_mode_t;

/*==============================================================================================
    Module entries
==============================================================================================*/

typedef struct mod_api_s mod_api_t;
typedef mod_api_t* ( *rt_get_mod_api_fn )( void );

typedef struct
{
    const char*       name;
    rt_get_mod_api_fn get_mod_api; /* NULL → load as DLL */

} rt_module_entry_t;

#define RT_SERVICE( n ) { #n, n##_get_mod_api }

#ifdef BUILD_STATIC
    #define RT_MODULE( n ) { #n, n##_get_mod_api }
#else
    #define RT_MODULE( n ) { #n, NULL }
#endif

/*==============================================================================================
    Host descriptor
==============================================================================================*/

typedef struct rt_host_desc_s
{
    const char*              name;            /* host name for logging and window titles */
    u32                      flags;           /* RT_HOST_* */
    rt_loop_mode_t           loop_mode;       /* determines how the main loop is driven */
    i32                      frame_target_ms; /* 0 → default 16                        */
    const rt_module_entry_t* modules;         /* null-terminated array                 */
    void ( *on_ready )( void );               /* after init, before first frame  */
    void ( *on_update )( f32 dt );            /* each frame, between input+render */

} rt_host_desc_t;

/*==============================================================================================
    API
==============================================================================================*/

int  rt_host_main( const rt_host_desc_t* desc, int argc, char** argv );
void rt_host_quit( void ); /* headless quit — sets flag, checked each frame top */
bool rt_host_should_quit( void );

/*============================================================================================*/
#endif