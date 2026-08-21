/*==============================================================================================

    sandbox/gui/sb_gui_example/sb_gui_example.c -- gui feature explorer.

    The end-to-end tour of the gui system: every feature group has a demo window (ex_demos.c
    and the per-category files it includes), grouped under a main menu bar -- one menu per
    category, one checkable item per demo window.  This host is 100% gui-focused and runs the
    boot-tier easy-mode loop: gui()->boot() owns the window + render context (the borderless
    chrome shell auto-emits each frame), boot_poll/boot_present_begin/boot_present_end drive the frame.
    Rendering itself is not under test here (see sb_vulkan for that).

==============================================================================================*/

#include <stdio.h>
#include <string.h>   /* strcmp -- argv scan for -census */

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/ref/ref_host.h"
#include "engine/sys/sys_host.h"
#include "engine/app/app_host.h"
#include "engine/core/core_host.h"
#include "runtime_service/rhi/rhi_host.h"
#include "runtime_service/gui/gui_host.h"
#include "developer/dev_font/dev_font.h"   /* runtime font baker -- the gui type ramp's sizes */
#include "ex_demos.h"

// clang-format off

/* Runtime font baker for the gui font resolver -- dev_font is initialized in main before
   wiring.  Fine (FreeType) cache when fresh, else the fast stb bake + a background refine. */
static bool
ex_gui_font_bake( const char* family, u32 size_px, char* out, int n, void* user )
{
    (void)user;
    if ( !dev_font_get_ex( family, (int)size_px, DEV_FONT_FINE_IF_CACHED, NULL, out, n ) )
        return false;
    dev_font_refine_kick( family, (int)size_px, NULL );
    return true;
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    /* -census runs the scripted style-record sweep instead of the interactive explorer: every
       demo, one at a time, under every built-in theme, dumping the census per theme and exiting
       when the last one is out (ex_census.c).

       -nopal turns the style palette off for the whole run, interactive or scripted.  Pairing it
       with -census is the A/B: the same workload with and without the shared table, so the two
       dumps show what the palette reclaims -- and, more usefully, that the RECORD SET is the same
       either way.  A record present in one run and not the other is a palette bug.

       -nointern leaves the palette on but holds it to the BAKE TABLE alone, which is the
       finer A/B: it separates what the authored chrome rows cover from what the frame
       interned for itself, and the record set must again be identical across the pair. */
    bool census = false, nopal = false, nointern = false;
    for ( int i = 1; i < argc; ++i )
    {
        if ( strcmp( argv[ i ], "-census"   ) == 0 ) census   = true;
        if ( strcmp( argv[ i ], "-nopal"    ) == 0 ) nopal    = true;
        if ( strcmp( argv[ i ], "-nointern" ) == 0 ) nointern = true;
    }

    /* Load modules -- gui's full dependency set is just rhi + app (+ the engine core stack). */
    mod_system_init();
    mod_static( sys );
    mod_static( ref );
    mod_static( app );
    mod_static( core );
    mod_static( rhi );
    mod_static( gui );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "[sb_gui_example] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );
    core()->log_set_min_level( LOG_LEVEL_INFO );

    int ret_code = 1;

    /* Runtime font baker: serves any size no shipped bake matches (the type ramp's SMALL/LARGE
       roles, DPI retarget sizes, the Font Sizes demo).  Installed BEFORE boot so the boot-time
       style landing already bakes exact sizes instead of laddering to shipped neighbours. */
    dev_font_init( NULL );
    gui()->font_baker_set( ex_gui_font_bake, NULL );

    /* One-call setup: gui owns the main window + render context end to end (boot path).
       Borderless by default -- gui()->viewport_shell() is the chrome (titlebar drives OS move
       + caption buttons, borders resize) and is auto-emitted each frame; set .os_chrome = true
       to compare against the stock Win32 frame.  Default caps: every feature group compiled
       in -- this is the explorer, it needs them all. */
    i32 vp0 = gui()->boot( &( gui_boot_desc_t ){
        .title = "ORB -- gui example",
        .x = 64, .y = 64,
        .w     = 1920 + 320, .h = 1080 + 180,
        .font  = GUI_FONT_JETBRAINS,
        .clock = sys_tick_seconds,
        .sleep = sys_sleep_milliseconds,
        .wait  = sys_wait_for_os_events_ms,
        .clear = { 0.05f, 0.05f, 0.08f, 1.0f },
        .debug = true,    // gui owns the debug hotkeys + overlays (see debug_enable, gui_api.h)
    } );
    if ( vp0 == GUI_VP_INVALID )
    {
        fprintf( stderr, "[sb_gui_example] gui->boot failed\n" );
        goto shutdown;
    }

   gui()->debug_enable( true );

    if ( nopal )
    {
        gui()->debug_set_style_palette( false );
        printf( "[sb_gui_example] style palette OFF -- every window mints its own style records\n" );
    }

    if ( nointern )
    {
        gui()->debug_set_style_intern( false );
        printf( "[sb_gui_example] interning OFF -- palette holds the bake table alone\n" );
    }

    if ( census && !ex_census_start() )
        goto shutdown;

    /* Main loop -- boot_poll pumps the OS and routes events (rhi swapchain resize, gui input
       + floater lifecycle); false on quit or main-window close. */

    f32 dt = 0.0f;
    while ( gui()->boot_poll( &dt ) )
    {
        /* Build the UI -- balanced scopes; emit is skipped entirely on provably clean frames
           (frame_begin false), render then replays the preserved tessellation.  ex_frame owns
           the whole explorer: the main menu bar (one menu per feature category) plus every
           demo window toggled open through it. */
        if ( gui()->frame_begin( dt ) )
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );

            ex_frame();

            gui()->ctx_end();
        }
        gui()->frame_end();

        /* Present the main surface + every gui-owned floater (viewport reconcile, minimized
           guard, clear to the boot color -- all inside). */
        gui()->boot_present_begin( NULL );
        gui()->boot_present_end();

        /* The sweep advances AFTER the present, so the frame it just scored is fully tessellated
           and counted before the next demo opens.  It returns false on the last theme's dump. */
        if ( census && !ex_census_frame() )
            break;

        /* Frame pacing: spin at 4 ms (~250 Hz); with idle skip on block on OS input while
           the UI is static, 16 ms (~60 Hz) while a widget animation settles. */
        gui()->boot_pace ( 4, 16 );
    }

    gui()->print_mem_stats();

    ret_code = 0;

shutdown:
    if ( vp0 != GUI_VP_INVALID ) gui()->shutdown();  /* also tears down the boot window + context */
    dev_font_shutdown();
    rhi()->shutdown();                               /* no-op if boot never initialized it */
    mod_system_exit();
    return ret_code;
}

/*============================================================================================*/
// clang-format on
