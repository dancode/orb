/*==============================================================================================

    host_main.c -- runtime host implementation.

    Boot sequence:
        1. mod_system_init()                          -- registry online
        2. ref_wire_mod_callbacks()                   -- install hooks; no code fires yet
        3. mod_static_load( sys, ref, job, run )      -- PASSIVE: engine baseline registered
        4. load_all( desc->modules )                  -- PASSIVE: every entry registered
        5. mod_init_all()                             -- pass 1: load callbacks fire in dep order
                                                                 (ref frames pushed, reflection live)
                                                         pass 2: init() runs in same order
        6. MOD_HOST_FETCH_API( render, draw, gui )  -- cache host-owned API ptrs
        7. window_open()                              -- when app is loaded (inferred from k_modules)
           rhi->init() + context_create()             -- when rhi is loaded
           draw->init()                               -- when draw is loaded, after rhi context
           gui->init() + viewport_open()            -- when gui is loaded, after draw init
        8. desc->on_ready()                           -- host post-init hook
        9. enter loop per desc->loop_mode
       10. host_shutdown()                            -- single teardown path, order reversed

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
    [event drain]             app()->next_event() per frame: routes to gui()->event(),
                              handles resize (context_resize + viewport_resize) and close.
    [frame clock]             run_clock_update()
    [console poll]            sys, if RUN_HOST_CONSOLE
    [job tick]                job()->tick()
    [gui frame begin]       gui()->frame_begin + ctx_begin( DEFAULT ) -- when gui loaded
    [host update]             desc->on_update( dt )             -- builds UI and game logic
    [gui frame end]         gui()->ctx_end + frame_end      -- seals the build, when gui loaded
    [gui platform sync]     gui()->viewport_update() -- when gui loaded
    [render]                  see Render paths below
    [hot-reload]              mod_check_reloads + flush, if RUN_HOST_HOT_RELOAD
    [frame pacing]            sleep or editor wait

    Render paths
    ------------
    render() present: render->begin_frame / draw_scene / end_frame.
                      render owns gui composition (calls gui()->render inside its frame).
    render() absent, gui() present: host drives the explicit frame --
                      frame_begin / clear / gui->render / frame_end.
    gui() always:   gui->viewport_render_floaters() after the main surface (presents tear-off
                      floater windows; no-op when none are alive).

    Shutdown
    --------
    host_shutdown() is the single teardown path for both normal exit and every startup
    failure. It checks each piece of state and tears down only what was initialized,
    in reverse order: gui -> draw -> rhi context -> rhi -> window -> console -> mod.
    Every error path is a one-liner: log, host_shutdown(), return 1.

    Frame timing
    ------------
    sys()->tick_seconds() returns a stable monotonic clock from engine init.
    The host diffs two readings to compute raw dt each frame, then hands it to
    run_clock_update() which caps it, applies time_scale, and stamps frame_number.
    Callbacks receive f32 dt (capped, scaled). Richer timing is available via
    run()->clock() from any module that depends on "run".

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

/*==============================================================================================
    Host state -- tracks what has been initialized so host_shutdown() tears down exactly
    what is live, in reverse order, whether called from a startup failure or normal exit.
==============================================================================================*/

static win_id_t   s_win_id       = APP_WIN_INVALID;
static i32        s_ctx_id       = RHI_CTX_INVALID;
static gui_vp_t s_vp0          = GUI_VP_INVALID;
static bool       s_rhi_inited   = false;
static bool       s_draw_inited  = false;
static bool       s_gui_inited = false;
static bool       s_console      = false;

/*==============================================================================================
    Shutdown -- single teardown path for both startup failures and normal exit.
    Reads host state; only tears down what is live; resets each flag after use.
==============================================================================================*/

static void
host_shutdown( void )
{
    /* Reverse of startup: gui (floater contexts) -> draw (GPU buffers) ->
       rhi context -> rhi device -> window -> console -> mod system. */

    if ( s_gui_inited )              { gui()->shutdown();                   s_gui_inited = false; }
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
    UNUSED( argc );
    UNUSED( argv );

    if ( !desc || !desc->modules )
    {
        fprintf( stderr, "[host] descriptor or module list is missing\n" );
        return 1;
    }

    /* Reset all state so host_shutdown() starts from a clean baseline. */
    g_quit_requested = false;
    s_win_id         = APP_WIN_INVALID;
    s_ctx_id         = RHI_CTX_INVALID;
    s_vp0            = GUI_VP_INVALID;
    s_rhi_inited     = false;
    s_draw_inited    = false;
    s_gui_inited   = false;
    s_console        = false;

    /* ---- boot --------------------------------------------------------- */

    mod_system_init();

    /* Wire ref into the module lifecycle BEFORE any module loads. The ref registry
       self-bootstraps on first touch, so this is safe -- there's no ordering dependency
       on ref.mod_init. Every subsequent load (static, dynamic, or hot-reload swap)
       auto-registers reflection through the generic callback. */

    ref_wire_mod_callbacks();

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

    /* ---- cache engine module APIs ------------------------------------- */
    /*
       MOD_HOST_FETCH_API in static builds:  no-op -- the gateway returns the linked struct directly.
       MOD_HOST_FETCH_API in dynamic builds: populates g_*_api_ptr from the module registry.
                                         Returns NULL when the module is absent -- headless
                                         hosts that don't load draw or gui get NULL here,
                                         which is fine; the guarded paths below check it.
    */
    MOD_HOST_FETCH_API( rhi    );
    MOD_HOST_FETCH_API( render );
    MOD_HOST_FETCH_API( draw   );
    MOD_HOST_FETCH_API( gui  );

    /* ---- windowed path: inferred from k_modules[] -------------------- */
    /*
       If app was declared in k_modules, app() is non-NULL here and we
       create a window. No separate flag -- the module list is the declaration.
    */
    const bool windowed = ( app() != NULL );

    if ( windowed )
    {
        const i32 w = desc->window_width  > 0 ? desc->window_width  : 1280;
        const i32 h = desc->window_height > 0 ? desc->window_height : 720;

        s_win_id = app()->window_open( desc->name ? desc->name : "orb", 0, 0, w, h, APP_WIN_DEFAULT );
        if ( s_win_id == APP_WIN_INVALID )
        {
            fprintf( stderr, "[host] window creation failed\n" );
            host_shutdown();
            return 1;
        }

        if ( rhi() )
        {
            if ( !rhi()->init() )
            {
                fprintf( stderr, "[host] rhi init failed\n" );
                host_shutdown();
                return 1;
            }
            s_rhi_inited = true;

            s_ctx_id = rhi()->context_open( s_win_id );
            if ( s_ctx_id == RHI_CTX_INVALID )
            {
                fprintf( stderr, "[host] rhi context_open failed\n" );
                host_shutdown();
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
                    host_shutdown();
                    return 1;
                }
                s_draw_inited = true;
            }

            if ( gui() )
            {
                if ( !gui()->init( GUI_FONT_NONE ) )
                {
                    fprintf( stderr, "[host] gui init failed\n" );
                    host_shutdown();
                    return 1;
                }
                s_gui_inited = true;

                s_vp0 = gui()->viewport_open( s_win_id );
                if ( s_vp0 == GUI_VP_INVALID )
                {
                    fprintf( stderr, "[host] gui viewport_open failed\n" );
                    host_shutdown();
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

    f64 last_tick = sys()->tick_seconds();

    while ( !g_quit_requested )
    {
        /* -- pump OS events (windowed) ---------------------------------- */

        if ( windowed && !app()->pump_events() )
            break;

        /* -- drain event ring ------------------------------------------ */

        /* Drain events.  rhi()->event() routes WIN_RESIZE to the matching swapchain; gui()->event()
           updates viewport sizes and handles input (text, scroll, mouse state).  The host only
           needs to act on WIN_CLOSE for the main window -- all floater events are consumed by gui. */

        if ( windowed )
        {
            app_event_t ev;
            while ( app()->next_event( &ev ) )
            {
                if ( rhi() ) rhi()->event( &ev );
                if ( gui() && gui()->event( &ev ) )
                    continue;

                if ( ev.type == APP_EV_WIN_CLOSE )
                    goto loop_exit;
            }
        }

        /* -- frame clock ------------------------------------------------ */

        f64 now     = sys()->tick_seconds();
        f32 dt_real = ( f32 )( now - last_tick );
        last_tick   = now;

        run_clock_update( now, dt_real );           /* caps, scales, stamps frame_number */
        f32 dt = run()->clock()->dt;            /* capped + scaled -- pass to callbacks */

        /* -- console key state ------------------------------------------ */

        if ( s_console )
            sys_console_input_poll();

        /* -- job dispatcher tick --------------------------------------- */

        if ( job() )
             job()->tick();

        /* -- gui frame begin ------------------------------------------ */

        /* frame_begin snaps the IO state (mouse/keyboard) from the events drained above and binds
           the default context; call before any widget code, including on_update below.  The build
           is a balanced scope -- ctx_end + frame_end seal it after on_update emits its windows. */

        if ( s_gui_inited )
        {
            gui()->frame_begin( dt );
            gui()->ctx_begin( GUI_CTX_DEFAULT );
        }

        /* -- host update ------------------------------------------------- */

        /* Sandbox logic, game bootstrap, tool work. Lives at the top of the
           stack -- can call any loaded module API or run_host_quit(). */

        if ( desc->on_update )
             desc->on_update( dt );

        /* Seal the gui build: close the default context, then the frame. */
        if ( s_gui_inited )
        {
            gui()->ctx_end();
            gui()->frame_end();
        }

        /* -- gui platform sync ---------------------------------------- */

        /* Reconcile gui-owned floater windows with their OS windows after the
           UI build and before rendering -- the safe point to tear surfaces down.
           Destroys any floater the user has closed. */

        if ( s_gui_inited )
            gui()->viewport_update();

        /* -- render ------------------------------------------------------ */

        /* Render path A (render module present): render owns its frame and is
           responsible for compositing gui()->render() inside draw_scene or end_frame.
           Render path B (gui only, no render): host drives the frame explicitly --
           clear the surface, flush the gui draw list, then present. */

        if ( windowed && !app()->window_is_minimized( s_win_id ) )
        {
            if ( render() )
            {
                if ( render()->begin_frame( s_ctx_id ) )
                {
                    render()->draw_scene( s_ctx_id, dt );
                    render()->end_frame( s_ctx_id );
                }
            }
            else if ( s_gui_inited && s_vp0 != GUI_VP_INVALID )
            {
                rhi_cmd_t cmd = rhi()->frame_begin( s_ctx_id );
                if ( rhi_cmd_valid( cmd ) )
                {
                    /* Clear so gui composites over a fresh background (not last frame). */
                    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                        .load_op  = RHI_LOAD_OP_CLEAR,
                        .store_op = RHI_STORE_OP_STORE,
                        .clear    = { 0.05f, 0.05f, 0.08f, 1.0f },
                    }, 1, NULL );
                    rhi()->cmd_end_rendering( cmd );

                    gui()->render( s_vp0, cmd );
                    rhi()->frame_end( s_ctx_id );
                }
            }
        }

        /* Present gui-owned floater windows (tear-off panels, tool popouts).
           Each floater drives its own rhi context frame internally. No-op when
           no floaters are alive; safe to call unconditionally. */
        if ( s_gui_inited )
            gui()->viewport_render_floaters();

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

        /* Game mode: spin at the target frame rate.
           Editor mode: block until OS input arrives, capped by editor_timeout_ms
           so hot-reload checks and other periodic work still run. */
        if ( editor_sleep )
        {
            /* While any animated widget is mid-transition, skip the blocking wait and run at
               frame_ms cadence instead so the animation plays out smoothly.  Once all transitions
               settle, wants_redraw drops false and the normal editor sleep resumes. */
            bool animating = s_gui_inited && gui() && gui()->wants_redraw();
            if ( animating )
            {
                if ( g_sleep_debug ) printf( "[host] anim frame    (no sleep)\n" );
                sys()->sleep_milliseconds( frame_ms );
            }
            else
            {
                if ( g_sleep_debug ) printf( "[host] editor sleep  (timeout %d ms)\n", editor_timeout_ms );
                sys()->wait_for_os_events_ms( editor_timeout_ms );
                if ( g_sleep_debug ) printf( "[host] editor wakeup (frame %llu)\n", (unsigned long long)run()->clock()->frame_number );
            }
        }
        else
            sys()->sleep_milliseconds( frame_ms );
    }

loop_exit:

    /* ---- shutdown ---------------------------------------------------- */

    host_shutdown();
    return 0;
}

/*============================================================================================*/
