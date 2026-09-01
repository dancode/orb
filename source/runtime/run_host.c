/*==============================================================================================

    run_host.c -- runtime host implementation (run_host_main + boot sequence + main loop).

    Boot sequence:

        1. mod_system_init()                        -- registry online
        2. ref_wire_mod_callbacks()                 -- install hooks; no code fires yet
        3. mod_static_load( <engine root libraries> )   -- PASSIVE: engine floor registered
        4. load_all( desc->modules )                -- PASSIVE: every entry registered
           mod_dynamic_load_dir( project )          -- PASSIVE: optional project DLL (desc->project_name)
        5. mod_init_all()                           -- pass 1: load callbacks fire in dep order (ref frames pushed, reflection live)
                                                       pass 2: init() runs in same order
        6. MOD_HOST_FETCH_API( rhi, render, ... )   -- cache host-owned optional-service API ptrs
        7. window_open()                            -- when RUN_HOST_WINDOWED is set (explicit host policy)
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

    The engine floor ( every root library in source/engine ) is loaded by the host and is
    always present regardless of what k_modules[] declares.  These root engine libraries are
    cheap to init and create no OS resources on load (job spawns no threads until configured,
    net opens no sockets until peer_create, app opens no window until window_open), so the
    loop dereferences them unguarded.  The higher layers with real init cost -- rhi, draw,
    gui, render, and eventually game / editor services -- are declared by the host descriptor
    and guarded with if ( svc() ) at their call sites.

    Frame order
    -----------
    [pump OS events]          app()->pump_events()
    [event drain]             app()->next_event() per frame, offered to rhi()->event (swapchain
                              resize), gui()->event (input, floaters), desc->on_event (leftovers)
                              until one answers APP_EVENT_CONSUMED; a surviving main WIN_CLOSE
                              goes through desc->on_close_request (veto).
    [frame clock]             run_clock_update()
    [console poll]            sys, if RUN_HOST_CONSOLE
    [cmd pump]                cmd_pump() -- queued command text (console, exec, +cmdline)
    [job tick]                job()->tick()
    [host update]             desc->on_update( dt )     -- game logic, every frame, no widgets
    [gui emit]                gated on gui()->frame_begin's dirty bool (retained-cache skip):
                              ctx_begin -> chrome shell (borderless) ->
                              desc->on_gui( dt ) -> ctx_end; frame_end seals either way.
    [gui platform sync]       gui()->viewport_update() -- when gui loaded
    [render]                  see Render paths below
    [hot-reload]              mod_check_reloads + flush, if RUN_HOST_HOT_RELOAD
    [frame pacing]            game: sleep toward deadline; editor: input-wakeable wait --
                              frame cadence while the gui settles, long block once clean.

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
    budget resyncs the deadline forward rather than spiraling to catch up.  The exact-
    average guarantee holds for game-mode pacing; the editor's input-wakeable waits
    trade it away deliberately -- frames cut short by input do not bank credit (the
    deadline is clamped to one period ahead), so responsiveness never mortgages a later
    animation frame.

    Per-phase timings (events, update, gui, render, work, wait, frame) are stamped
    through the loop and published every frame via run()->frame_stats().

    API slot stability
    ------------------
    mod_get_api() returns a pointer to the module's stable api_slot -- a block the
    system owns and updates in-place on every hot-reload. g_*_api_ptr cached here
    never need refreshing; the function pointers they point to are live after every
    reload flush.

==============================================================================================*/
// clang-format off
/*==============================================================================================
    Quit flag (headless path)
==============================================================================================*/

static bool g_quit_requested = false;
static bool g_sleep_debug    = false;
static bool g_realtime       = false;

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

/* Realtime gate: while true, RUN_HOST_EDITOR_SLEEP is suspended and the loop paces
   at frame_target_ms like a game host.  This is core to editor simulate/play/stop --
   a live session must tick without waiting on OS input; when it stops the loop
   returns to blocking on input.  Idempotent; hosts may re-assert it every frame. */
void
run_host_realtime_set( bool active )
{
    if ( g_realtime == active )
        return;

    g_realtime = active;
    if ( g_sleep_debug )
        printf( "[host] realtime %s\n", active ? "ON (editor sleep suspended)" : "OFF (editor sleep resumes)" );
}

bool
run_host_realtime( void )
{
    return g_realtime;
}

/*==============================================================================================
    Time-unit conversions -- the pacing loop keeps deadlines in microseconds (what
    sys_tick_microseconds yields) but the OS sleep/wait entry points and the profiler
    take milliseconds.  These name the * 1000 / / 1000 crossings so a unit change reads
    as intent rather than a bare literal.  Integer forms truncate toward zero, which is
    what the pacer wants: never over-sleep past the deadline.
==============================================================================================*/

#define US_PER_MS 1000

static inline i64  us_from_ms   ( i32 ms ) { return ( i64 )ms * US_PER_MS; }         // ms -> us
static inline i32  ms_from_us   ( i64 us ) { return ( i32 )( us / US_PER_MS ); }     // us -> ms (trunc)
static inline f64  ms_from_us_f ( i64 us ) { return ( f64 )us / ( f64 )US_PER_MS; }  // us -> ms (exact)

/*==============================================================================================
    Cached engine module API pointers
==============================================================================================*/

MOD_USE_RUN;
MOD_USE_RHI;
MOD_USE_RENDER;
MOD_USE_DRAW;
MOD_USE_GUI;
MOD_USE_INPUT;
MOD_USE_CONSOLE;

/* Host-side diagnostics -- defined in run_perf.c, included after this unit in the runtime unity
   build (run.c).  host_perf_tick polls the toggle + folds this frame's stats (every frame);
   host_perf_active reports whether the overlay is on; host_draw_perf_content paints it (through
   draw's built-in bitmap font) inside an already-open draw pass at a render composite point.
   host_prof_commands_register installs the prof_dump / prof_hitch capture verbs;
   host_prof_frame_flush drains an active capture (once per frame); host_prof_hitch_frame feeds
   the hitch monitor this frame's work time. */
void host_perf_tick( void );
bool host_perf_active( void );
void host_draw_perf_content( i32 win_w, i32 win_h );
void host_prof_commands_register( void );
void host_prof_frame_flush( void );
void host_prof_hitch_frame( f64 work_ms );

/*==============================================================================================
    Host state -- tracks what has been initialized so run_host_shutdown() tears down exactly
    what is live, in reverse order, whether called from a startup failure or normal exit.
==============================================================================================*/

static win_id_t s_win_id      = APP_WIN_INVALID;
static i32      s_ctx_id      = RHI_CTX_INVALID;
static i32 s_vp0         = GUI_VP_INVALID;
static bool     s_rhi_inited  = false;
static bool     s_draw_inited = false;
static bool     s_gui_inited  = false;
static bool     s_console     = false;

/* UI scale cvars, registered when gui is live and pushed into gui()->dpi_set each frame --
   gui deps are { rhi, app }, deliberately not core, so the cvar half of the seam lives here. */
static cvar_t*  s_cv_dpi_mode = NULL;   // ui_dpi_mode -- off / auto / manual (string enum)
static cvar_t*  s_cv_ui_scale = NULL;   // ui_scale    -- manual-mode factor

/*==============================================================================================
    gui diagnostics sink -- the seam between gui and core's log.

    gui deps are { rhi, app }, deliberately not core, so nothing inside gui can reach LOG_*; left
    unwired it writes to stdout and its diagnostics miss the ring, the log file, and the console.
    This callback is the whole of the bridge, installed before gui()->init() below.

    Straight passthrough with "%s": the message arrives already formatted, and any '%' surviving
    in it (a percentage in the draw-list stats, say) would be read as a conversion otherwise.
    The channel is "gui", so `log gui <level>` tunes this stream alone.
==============================================================================================*/

static void
run_host_gui_log( gui_log_level_t level, const char* msg, void* user )
{
    (void)user;

    log_level_t lvl = ( level == GUI_LOG_ERROR ) ? LOG_LEVEL_ERROR
                    : ( level == GUI_LOG_WARN  ) ? LOG_LEVEL_WARN
                                                 : LOG_LEVEL_INFO;

    core()->log_write( lvl, "gui", "%s", msg );
}

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

i32
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
    Main-surface clear color -- read ONLY by the host-driven render paths (B: gui composite,
    C: draw-only), where the host opens the frame and must clear the swapchain itself; path A
    never sees it (render's draw_scene owns its own clear).  desc->gui->clear overrides when
    set; alpha 0 reads as "unset" (a cleared swapchain is always opaque) -> default dark.
==============================================================================================*/

static void
host_clear_color( const run_gui_desc_t* gd, f32 out[ 4 ] )
{
    out[ 0 ] = RHI_CLEAR_DEFAULT_R;
    out[ 1 ] = RHI_CLEAR_DEFAULT_G;
    out[ 2 ] = RHI_CLEAR_DEFAULT_B;
    out[ 3 ] = RHI_CLEAR_DEFAULT_A;

    if ( gd && gd->clear[ 3 ] > 0.0f )
    {
        out[ 0 ] = gd->clear[ 0 ];
        out[ 1 ] = gd->clear[ 1 ];
        out[ 2 ] = gd->clear[ 2 ];
        out[ 3 ] = gd->clear[ 3 ];
    }
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
    g_realtime       = false;
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

    /* Engine floor -- every engine root library (source/engine), always loaded regardless of
       k_modules[].  They are cheap to init and create no OS resources on load (job spawns NO
       threads until job_configure below, net opens no sockets until peer_create, app creates
       no window until window_open).  The loop and module lifecycle dereference these
       unconditionally, so they carry no if() guard.  app being always present is why
       "windowed" is explicit host policy (RUN_HOST_WINDOWED) rather than an app()-presence
       inference.  Real cost lives one layer up in the runtime services (rhi builds a Vulkan
       device, etc.) -- those stay opt-in in k_modules[]. */
    if ( !mod_static_load( "sys",  sys_get_mod_desc() )  ||
         !mod_static_load( "ref",  ref_get_mod_desc() )  ||
         !mod_static_load( "prof", prof_get_mod_desc() ) ||
         !mod_static_load( "pack", pack_get_mod_desc() ) ||
         !mod_static_load( "fs",   fs_get_mod_desc() )   ||
         !mod_static_load( "job",  job_get_mod_desc() )  ||
         !mod_static_load( "net",  net_get_mod_desc() )  ||
         !mod_static_load( "app",  app_get_mod_desc() )  ||
         !mod_static_load( "core", core_get_mod_desc() ) ||
         !mod_static_load( "run",  run_get_mod_desc() ) )
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

    /* Optional game project -- registered after modules[] so its deps (core, game,
       render) are all present, before mod_init_all so the single dep-ordered init pass
       covers it.  Dynamic path: loaded from an external dir when given (a child project's
       bin/); the mod system hot-reloads it there like any other dynamic module.  Static
       path (ship exes): the host linked the project in and passes its descriptor --
       registered like any other static module, hot-reload a no-op.  The runtime never
       calls into it either way -- hosts drive it via mod_get_api( project_name ). */
    if ( desc->project_name && desc->project_name[ 0 ] )
    {
        bool ok = desc->project_get_mod_desc
                      ? mod_static_load( desc->project_name, desc->project_get_mod_desc() )
                      : mod_dynamic_load_dir( desc->project_name, desc->project_dir );
        if ( !ok )
        {
            fprintf( stderr, "[host] failed to load project '%s' (dir: %s): %s\n",
                     desc->project_name,
                     desc->project_dir && desc->project_dir[ 0 ] ? desc->project_dir : "<exe dir>",
                     mod_last_error() );
            mod_system_exit();
            return 1;
        }
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

    /* Size the job worker pool -- frontend policy.  desc->job_workers: 0 (the zero-init
       default) = no threads, the main thread runs jobs itself; N = N workers; -1 = auto (one
       per core less the main thread).  job is always loaded but spawns nothing until here. */
    job()->configure( desc->job_workers );

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

    /* Profiler capture verbs (prof_dump / prof_hitch) -- see run_perf.c. */
    host_prof_commands_register();

    /* ---- cache engine module APIs ------------------------------------- */
    /*
       This TU opts into the pointer gateway for these optional services in every build mode
       (MOD_HOST_DYNAMIC_SERVICES in run.c), so each fetch returns NULL when the module is
       absent from k_modules[] -- headless hosts that don't load rhi, draw, or gui get NULL
       here, which is fine; the guarded paths below check it.  app is NOT in this list: it is
       an engine-floor static (always loaded), so app() is hard-bound and never NULL.
    */
    MOD_HOST_FETCH_API( rhi    );
    MOD_HOST_FETCH_API( render );
    MOD_HOST_FETCH_API( draw   );
    MOD_HOST_FETCH_API( gui    );
    MOD_HOST_FETCH_API( input  );
    MOD_HOST_FETCH_API( console );

    /* ---- windowed path: explicit host policy ------------------------- */
    /*
       app is always loaded now, so its presence can no longer signal intent.  A host
       declares it wants a window by setting RUN_HOST_WINDOWED; headless hosts (server,
       tool) leave it clear and never open a window or pump OS events.
    */
    const bool windowed   = ( desc->flags & RUN_HOST_WINDOWED ) != 0;

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
                /* Wire the optional gui service from the descriptor: the frame hooks are the
                   sys services gui cannot link itself.  Only the clock matters on this path
                   (perf / idle timing); the sleep + event-wait hooks feed gui's own boot_pace
                   ladder, which this host does not call -- it owns the loop and its pacing,
                   reading gui's settle state through wants_redraw / frame_dirty /
                   volatile_live in the wait block below. */

                const run_gui_desc_t* gd = desc->gui;

                /* Route gui diagnostics into core's log BEFORE init(), so the init-path
                   messages (contract violations, a failed font load) land in the same ring,
                   file, and console as everything else.  gui deps are { rhi, app } -- it cannot
                   reach core itself -- so this callback is the whole of the seam.  Writing on
                   the "gui" channel gets it per-channel verbosity control for free (channels
                   auto-register on first write; the `log` console command lists them). */
                gui()->log_set_fn( run_host_gui_log, NULL );

                /* The resolver's runtime baker, when the host brought one (a dev_font_get
                   adapter).  Installed before init() so the boot resolve and the first style
                   landing can already bake exact sizes. */
                if ( gd && gd->font_baker )
                    gui()->font_baker_set( gd->font_baker, gd->font_baker_user );

                if ( !gui()->init( gd ? gd->font : GUI_FONT_NONE, gd ? gd->font_size : 0 ) )
                {
                    fprintf( stderr, "[host] gui init failed\n" );
                    run_host_shutdown();
                    return 1;
                }
                s_gui_inited = true;

                gui()->frame_set_hooks( sys_tick_seconds, sys_sleep_milliseconds,
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

                /* UI scale control.  gui defaults to AUTO (follow the monitor scale); these
                   cvars make the mode and the manual factor user-settable from the console /
                   configs, applied by the per-frame dpi_set push in the loop below. */
                {
                    static const char* k_dpi_modes[] = { "off", "auto", "manual" };
                    s_cv_dpi_mode = cvar_register_s(
                        "ui_dpi_mode", "UI response to the monitor scale: off = authored size, "
                        "auto = follow the main window's monitor, manual = apply ui_scale.",
                        k_dpi_modes, 3, 1 /* default: auto */, CVAR_ARCHIVE );
                    s_cv_ui_scale = cvar_register_f(
                        "ui_scale", "UI scale factor applied when ui_dpi_mode is manual.",
                        1.0f, 0.5f, 4.0f, CVAR_ARCHIVE );
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

    /* ---- frame loop -------------------------------------------------- */

    /* Internal tick is integer microseconds; the absolute deadline accumulator keeps
       the average frame rate exact -- Sleep()'s truncation and overshoot self-correct
       against it instead of drifting the period toward work + frame_ms. */

    const i64 frame_us    = us_from_ms( frame_ms );
    i64       deadline_us = sys_tick_microseconds() + frame_us;

    PROF_THREAD_NAME( "main" );

    while ( !g_quit_requested )
    {
        run_frame_stats_t stats      = { 0 };
        i64               t_frame_us = sys_tick_microseconds();

        /* -- pump OS events (windowed) ---------------------------------- */

        /* The pump's return is NOT honored here: the main window's WM_CLOSE arms app's
           quit flag AND queues APP_EV_WIN_CLOSE in the same pump, so breaking now would
           exit before the drain below ever hands that event to on_close_request (the
           documented veto point).  Pump, drain, then check should_quit at the bottom of
           the drain -- a veto calls quit_reset and the loop carries on. */
        if ( windowed )
            app()->pump_events();

        /* -- drain event ring ------------------------------------------ */

        /* Drain events.  Sinks are offered each event in order -- rhi (swapchain resize), gui
           (viewport sizes, input, owned-floater lifecycle), the host's on_event tap -- and each
           answers on the app_event_result_t schema (app.h).  The routing rule is ONE line with no
           per-event-type exceptions: stop at the first APP_EVENT_CONSUMED.  A sink that acted on a
           broadcast event (a resize reaching swapchain + viewport + host) answers APP_EVENT_SHARED
           and routing carries on.  Any WIN_CLOSE that survives the whole chain is the main
           window's -- on_close_request may veto it (save prompts). */

        if ( windowed )
        {
            app_event_t ev;
            while ( app()->next_event( &ev ) )
            {
                if ( rhi() && rhi()->event( &ev ) == APP_EVENT_CONSUMED )
                    continue;

                if ( gui() && gui()->event( &ev ) == APP_EVENT_CONSUMED )
                    continue;

                if ( desc->on_event && desc->on_event( &ev ) == APP_EVENT_CONSUMED )
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
                    /* The main window is closing -- give the host a chance to veto it (save prompt).  
                       The veto is the only way to cancel the quit that app() armed in pump_events. */
                    if ( !desc->on_close_request || desc->on_close_request() )
                        goto loop_exit;

                    /* Vetoed: cancel the quit the WM_CLOSE armed so pump_events keeps
                       the app alive ("unsaved changes" flows). */
                    app()->quit_reset();
                }
            }

            /* OS-level quit with no vetoed close in flight (WM_QUIT, or a WIN_CLOSE the
               ring dropped): honor it here, AFTER the drain gave on_close_request its
               chance. */
            if ( app()->should_quit() )
                break;
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

        /* -- dev console housekeeping ------------------------------------ */

        /* Grave/escape toggle + the post-submit redraw pin.  Emits no widgets (those go in
           the gui build below); polls app keys, so it runs after the event drain.  Guarded:
           the console is an optional service (deps gui) -- absent on headless hosts. */
        if ( console() )
             console()->frame( dt );

        /* -- job dispatcher tick --------------------------------------- */

        /* job is in the engine floor -- always present.  With no workers this drains the
           queue on the main thread; with workers it is a cheap no-op. */
        job()->tick();

        /* -- host update ------------------------------------------------- */

        /* Game logic, tool work -- every frame, BEFORE the gui emit so the UI reflects this
           frame's state.  Widget calls do not belong here (the emit below may be skipped on
           clean retained frames); they go in on_gui.  Can call any loaded module API or
           run_host_quit(). */

        PROF_ZONE_BEGIN( "host/update" );
        if ( desc->on_update )
             desc->on_update( dt );
        PROF_ZONE_END();

        i64 t_update_us = sys_tick_microseconds();
        stats.update_us = t_update_us - t_events_us;

        /* -- perf HUD toggle + stats fold ------------------------------- */

        /* Poll F8 and fold last frame's timings once per frame, backend-agnostic: the gui
           backend reads this state in the emit below, the draw backend at the render composite. */
        host_perf_tick();

        /* -- gui emit ---------------------------------------------------- */

        /* frame_begin snaps the IO state from the events drained above and returns frame_dirty:
           false = nothing changed, the retained geometry re-presents and the whole build is
           skipped (on_gui does not run).  On dirty frames the build opens,
           the chrome shell goes first when the window is borderless (it publishes the caption
           band -- read it via viewport_caption_h(0)), then on_gui emits the host's windows.
           frame_end seals the frame either way (clean frames replay volatile widgets there). */

        PROF_ZONE_BEGIN( "host/gui" );
        if ( s_gui_inited )
        {
            /* Push the cvar-selected DPI mode before frame_begin -- its poll resolves the wanted
               scale there.  Every frame: dpi_set is a cheap store, and a console / config edit
               takes hold on the next frame with no change tracking here. */
            if ( s_cv_dpi_mode )
            {
                const char*    m    = cvar_get_string( s_cv_dpi_mode );
                gui_dpi_mode_t mode = ( m[ 0 ] == 'o' ) ? GUI_DPI_OFF
                                    : ( m[ 0 ] == 'm' ) ? GUI_DPI_MANUAL
                                                        : GUI_DPI_AUTO;
                gui()->dpi_set( mode, cvar_get_float( s_cv_ui_scale ) );
            }

            if ( gui()->frame_begin( dt ) )
            {
                gui()->ctx_begin();

                /* Borderless shell -- draws the caption and sizing borders over the host's windows. */
                if ( borderless && s_vp0 != GUI_VP_INVALID )
                    gui()->viewport_shell( s_vp0, desc->name ? desc->name : "orb", GUI_WIN_NONE );

                if ( desc->on_gui )
                     desc->on_gui( dt );

                /* Dev console drop-down, over the host's windows -- emitted last so it draws
                   on top.  A no-op while closed; spans the main viewport's width. */
                if ( console() )
                     console()->emit( dt, s_vp0 );

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
        PROF_ZONE_END();

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

        PROF_ZONE_BEGIN( "host/render" );
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
                    f32 clear[ 4 ];
                    host_clear_color( desc->gui, clear );
                    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
                        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
                        .load_op  = RHI_LOAD_OP_CLEAR,
                        .store_op = RHI_STORE_OP_STORE,
                        .clear    = { clear[ 0 ], clear[ 1 ], clear[ 2 ], clear[ 3 ] },
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
                    f32 clear[ 4 ];
                    host_clear_color( desc->gui, clear );
                    i32 dw = 0, dh = 0;
                    app()->window_get_size( s_win_id, &dw, &dh );
                    draw()->begin_pass( cmd, dw, dh, clear );
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

        PROF_ZONE_END(); // render

        i64 t_render_us = sys_tick_microseconds();
        stats.render_us = t_render_us - t_gui_us;

        /* -- profiler dump flush ------------------------------------------ */

        /* While a prof_dump capture is live the host is the drain consumer: move this
           frame's events (all rings) into the trace file, closing it on the last frame. */
        host_prof_frame_flush();

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
           Editor mode: input-wakeable waits at two timeouts -- the remaining frame
           budget while the gui settles, editor_timeout_ms once it is clean (capped so
           hot-reload checks and other periodic work still run).  Either wait returns
           the moment OS input arrives.
           Realtime gate: a live play/simulate session (run_host_realtime_set) suspends
           the editor waits entirely -- the session must tick every frame regardless of
           OS input, so the loop paces like a game host until the session stops. */

        i64 work_end_us = sys_tick_microseconds();
        i64 remain_us   = deadline_us - work_end_us;

        stats.work_us = work_end_us - t_frame_us;

        PROF_ZONE_BEGIN( "host/wait" );
        if ( editor_sleep && !g_realtime )
        {
            /* Run at frame_ms cadence -- rather than the long input block -- while the gui has
               not SETTLED, then block until input.  "Not settled" is two things:

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
               extra clean-check frame after interaction stops -- then it idles at zero cost.

                 volatile_live : live volatile blocks (gui volatile_cb) patch on idle frames but
                                 advance only when a frame RUNS -- blocking here freezes them
                                 until a timeout / spurious wakeup, stuttering the animation at
                                 the wait interval.  Keep frame cadence while any are on screen;
                                 the moment they leave (window closed / scrolled out / dormant)
                                 this drops false and the zero-cost block resumes. */
            bool gui_settling = s_gui_inited && gui() && ( gui()->wants_redraw() || gui()->frame_dirty() || gui()->volatile_live() );
            if ( gui_settling )
            {
                if ( g_sleep_debug ) printf( "[host] settle frame  (no block)\n" );

                /* Pace at frame cadence but stay AWAKE to input: a deaf Sleep here adds a
                   full frame of latency to every stage of an in-flight gesture (hover promote,
                   press arm, threshold cross, the posted start-move) while the physical mouse
                   keeps going.  On the native-borderless caption that lag let the posted
                   start-move overtake the real button-up already sitting in the queue -- the
                   OS move loop then started after the release and ate the next caption click.
                   Waking on input collapses the gesture pipeline to event rate; with no input
                   this times out at the same cadence the Sleep gave (animations still pace). */
                if ( remain_us >= us_from_ms( 1 ) )
                    sys()->wait_for_os_events_ms( ms_from_us( remain_us ) );
            }
            else
            {
                if ( g_sleep_debug ) { printf( "[host] editor sleep  (timeout %d ms)\n", editor_timeout_ms ); }
                sys()->wait_for_os_events_ms( editor_timeout_ms );
                if ( g_sleep_debug ) { printf( "[host] editor wakeup (frame %llu)\n", (unsigned long long)run()->clock()->frame_number ); }
            }
        }
        else if ( remain_us >= us_from_ms( 1 ) )
        {
            sys()->sleep_milliseconds( ms_from_us( remain_us ) );
        }
        PROF_ZONE_END();

        /* Advance the deadline by exactly one period; if this frame overran the whole
           next budget (load spike, editor wait), resync forward instead of running
           back-to-back catch-up frames.  The editor's input-wakeable waits also clamp
           the OTHER direction: they return early whenever input arrives, and banking
           that credit would push the deadline far ahead of real time -- the first
           quiet frame after an input burst would then stall an animation for the whole
           banked surplus.  Game mode must NOT take that clamp: its Sleep wakes early
           only by request truncation (ms_from_us), and carrying that sub-
           millisecond credit forward is exactly how the accumulator keeps the average
           rate exact. */

        i64 t_end_us = sys_tick_microseconds();
        deadline_us += frame_us;
        if ( deadline_us < t_end_us )
             deadline_us = t_end_us + frame_us;
        else if ( editor_sleep && !g_realtime && deadline_us > t_end_us + frame_us )
             deadline_us = t_end_us + frame_us;

        stats.wait_us  = t_end_us - work_end_us;
        stats.frame_us = t_end_us - t_frame_us;

        /* Profiler frame boundary + headline counter -- macro-gated, zero cost at level 0. */
        PROF_FRAME_MARK();
        PROF_COUNTER_SET( "host/frame_us", stats.frame_us );

        /* Hitch monitor -- armed via "prof_hitch". Work time only: the pacing wait above
           (editor input block, budget sleep) is deliberate idling, not a hitch. */
        host_prof_hitch_frame( ms_from_us_f( stats.work_us ) );

        run_clock_stats_submit( &stats );
    }

loop_exit:
    
    /* ---- shutdown ---------------------------------------------------- */

    run_host_shutdown();
    return 0;
}

/*============================================================================================*/
// clang-format on