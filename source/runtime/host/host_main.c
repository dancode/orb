/*==============================================================================================

    host_main.c — runtime host implementation.

    Boot sequence:
        1. mod_system_init()                    — registry online
        2. ref_wire_mod_callbacks()              — install hooks; no code fires yet
        3. mod_static_load( sys, rs, run )      — PASSIVE: engine baseline registered
        4. load_all( desc->modules )            — PASSIVE: every entry registered
        5. mod_init_all()                       — pass 1: load callbacks fire in dep order
                                                          (rs frames pushed, reflection live)
                                                  pass 2: init() runs in same order
        6. MOD_HOST_FETCH_API( run, app, rhi, render ) — cache host-owned API ptrs
        7. window_open()                        — when app is loaded (inferred from k_modules)
        8. desc->on_ready()                     — host post-init hook
        9. enter loop per desc->loop_mode
       10. window_close() + mod_system_exit()  — exit in reverse dep order

    Load is passive on purpose. mod_static_load / mod_dynamic_load only register the
    descriptor — they fire no callbacks and run no module code. All lifecycle execution
    is deferred to mod_init_all, which knows the dep order and fans out subscribers
    (reflection, profilers) in that order. The rs frame stack therefore matches the
    dep graph — dependencies below, dependents above.

    The engine baseline ( sys + rs + run ) is loaded by the host and is always present
    regardless of what k_modules[] declares. Higher layers (core, app, rhi, render, and
    eventually game / editor services) are declared by the host descriptor.

    The loop is intentionally explicit. host.c knows the engine-level modules
    it manages (app, render) and calls them by name. It does not iterate the dep
    graph generically.

    Frame timing
    ------------
    sys()->tick_seconds() returns a stable monotonic clock from engine init.
    The host diffs two readings to compute raw dt each frame, then hands it to
    run_clock_update() which caps it, applies time_scale, and stamps frame_number.
    Callbacks receive f32 dt (capped, scaled). Richer timing is available via
    run()->clock() from any module that depends on "run".

    API slot stability
    ------------------
    mod_get_api() returns a pointer to the module's stable api_slot — a block the
    system owns and updates in-place on every hot-reload. g_app_api_ptr and
    g_render_api_ptr cached here never need refreshing; the function pointers
    they point to are live after every reload flush.

==============================================================================================*/
/*==============================================================================================
    Quit flag (headless path)
==============================================================================================*/

static bool g_quit_requested = false;

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

/*==============================================================================================
    Cached engine module API pointers
==============================================================================================*/

MOD_USE_APP;
MOD_USE_RUN;

MOD_USE_RHI;
MOD_USE_RENDER;



static win_id_t     s_win_id = APP_WIN_INVALID;
static i32          s_ctx_id = RHI_CTX_INVALID;

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

    g_quit_requested = false;
    s_win_id         = APP_WIN_INVALID;
    s_ctx_id         = RHI_CTX_INVALID;

    /* ---- boot --------------------------------------------------------- */

    mod_system_init();

    /* Wire rs into the module lifecycle BEFORE any module loads. The rs registry
       self-bootstraps on first touch, so this is safe — there's no ordering dependency
       on rs.mod_init. Every subsequent load (static, dynamic, or hot-reload swap)
       auto-registers reflection through the generic callback. */

    ref_wire_mod_callbacks();
    
    // if ( !mod_static( sys ) ) { }

    /* Engine baseline — sys (clock + sleep), rs (reflection), job (scheduling), run (frame clock). */
    if ( !mod_static_load( "sys", sys_get_mod_desc() ) ||
         !mod_static_load( "ref", ref_get_mod_desc() ) ||
         !mod_static_load( "job", job_get_mod_desc() ) ||
         !mod_static_load( "run", run_get_mod_desc() ) )


    {
        fprintf( stderr, "[host] baseline load failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    if ( !load_all( desc->modules ) )
    {
        mod_system_exit();
        return 1;
    }

    /* Single dep-ordered init pass. Every reflected module's init() can already query
       its own types via rs() — the load callback pushed each frame on its way in. */
    if ( !mod_init_all() )
    {
        fprintf( stderr, "[host] init failed: %s\n", mod_last_error() );
        mod_system_exit();
        return 1;
    }

    /* Route mod and app output through core's logger now that core is live. */
    mod_set_log_fn( core_log_fn );
    app_set_log_fn( core_log_fn );

    /* ---- cache engine module APIs ------------------------------------- */
    /*
       MOD_HOST_FETCH_API in static builds:  no-op — app() / render() return the
                                         linked struct directly.
       MOD_HOST_FETCH_API in dynamic builds: populates g_*_api_ptr from the module registry.
                                         Returns NULL when the module is absent — headless
                                         hosts that don't load app or render get NULL here,
                                         which is fine; the windowed path guards against it.
    */

    MOD_HOST_FETCH_API( render );



    /* ---- windowed path: inferred from k_modules[] -------------------- */
    /*
       If app was declared in k_modules, app() is non-NULL here and we
       create a window. No separate flag — the module list is the declaration.
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
            mod_system_exit();
            return 1;
        }

        if ( rhi() )
        {
            void* hwnd = app()->window_handle( s_win_id );

            if ( !rhi()->init() )
            {
                fprintf( stderr, "[host] rhi global init failed\n" );
                app()->window_close( s_win_id );
                mod_system_exit();
                return 1;
            }

            s_ctx_id = rhi()->context_create( s_win_id, hwnd, w, h );
            if ( s_ctx_id == RHI_CTX_INVALID )
            {
                fprintf( stderr, "[host] rhi context_create failed\n" );
                rhi()->shutdown();
                app()->window_close( s_win_id );
                mod_system_exit();
                return 1;
            }

            if ( render() )
                render()->context_register( s_ctx_id );
        }
    }

    printf( "[host] '%s' ready\n", desc->name ? desc->name : "host" );

    /* ---- optional console input -------------------------------------- */

    const bool console    = ( desc->flags & RUN_HOST_CONSOLE    ) != 0;
    const bool hot_reload = ( desc->flags & RUN_HOST_HOT_RELOAD ) != 0;
    const i32  frame_ms   = desc->frame_target_ms > 0 ? desc->frame_target_ms : 16;

    if ( console && !sys_console_input_init() )
        fprintf( stderr, "[host] WARNING: console input init failed\n" );

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

        /* -- frame clock ------------------------------------------------ */

        f64 now      = sys()->tick_seconds();
        f32 dt_real  = ( f32 )( now - last_tick );
        last_tick    = now;

        run_clock_update( now, dt_real );           /* caps, scales, stamps frame_number */
        f32 dt = run()->clock()->dt;            /* capped + scaled — pass to callbacks */

        /* -- console key state ------------------------------------------ */

        if ( console )
            sys_console_input_poll();

        /* -- job dispatcher tick --------------------------------------- */

        if ( job() )
            job()->tick();


        /* -- host update ------------------------------------------------- */

        /* Sandbox logic, game bootstrap, tool work. Lives at the top of the
           stack — can call any loaded module API or run_host_quit(). */

        if ( desc->on_update )
             desc->on_update( dt );


        /* -- render ------------------------------------------------------ */

        if ( windowed && render() )
        {
            if ( render()->begin_frame( s_ctx_id ) )
            {
                render()->draw_scene( s_ctx_id, dt );
                render()->end_frame( s_ctx_id );
            }
        }

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

        sys()->sleep_milliseconds( frame_ms );
    }

    /* ---- shutdown ---------------------------------------------------- */

    /* RHI surface teardown must happen before the window (HWND) is destroyed. */
    if ( rhi() )
    {
        if ( s_ctx_id != RHI_CTX_INVALID )
        {
            rhi()->context_destroy( s_ctx_id );
            s_ctx_id = RHI_CTX_INVALID;
        }
        rhi()->shutdown();
    }

    if ( windowed && app() && s_win_id != APP_WIN_INVALID )
        app()->window_close( s_win_id );

    if ( console )
        sys_console_input_shutdown();

    mod_system_exit();
    return 0;
}

/*============================================================================================*/
