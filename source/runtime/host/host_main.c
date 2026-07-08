/*==============================================================================================

    host_main.c -- runtime host implementation.

    Boot sequence:

        1. mod_system_init()                        -- registry online
        2. ref_wire_mod_callbacks()                 -- install hooks; no code fires yet
        3. mod_static_load( sys, ref, job, run )    -- PASSIVE: engine baseline registered
        4. load_all( desc->modules )                -- PASSIVE: every entry registered
        5. mod_init_all()                           -- pass 1: load callbacks fire in dep order (ref frames pushed, reflection live)
                                                       pass 2: init() runs in same order
        6. MOD_HOST_FETCH_API( app, rhi, render )   -- cache host-owned API ptrs
        7. window_open()                            -- when app is loaded (inferred from k_modules)
           rhi->init() + context_open()             -- when rhi is loaded
           draw->init()                             -- when draw is loaded, after rhi context
           gui->init() + viewport_open()            -- when gui is loaded, after draw init
        8. desc->on_ready()                         -- host post-init hook
        9. enter loop per desc->loop_mode
       10. run_host_shutdown()                      -- single teardown path, order reversed
    
    Load is passive on purpose. mod_static_load / mod_dynamic_load only register the
    descriptor -- they fire no callbacks and run no module code. All lifecycle execution
    is deferred to mod_init_all, which knows the dep order and fans out subscribers
    (reflection, profilers) in that order. The ref frame stack therefore matches the
    dep graph -- dependencies below, dependents above.

    The engine baseline ( sys + ref + job + run ) is loaded by the host and is always
    present regardless of what k_modules[] declares. Higher layers (core, app, rhi,
    draw, gui, render, and eventually game / editor services) are declared by the host
    descriptor.

    Frame order
    -----------
    [pump OS events]          app()->pump_events()
    [event drain]             app()->next_event() per frame: rhi()->event (swapchain resize),
                              gui()->event (input, floaters), desc->on_event (leftovers);
                              main WIN_CLOSE goes through desc->on_close_request (veto).
    [frame clock]             run_clock_update()
    [console poll]            sys, if RUN_HOST_CONSOLE
    [cmd pump]                cmd_pump() -- queued command text (console, exec, +cmdline)
    [job tick]                job()->tick()
    [host update]             desc->on_update( dt )     -- game logic, every frame, no widgets
    [gui emit]                gated on gui()->frame_begin's dirty bool (retained-cache skip):
                              ctx_begin( DEFAULT ) -> chrome shell (borderless) ->
                              desc->on_gui( dt ) -> ctx_end; frame_end seals either way.
    [gui platform sync]       gui()->viewport_update() -- when gui loaded
    [render]                  see Render paths below
    [hot-reload]              mod_check_reloads + flush, if RUN_HOST_HOT_RELOAD
    [frame pacing]            sleep or editor wait (gui wants_redraw keeps animations smooth)

    Render paths
    ------------
    render() present: render->begin_frame / draw_scene, then the host composites
                      gui()->render( vp0, render()->frame_cmd() ) when gui is live
                      (draw_scene closes the scene pass; gui opens its own LOAD pass
                      over the finished scene), then render->end_frame presents.
    render() absent, gui() present: host drives the explicit frame --
                      frame_begin / clear / gui->render / frame_end.
    gui() always:   gui->viewport_render_floaters() after the main surface (presents tear-off
                      floater windows; no-op when none are alive).

    Shutdown
    --------
    run_host_shutdown() is the single teardown path for both normal exit and every startup
    failure. It checks each piece of state and tears down only what was initialized,
    in reverse order: gui -> draw -> rhi context -> rhi -> window -> console -> mod.
    Every error path is a one-liner: log, run_host_shutdown(), return 1.

    Frame timing
    ------------
    The internal tick is integer microseconds -- sys_tick_microseconds(), QPC-backed,
    monotonic from engine init.  The host stamps it once after the event drain and hands
    it to run_clock_update(), which diffs against the previous stamp, caps, applies
    time_scale, and stamps frame_number.  Floats (dt, app_time) are derived at that
    boundary only; nothing float is ever accumulated.  Callbacks receive f32 dt
    (capped, scaled); richer timing via run()->clock() from any module depending on "run".

    Pacing runs against an absolute deadline accumulator (deadline_us += frame_us each
    frame), so Sleep()'s millisecond truncation and overshoot self-correct instead of
    drifting the period toward work + frame_ms.  A frame that overruns its whole next
    budget resyncs the deadline forward rather than spiraling to catch up.

    Per-phase timings (events, update, gui, render, work, wait, frame) are stamped
    through the loop and published every frame via run()->frame_stats().

    API slot stability
    ------------------
    mod_get_api() returns a pointer to the module's stable api_slot -- a block the
    system owns and updates in-place on every hot-reload. g_*_api_ptr cached here
    never need refreshing; the function pointers they point to are live after every
    reload flush.

==============================================================================================*/
/*==============================================================================================
    Quit flag (headless path)
==============================================================================================*/

static bool g_quit_requested = false;
static bool g_sleep_debug    = false;

void
run_host_quit( void )
{
    g_quit_requested = true;
}

bool
run_host_should_quit( void )
{
    return g_quit_requested;
}

void
run_host_sleep_debug_set( bool enabled )
{
    g_sleep_debug = enabled;
}

void
run_host_sleep_debug_toggle( void )
{
    g_sleep_debug = !g_sleep_debug;
    printf( "[host] editor sleep debug %s\n", g_sleep_debug ? "ON" : "OFF" );
}

/*==============================================================================================
    Cached engine module API pointers
==============================================================================================*/

MOD_USE_APP;
MOD_USE_RUN;

MOD_USE_RHI;
MOD_USE_RENDER;
MOD_USE_DRAW;
MOD_USE_GUI;
MOD_USE_INPUT;

/* Host-side perf HUD -- defined in host_perf.c, included after this unit in the runtime unity
   build (runtime.c).  host_perf_tick polls the toggle + folds this frame's stats (every frame);
   host_perf_active reports whether the overlay is on; host_draw_perf_content paints it (through
   draw's built-in bitmap font) inside an already-open draw pass at a render composite point. */
void host_perf_tick( void );
bool host_perf_active( void );
void host_draw_perf_content( i32 win_w, i32 win_h );

/* sys_key_t values are pinned to app_key_t for the shared range so console-input polling
   (sys_key_pressed) and windowed input (app()->key_pressed) agree on key constants.
   C5287 (newer MSVC) flags the mixed-enum comparison even through the casts; comparing
   the two tables is the whole point here, so it is silenced for this block only. */
#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 5287 )
#endif
_Static_assert( ( i32 )PLATFORM_KEY_A      == ( i32 )APP_KEY_A,      "sys/app key tables diverged" );
_Static_assert( ( i32 )PLATFORM_KEY_Z      == ( i32 )APP_KEY_Z,      "sys/app key tables diverged" );
_Static_assert( ( i32 )PLATFORM_KEY_0      == ( i32 )APP_KEY_0,      "sys/app key tables diverged" );
_Static_assert( ( i32 )PLATFORM_KEY_9      == ( i32 )APP_KEY_9,      "sys/app key tables diverged" );
_Static_assert( ( i32 )PLATFORM_KEY_F1     == ( i32 )APP_KEY_F1,     "sys/app key tables diverged" );
_Static_assert( ( i32 )PLATFORM_KEY_F12    == ( i32 )APP_KEY_F12,    "sys/app key tables diverged" );
_Static_assert( ( i32 )PLATFORM_KEY_ESCAPE == ( i32 )APP_KEY_ESCAPE, "sys/app key tables diverged" );
_Static_assert( ( i32 )PLATFORM_KEY_ENTER  == ( i32 )APP_KEY_ENTER,  "sys/app key tables diverged" );
_Static_assert( ( i32 )PLATFORM_KEY_SPACE  == ( i32 )APP_KEY_SPACE,  "sys/app key tables diverged" );
#ifdef _MSC_VER
#pragma warning( pop )
#endif

/*==============================================================================================
    Host state -- tracks what has been initialized so run_host_shutdown() tears down exactly
    what is live, in reverse order, whether called from a startup failure or normal exit.
==============================================================================================*/

static win_id_t s_win_id      = APP_WIN_INVALID;
static i32      s_ctx_id      = RHI_CTX_INVALID;
static gui_vp_t s_vp0         = GUI_VP_INVALID;
static bool     s_rhi_inited  = false;
static bool     s_draw_inited = false;
static bool     s_gui_inited  = false;
static bool     s_console     = false;

/*==============================================================================================
    Host-owned handles -- the alternative to hardcoding context 0 / viewport 0 in hosts.
    Valid from on_ready onward; sentinels when the owning service is absent.
==============================================================================================*/

win_id_t
run_host_window( void )
{
    return s_win_id;
}

i32
run_host_ctx( void )
{
    return s_ctx_id;
}

gui_vp_t
run_host_vp( void )
{
    return s_vp0;
}

/*==============================================================================================
    Perf HUD -- draw-backend composite

    Lay the bitmap-font perf HUD over the current swapchain image via a LOAD overlay pass (the
    scene / gui frame beneath survives).  A no-op unless draw is live and the overlay is on, so
    the render paths call it unconditionally at their composite point.  s_win_id gives the
    drawable size.
==============================================================================================*/

static void
host_perf_composite_draw( rhi_cmd_t cmd )
{
    if ( !s_draw_inited || !host_perf_active() || !rhi_cmd_valid( cmd ) )
        return;

    i32 dw = 0, dh = 0;
    app()->window_get_size( s_win_id, &dw, &dh );

    draw()->begin_overlay( cmd, dw, dh );
    host_draw_perf_content( dw, dh );
    draw()->end_pass();
}

/*==============================================================================================
    Shutdown -- single teardown path for both startup failures and normal exit.
    Reads host state; only tears down what is live; resets each flag after use.
==============================================================================================*/

void
run_host_shutdown( void )
{
    /* Reverse of startup: gui (floater contexts) -> draw (GPU buffers) ->
       rhi context -> rhi device -> window -> console -> mod system.
       Public for RUN_LOOP_NONE hosts, which tear down at their own call site;
       the looping modes call it internally on exit. */

    if ( s_gui_inited )                { gui()->shutdown();                     s_gui_inited   = false; }
    if ( s_draw_inited )               { draw()->shutdown();                    s_draw_inited  = false; }
    if ( s_ctx_id != RHI_CTX_INVALID ) { rhi()->context_destroy( s_ctx_id );    s_ctx_id = RHI_CTX_INVALID; }
    if ( s_rhi_inited )                { rhi()->shutdown();                     s_rhi_inited   = false; }
    if ( s_win_id != APP_WIN_INVALID ) { app()->window_close( s_win_id );       s_win_id = APP_WIN_INVALID; }
    if ( s_console )                   { sys_console_input_shutdown();          s_console = false; }

    mod_system_exit();
}

/*==============================================================================================
    Module loading
==============================================================================================*/

static bool
load_entry( const run_module_entry_t* e )
{
    return e->get_mod_desc ? mod_static_load( e->name, e->get_mod_desc() ) : mod_dynamic_load( e->name );
}

static bool
load_all( const run_module_entry_t* modules )
{
    if ( !modules )
        return true;

    for ( const run_module_entry_t* e = modules; e->name; ++e )
    {
        if ( load_entry( e ) == false )
        {
            fprintf( stderr, "[host] failed to load '%s': %s\n", e->name, mod_last_error() );
            return false;
        }
    }
    return true;
}

/*==============================================================================================
    Main entry
==============================================================================================*/

int
run_host_main( const run_host_desc_t* desc, int argc, char** argv )
{
    if ( !desc || !desc->modules )
    {
        fprintf( stderr, "[host] descriptor or module list is missing\n" );
        return 1;
    }

    /* Reset all state so run_host_shutdown() starts from a clean baseline. */
    g_quit_requested = false;
    s_win_id         = APP_WIN_INVALID;
    s_ctx_id         = RHI_CTX_INVALID;
    s_vp0            = GUI_VP_INVALID;
    s_rhi_inited     = false;
    s_draw_inited    = false;
    s_gui_inited     = false;
    s_console        = false;

    /* ---- boot --------------------------------------------------------- */

    mod_system_init();

    /* Wire ref into the module lifecycle BEFORE any module loads. The ref registry
       self-bootstraps on first touch, so this is safe -- there's no ordering dependency
       on ref.mod_init. Every subsequent load (static, dynamic, or hot-reload swap)
       auto-registers reflection through the generic callback. */

    ref_wire_mod_callbacks();

    /* Wire core into the module lifecycle the same way: cvar callback registrations get
       stamped with their owning module id, and module-owned callbacks are dropped by the
       unload hook before a DLL is released (hot reload / unload / shutdown). */

    core_wire_mod_callbacks();

    /* Engine baseline -- sys (clock + sleep), ref (reflection), job (scheduling), run (frame clock). */
    if ( !mod_static_load( "sys", sys_get_mod_desc() ) ||
         !mod_static_load( "ref", ref_get_mod_desc() ) ||
         !mod_static_load( "job", job_get_mod_desc() ) ||
         !mod_static_load( "run", run_get_mod_desc() ) )
    {
        fprintf( stderr, "[host] baseline load failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    /* Engine extented -- Load all the modules dynamically passed in to the host from the .exe */
    if ( !load_all( desc->modules )) 
    {
        mod_system_exit();
        return 1;
    }

    /* Single dep-ordered init pass. Every reflected module's init() can already query
       its own types via ref() -- the load callback pushed each frame on its way in. */
    if ( !mod_init_all() )
    {
        fprintf( stderr, "[host] mod_init_all failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    /* Route mod and app output through core's logger now that core is live. */
    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );

    /* Queue default.cfg -> config.cfg -> autoexec.cfg, then "+command arg..." groups from the
       command line (+set r_width 1920 +exec dev.cfg). They execute at the loop's first cmd_pump,
       after every module has registered. Priority tags (see cvar_load_defaults) make the queue
       order informational, not load-bearing -- command-line args are CVAR_PRI_USER, so they win
       over the configs even though those are also queued this frame. */
    cvar_load_defaults();
    cmd_queue_args( argc, argv );

    /* Give the bind system app's source-name table (keyboard + mouse + pad) so
       "bind f5 quicksave" and "bind pad_a +jump" round-trip. */
    cmd_bind_wire_names( app_key_names(), APP_SRC_COUNT );

    /* ---- cache engine module APIs ------------------------------------- */
    /*
       This TU opts into the pointer gateway for these services in every build mode
       (MOD_HOST_DYNAMIC_SERVICES in runtime.c), so each fetch returns NULL when the module
       is absent from k_modules[] -- headless hosts that don't load app, draw, or gui get
       NULL here, which is fine; the guarded paths below check it.  app's fetch is what
       drives the windowed inference.
    */
    MOD_HOST_FETCH_API( app    );
    MOD_HOST_FETCH_API( rhi    );
    MOD_HOST_FETCH_API( render );
    MOD_HOST_FETCH_API( draw   );
    MOD_HOST_FETCH_API( gui    );
    MOD_HOST_FETCH_API( input  );

    /* ---- windowed path: inferred from k_modules[] -------------------- */
    /*
       If app was declared in k_modules, app() is non-NULL here and we
       create a window. No separate flag -- the module list is the declaration.
    */
    const bool windowed   = ( app() != NULL );

    /* Borderless is honored only when gui is loaded -- gui draws the chrome shell (caption,
       sizing borders) each frame; without it the window would have no frame at all.  A host
       without gui always gets the plain platform window. */
    const bool borderless = ( desc->flags & RUN_HOST_BORDERLESS ) != 0 && gui() != NULL;

    if ( windowed )
    {
        const i32 w = desc->window_width  > 0 ? desc->window_width  : 1280;
        const i32 h = desc->window_height > 0 ? desc->window_height : 720;

        s_win_id = app()->window_open( desc->name ? desc->name : "orb", 0, 0, w, h,
                                       borderless ? APP_WIN_BORDERLESS : APP_WIN_DEFAULT );
        if ( s_win_id == APP_WIN_INVALID )
        {
            fprintf( stderr, "[host] window creation failed\n" );
            run_host_shutdown();
            return 1;
        }

        if ( rhi() )
        {
            if ( !rhi()->init() )
            {
                fprintf( stderr, "[host] rhi init failed\n" );
                run_host_shutdown();
                return 1;
            }
            s_rhi_inited = true;

            s_ctx_id = rhi()->context_open( s_win_id );
            if ( s_ctx_id == RHI_CTX_INVALID )
            {
                fprintf( stderr, "[host] rhi context_open failed\n" );
                run_host_shutdown();
                return 1;
            }

            /* bind rhi() to render() so render can drive the rhi() */
            if ( render() )
                 render()->context_register( s_ctx_id );

            /* GPU resource init -- draw and gui after the device is live. */

            if ( draw() )
            {
                if ( !draw()->init() )
                {
                    fprintf( stderr, "[host] draw init failed\n" );
                    run_host_shutdown();
                    return 1;
                }
                s_draw_inited = true;
            }

            if ( gui() )
            {
                /* Wire the optional gui service from the descriptor: feature caps must land
                   before init; the frame hooks are the sys services gui cannot link itself
                   (clock for perf/idle timing, sleep + event-wait for frame_pace) -- the host
                   supplies them but keeps ownership of the loop and its pacing. */
                const run_gui_desc_t* gd = desc->gui;

                if ( gd && gd->caps )
                    gui()->init_config_front( *gd->caps );

                if ( !gui()->init( gd ? gd->font : GUI_FONT_NONE ) )
                {
                    fprintf( stderr, "[host] gui init failed\n" );
                    run_host_shutdown();
                    return 1;
                }
                s_gui_inited = true;

                gui()->set_frame_hooks( sys_tick_seconds, sys_sleep_milliseconds,
                                        sys_wait_for_os_events_ms );
                if ( gd && gd->debug )
                    gui()->debug_enable( true );

                s_vp0 = gui()->viewport_open( s_win_id );
                if ( s_vp0 == GUI_VP_INVALID )
                {
                    fprintf( stderr, "[host] gui viewport_open failed\n" );
                    run_host_shutdown();
                    return 1;
                }
            }
        }
    }

    printf( "[host] '%s' ready\n", desc->name ? desc->name : "host" );

    /* ---- optional console input -------------------------------------- */

    const bool hot_reload   = ( desc->flags & RUN_HOST_HOT_RELOAD   ) != 0;
    const bool editor_sleep = ( desc->flags & RUN_HOST_EDITOR_SLEEP ) != 0;
    const i32  frame_ms     = desc->frame_target_ms > 0 ? desc->frame_target_ms : 16;

    if ( desc->flags & RUN_HOST_CONSOLE )
    {
        if ( !sys_console_input_init() )
            fprintf( stderr, "[host] WARNING: console input init failed\n" );
        else
            s_console = true;
    }

    /* In editor_sleep mode, bounds hot-reload check latency when the UI is idle.
       200 ms keeps reloads responsive; 500 ms for hosts with no hot-reload. */
    const i32 editor_timeout_ms = hot_reload ? 200 : 500;

    /* ---- post-init host hook ----------------------------------------- */

    if ( desc->on_ready )
         desc->on_ready();

    /* ---- caller-driven path ------------------------------------------ */

    if ( desc->loop_mode == RUN_LOOP_NONE )
        return 0;

    /* ---- loop -------------------------------------------------------- */

    /* Main-surface clear color for the gui-composite render path; alpha 0 in the desc reads
       as "unset" (a cleared swapchain is always opaque) -- default dark, same rule as boot. */
    f32 gui_clear[ 4 ] = { RHI_CLEAR_DEFAULT_R, RHI_CLEAR_DEFAULT_G,
                           RHI_CLEAR_DEFAULT_B, RHI_CLEAR_DEFAULT_A };
    if ( desc->gui && desc->gui->clear[ 3 ] > 0.0f )
    {
        gui_clear[ 0 ] = desc->gui->clear[ 0 ];
        gui_clear[ 1 ] = desc->gui->clear[ 1 ];
        gui_clear[ 2 ] = desc->gui->clear[ 2 ];
        gui_clear[ 3 ] = desc->gui->clear[ 3 ];
    }

    /* Internal tick is integer microseconds; the absolute deadline accumulator keeps
       the average frame rate exact -- Sleep()'s truncation and overshoot self-correct
       against it instead of drifting the period toward work + frame_ms. */
    const i64 frame_us    = ( i64 )frame_ms * 1000;
    i64       deadline_us = sys_tick_microseconds() + frame_us;

    while ( !g_quit_requested )
    {
        run_frame_stats_t stats      = { 0 };
        i64               t_frame_us = sys_tick_microseconds();

        /* -- pump OS events (windowed) ---------------------------------- */

        if ( windowed && !app()->pump_events() )
            break;

        /* -- drain event ring ------------------------------------------ */

        /* Drain events.  rhi()->event() routes WIN_RESIZE to the matching swapchain; gui()->event()
           updates viewport sizes and handles input (text, scroll, mouse state; owned-floater
           closes are consumed here too).  on_event taps whatever is left; any WIN_CLOSE that
           survives is the main window's -- on_close_request may veto it (save prompts). */

        if ( windowed )
        {
            app_event_t ev;
            while ( app()->next_event( &ev ) )
            {
                if ( rhi() ) rhi()->event( &ev );
                if ( gui() && gui()->event( &ev ) )
                    continue;
                if ( desc->on_event && desc->on_event( &ev ) )
                    continue;

                /* Digital edges nobody consumed reach the bind system (auto-repeat filtered:
                   binds fire once per physical press).  KEY events already carry unified
                   source codes (keyboard + pad buttons); mouse buttons translate to their
                   APP_SRC_MOUSE* codes here, and wheel notches fire as down+up pulses on
                   wheelup/wheeldown (a notch has no held state).  Bound commands queue into
                   the buffer and run at this frame's cmd_pump below. */
                if ( ev.type == APP_EV_KEY_DOWN && !ev.data.key.repeat )
                    cmd_bind_event( ( u32 )ev.data.key.key, true );
                else if ( ev.type == APP_EV_KEY_UP )
                    cmd_bind_event( ( u32 )ev.data.key.key, false );
                else if ( ev.type == APP_EV_MOUSE_DOWN )
                    cmd_bind_event( ( u32 )( APP_SRC_MOUSE1 + ev.data.mouse_btn.button ), true );
                else if ( ev.type == APP_EV_MOUSE_UP )
                    cmd_bind_event( ( u32 )( APP_SRC_MOUSE1 + ev.data.mouse_btn.button ), false );
                else if ( ev.type == APP_EV_MOUSE_WHEEL )
                {
                    /* Wheel delta is positive toward the user (scroll back/down). */
                    const u32 src = ( ev.data.mouse_wheel.delta < 0.0f ) ? APP_SRC_WHEELUP
                                                                         : APP_SRC_WHEELDOWN;
                    cmd_bind_event( src, true );
                    cmd_bind_event( src, false );
                }

                if ( ev.type == APP_EV_WIN_CLOSE )
                {
                    if ( !desc->on_close_request || desc->on_close_request() )
                        goto loop_exit;
                }
            }
        }

        /* -- frame clock ------------------------------------------------ */

        i64 t_events_us = sys_tick_microseconds();
        stats.events_us = t_events_us - t_frame_us;

        run_clock_update( ( u64 )t_events_us ); /* diffs, caps, scales, stamps frame_number */
        f32 dt = run()->clock()->dt;            /* capped + scaled -- pass to callbacks */

        /* -- console key state ------------------------------------------ */

        if ( s_console )
            sys_console_input_poll();

        /* -- command buffer pump ----------------------------------------- */

        /* Drain queued command text (console submits, exec files, +cmdline args). */
        cmd_pump();

        /* -- input action latch ------------------------------------------ */

        /* AFTER the pump on purpose: +/- edges the binds queued this frame resolve into
           this frame's pressed/released counts (the input service's ordering contract). */
        if ( input() )
             input()->frame( dt );

        /* -- job dispatcher tick --------------------------------------- */

        if ( job() )
             job()->tick();

        /* -- host update ------------------------------------------------- */

        /* Game logic, tool work -- every frame, BEFORE the gui emit so the UI reflects this
           frame's state.  Widget calls do not belong here (the emit below may be skipped on
           clean retained frames); they go in on_gui.  Can call any loaded module API or
           run_host_quit(). */

        if ( desc->on_update )
             desc->on_update( dt );

        i64 t_update_us = sys_tick_microseconds();
        stats.update_us = t_update_us - t_events_us;

        /* -- perf HUD toggle + stats fold ------------------------------- */

        /* Poll F8 and fold last frame's timings once per frame, backend-agnostic: the gui
           backend reads this state in the emit below, the draw backend at the render composite. */
        host_perf_tick();

        /* -- gui emit ---------------------------------------------------- */

        /* frame_begin snaps the IO state from the events drained above and returns frame_dirty:
           false = nothing changed, the retained geometry re-presents and the whole build is
           skipped (on_gui does not run).  On dirty frames the default context's build opens,
           the chrome shell goes first when the window is borderless (it publishes the caption
           band -- read it via viewport_caption_h(0)), then on_gui emits the host's windows.
           frame_end seals the frame either way (clean frames replay volatile widgets there). */

        if ( s_gui_inited )
        {
            if ( gui()->frame_begin( dt ) )
            {
                gui()->ctx_begin( GUI_CTX_DEFAULT );

                if ( borderless && s_vp0 != GUI_VP_INVALID )
                    gui()->viewport_shell( s_vp0, desc->name ? desc->name : "orb", GUI_WIN_NONE );

                if ( desc->on_gui )
                     desc->on_gui( dt );

                gui()->ctx_end();
            }
            gui()->frame_end();
        }

        /* -- gui platform sync ---------------------------------------- */

        /* Reconcile gui-owned floater windows with their OS windows after the
           UI build and before rendering -- the safe point to tear surfaces down.
           Destroys any floater the user has closed. */

        if ( s_gui_inited )
            gui()->viewport_update();

        i64 t_gui_us = sys_tick_microseconds();
        stats.gui_us = t_gui_us - t_update_us;

        /* -- render ------------------------------------------------------ */

        /* Three mutually exclusive paths, chosen by which services the host loaded (the same
           k_modules[] inference as boot).  Each ends with the frame presented; skipped entirely
           while minimized (no drawable surface).  The perf HUD is the same in all three:
           host_perf_composite_draw / host_draw_perf_content, drawn through draw's bitmap font --
           a no-op when the overlay is off, so the paths just call it at their composite point.

           Path A -- render present (the game path): render owns the frame.  draw_scene opens the
             scene pass (CLEAR), draws it, and CLOSES it, leaving the frame open with no pass --
             the composite point.  Over that finished scene the host lays gui()->render (the full
             UI, when gui is live) and then the perf HUD, each opening its OWN load pass on the same
             command list so what is beneath survives.  Then end_frame presents.

           Path B -- gui but no render: the host drives the frame by hand -- open it, CLEAR the
             swapchain (so gui composites over a fresh background, not last frame's image), flush
             the gui draw list, lay the perf HUD on top, present.

           Path C -- draw but no render and no gui (a minimal draw-only host): the host owns the
             whole frame -- open it, draw()->begin_pass CLEARS the surface, the perf HUD paints
             into that same pass, present.  The clear + present run every tick regardless of the
             HUD, or nothing reaches the screen. */

        bool gui_ran = ( s_gui_inited && s_vp0 != GUI_VP_INVALID );
        if ( windowed && !app()->window_is_minimized( s_win_id ) )
        {
            if ( render() )   /* -- path A: render owns the frame -- */
            {
                if ( render()->begin_frame( s_ctx_id ) )
                {
                    render()->draw_scene( s_ctx_id, dt );

                    /* Composite over the finished scene at the open-frame/no-pass point: the gui
                       UI (if live), then the perf HUD on top -- each opens its own LOAD pass. */
                    rhi_cmd_t cmd = render()->frame_cmd( s_ctx_id );

                    if ( gui_ran )
                         gui()->render( s_vp0, cmd );

                    host_perf_composite_draw( cmd );

                    render()->end_frame( s_ctx_id );
                }
            }
            else if ( gui_ran ) /* -- path B: gui, no render -- */
            {
                rhi_cmd_t cmd = rhi()->frame_begin( s_ctx_id );
                if ( rhi_cmd_valid( cmd ) )
                {
                    /* Clear so gui composites over a fresh background (not last frame). */
                    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                        .load_op  = RHI_LOAD_OP_CLEAR,
                        .store_op = RHI_STORE_OP_STORE,
                        .clear    = { gui_clear[ 0 ], gui_clear[ 1 ], gui_clear[ 2 ], gui_clear[ 3 ] },
                    }, 1, NULL );
                    rhi()->cmd_end_rendering( cmd );
                    gui()->render( s_vp0, cmd );

                    host_perf_composite_draw( cmd );

                    rhi()->frame_end( s_ctx_id );
                }
            }
            else if ( s_draw_inited )   /* -- path C: draw only, no render, no gui -- */
            {
                /* Clear the surface and paint the perf HUD through draw's built-in font --
                   the gui-less host's whole frame (see the path-C note in the header above). */
                rhi_cmd_t cmd = rhi()->frame_begin( s_ctx_id );
                if ( rhi_cmd_valid( cmd ) )
                {
                    i32 dw = 0, dh = 0;
                    app()->window_get_size( s_win_id, &dw, &dh );
                    draw()->begin_pass( cmd, dw, dh, gui_clear );
                    host_draw_perf_content( dw, dh );
                    draw()->end_pass();
                    rhi()->frame_end( s_ctx_id );
                }
            }
        }

        /* Present gui-owned floater windows (tear-off panels, tool popouts).
           Each floater drives its own rhi context frame internally. No-op when
           no floaters are alive; safe to call unconditionally. */
        if ( s_gui_inited )
            gui()->viewport_render_floaters();

        i64 t_render_us = sys_tick_microseconds();
        stats.render_us = t_render_us - t_gui_us;

        /* -- hot-reload -------------------------------------------------- */

        if ( hot_reload )
        {
            mod_check_reloads();
            mod_system_flush_reloads();
        }

        /* -- single-shot exit -------------------------------------------- */

        if ( desc->loop_mode == RUN_LOOP_ONCE )
            break;

        /* -- frame pacing ------------------------------------------------ */

        /* Game mode: sleep toward the absolute deadline, whole milliseconds only
           (Sleep granularity); the deadline accumulator below absorbs truncation and
           overshoot so the average rate holds exactly -- event pump and every other
           phase count against the budget because the deadline is absolute.
           Editor mode: block until OS input arrives, capped by editor_timeout_ms
           so hot-reload checks and other periodic work still run. */
        i64 work_end_us = sys_tick_microseconds();
        i64 remain_us   = deadline_us - work_end_us;

        stats.work_us = work_end_us - t_frame_us;

        if ( editor_sleep )
        {
            /* Run at frame_ms cadence -- rather than blocking on OS input -- while the gui has not
               SETTLED, then block until input.  "Not settled" is two things:

                 wants_redraw : an animated widget is still mid-transition (play it out smoothly).
                 frame_dirty  : this frame still emitted widgets.  This is the one the old
                                wants_redraw-only gate missed.  Retained-mode gui needs a FOLLOW-UP
                                frame after any change to re-stabilize -- a collapsed window
                                resolving its new height, a popup snapping to its measured size, a
                                click's structural effect landing via build_any_changed the NEXT
                                frame.  Blocking the instant a dirty frame finished starved those
                                follow-ups, so interactions froze until the next input nudged the
                                loop (removing RUN_HOST_EDITOR_SLEEP hid it by never blocking).

               So keep ticking until a genuinely CLEAN frame is produced (nothing left to draw),
               then block; OS input wakes it, capped by editor_timeout_ms.  Cost is at most one
               extra clean-check frame after interaction stops -- then it idles at zero cost. */
            bool gui_settling = s_gui_inited && gui() && ( gui()->wants_redraw() || gui()->frame_dirty() );
            if ( gui_settling )
            {
                if ( g_sleep_debug ) printf( "[host] settle frame  (no block)\n" );
                if ( remain_us >= 1000 )
                    sys()->sleep_milliseconds( ( i32 )( remain_us / 1000 ) );
            }
            else
            {
                if ( g_sleep_debug ) printf( "[host] editor sleep  (timeout %d ms)\n", editor_timeout_ms );
                sys()->wait_for_os_events_ms( editor_timeout_ms );
                if ( g_sleep_debug ) printf( "[host] editor wakeup (frame %llu)\n", (unsigned long long)run()->clock()->frame_number );
            }
        }
        else if ( remain_us >= 1000 )
            sys()->sleep_milliseconds( ( i32 )( remain_us / 1000 ) );

        /* Advance the deadline by exactly one period; if this frame overran the whole
           next budget (load spike, editor wait), resync forward instead of running
           back-to-back catch-up frames. */
        i64 t_end_us = sys_tick_microseconds();
        deadline_us += frame_us;
        if ( deadline_us < t_end_us )
             deadline_us = t_end_us + frame_us;

        stats.wait_us  = t_end_us - work_end_us;
        stats.frame_us = t_end_us - t_frame_us;
        run_clock_stats_submit( &stats );
    }

loop_exit:

    /* ---- shutdown ---------------------------------------------------- */

    run_host_shutdown();
    return 0;
}

/*============================================================================================*/
