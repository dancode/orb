/*==============================================================================================

    runtime_service/gui/frame/gui_boot.c -- convenience runtime setup and loop for GUI TIER.

    We use this for our sandbox and testbed applications that isolate themselves from the full
    runtime. Gui owns the main surface (window + rhi context + viewport 0) and drives the loop. 
        
    The runtime host (source/runtime, run_host_main) is the alternative: It owns all of that 
    itself and treats gui as an optional service. Nothing here is required to use gui.

    boot() is pure composition -- every line is an ordered public call a host would make itself.
    A true runtime-path host calls none of the boot_ functions.

    boot()          : composes the public setup sequence.
    boot_poll()     : gui loop event pump. 
    boot_present_*  : dispatch to rhi()

    boot_pace()     : this loop's end-of-frame sleep (spin / settle / block-on-input).

    boot_ means membership in THIS loop, not a dependency on s_boot.  boot_pace touches no s_boot
    state, but the ladder it runs is this loop's pacing policy: a runtime host schedules against a
    frame deadline instead and reads gui's settle state through the public queries (wants_redraw,
    frame_dirty, volatile_live) -- see run_host.c's editor_sleep.  Those queries are the shared
    part; the sleep is not.

    boot_shutdown()    -- teardown called from gui_shutdown
    boot_shell_emit()  -- auto chrome shell emitted at the default ctx_begin

    Ordering contract:

        gui()->boot( &desc );                        -- once, after mod_init_all
        while ( gui()->boot_poll( &dt ) )           -- pump + route events; false on close
        {
            if ( gui()->frame_begin( dt ) ) { ctx_begin; ...build...; ctx_end; }
            gui()->frame_end();
            rhi_cmd_t cmd;
            if ( gui()->boot_present_begin( &cmd ) ) -- open the frame; host render passes
                ...record into cmd...
            gui()->boot_present_end();               -- gui draw + present + floaters
            gui()->boot_pace ( 4, 16 );
        }
        gui()->shutdown();                           -- also tears down the boot-owned surface

==============================================================================================*/
// clang-format off

/*==============================================================================================
    boot state -- handles created by boot() that shutdown must destroy
==============================================================================================*/

static struct
{
    bool     active;          // boot() ran; shutdown tears the surface down          
    bool     shell;           // borderless boot: auto-emit the chrome shell 

    i32      win_id;          // boot-owned OS window (also the viewport slot)
    i32      rhi_ctx;         // boot-owned rhi render context (swapchain)
    i32      vp_id;           // the primary viewport boot opened

    f32      clear[ 4 ];      // boot_present_begin swapchain clear color
    char     title[ 64 ];     // window title, re-used as the shell caption each frame

} s_boot;

static f64 s_poll_last;       // boot_poll dt clock (0 = first frame)                

static struct
{
    bool      begun;          // boot_present_begin opened this frame (end resets)    
    bool      cmd_live;       // rhi frame is open; present must render + end it      
    rhi_cmd_t cmd;

} s_present;

/*==============================================================================================
    boot -- stand up the main surface end to end
==============================================================================================*/

gui_vp_t
gui_boot( const gui_boot_desc_t* desc )
{
    if ( !desc || s_boot.active )
        return GUI_VP_INVALID;

    /* rhi()->init() is idempotent -- safe if the host already initialized rhi or draw(). */

    if ( !rhi()->init() )
        return GUI_VP_INVALID;

    u32 win_flags = desc->os_chrome ? APP_WIN_DEFAULT : APP_WIN_BORDERLESS;
    i32 win = app()->window_open( desc->title ? desc->title : "orb",
                                  desc->x, desc->y, desc->w, desc->h, win_flags );
    if ( win == APP_WIN_INVALID )
        return GUI_VP_INVALID;

    i32 rctx = rhi()->context_open( win );
    if ( rctx == RHI_CTX_INVALID )
    {
        app()->window_close( win );
        return GUI_VP_INVALID;
    }

    if ( !gui_init( desc->font ) )
    {
        rhi()->context_destroy( rctx );
        app()->window_close( win );
        return GUI_VP_INVALID;
    }

    gui_vp_t vp = gui_viewport_open( win );
    if ( vp == GUI_VP_INVALID )
    {
        gui_shutdown();
        rhi()->context_destroy( rctx );
        app()->window_close( win );
        return GUI_VP_INVALID;
    }

    gui_frame_set_hooks( desc->clock, desc->sleep, desc->wait );
    if ( desc->debug )
        gui_debug_enable( true );

    s_boot.active  = true;
    s_boot.win_id  = win;
    s_boot.rhi_ctx = rctx;
    s_boot.vp_id   = vp;
    s_boot.shell   = !desc->os_chrome;

    /* Alpha 0 means unset (cleared swapchain is always opaque): fall back to default dark. */
    if ( desc->clear[ 3 ] > 0.0f )
        memcpy( s_boot.clear, desc->clear, sizeof( s_boot.clear ) );
    else
    {
        s_boot.clear[ 0 ] = RHI_CLEAR_DEFAULT_R;  s_boot.clear[ 1 ] = RHI_CLEAR_DEFAULT_G;
        s_boot.clear[ 2 ] = RHI_CLEAR_DEFAULT_B;  s_boot.clear[ 3 ] = RHI_CLEAR_DEFAULT_A;
    }

    fmt_snprintf( s_boot.title, sizeof( s_boot.title ), "%s", desc->title ? desc->title : "orb" );
    return vp;
}

/* Teardown for the boot-owned surface, called at the END of gui_shutdown (forward-declared
   there).  The viewport's GPU buffers are already gone; release the swapchain context and
   OS window.  rhi()->shutdown() stays host-side -- boot cannot know what else still needs
   the device. */

static void
boot_shutdown( void )
{
    if ( !s_boot.active )
        return;
    rhi()->context_destroy( s_boot.rhi_ctx );
    app()->window_close( s_boot.win_id );
    memset( &s_boot, 0, sizeof( s_boot ) );
    memset( &s_present, 0, sizeof( s_present ) );
    s_poll_last = 0.0;
}

/* Auto chrome shell -- called from gui_ctx_begin when the DEFAULT context binds (once per
   frame).  viewport_shell no-ops on an OS-chrome window, so this is doubly gated.
   Explicit-path hosts (no boot) are untouched: s_boot.shell is false. */

static void
boot_shell_emit( void )
{
    if ( !s_boot.active || !s_boot.shell )
        return;
    gui_viewport_shell( s_boot.vp_id, s_boot.title, GUI_WIN_NONE );
}

/*==============================================================================================
    boot_poll -- the loop's event pump
==============================================================================================*/

/* Pump the OS, route events through rhi (swapchain resize) and gui (input, floater lifecycle),
   and return the frame dt.  Returns false when the app should exit: pump_events said quit, or
   the main window was closed (gui_event consumes owned-floater closes, so any WIN_CLOSE that
   reaches here is the main window's).  dt is clamped to 100 ms to prevent large steps after a
   debugger stall or window drag.  Without a clock hook, defaults to a nominal 60 Hz frame. */

bool
gui_boot_poll( f32* out_dt )
{
    f64 t_poll = perf_span_open();   /* time the OS pump + input snapshot as the "poll" phase */

    if ( !app()->pump_events() )
        return false;

    app_event_t ev;
    while ( app()->next_event( &ev ) )
    {
        rhi()->event( &ev );
        if ( gui_event( &ev ) )
            continue;
        if ( ev.type == APP_EV_WIN_CLOSE )
            return false;
    }

    f32 dt = 1.0f / 60.0f;
    if ( s_perf.clock )
    {
        f64 now = s_perf.clock();
        if ( s_poll_last > 0.0 )
        {
            dt = ( f32 )( now - s_poll_last );
            if ( dt > 0.1f )
                dt = 0.1f;
        }
        s_poll_last = now;
    }
    if ( out_dt )
        *out_dt = dt;

    perf_span_ema( &s_perf.s_poll_ms, t_poll );
    return true;
}

/*==============================================================================================
    boot_present_begin / boot_present_end -- render + present pair.

    These reach the boot-owned window + context + viewport, which a runtime-path host cannot
    supply.  Off the boot path the begin fires a contract message and returns false; there is
    no half-measure -- use the explicit block from gui_api.h (BOOT TIER) instead.
==============================================================================================*/

/* Open the main surface's frame: reconcile floaters, guard minimized, begin the rhi frame,
   and clear the swapchain.  Returns true with the live command buffer so the host can record
   its own passes before boot_present_end() draws the gui.  Returns false on minimized,
   swapchain rebuild, or no boot -- still call boot_present_end() unconditionally. */

bool
gui_boot_present_begin( rhi_cmd_t* out_cmd )
{
    /* Fork gate FIRST -- before perf clock, viewport_update, or `begun`.  Off the boot path
       this call does not apply at all: a viewport_update here would double-reconcile a host
       that already runs it, and a latched `begun` would hand the floater present to a pair
       that never opened anything. */

    GUI_CONTRACT( s_boot.active,
                  "boot_present_begin() without gui()->boot() -- this pair renders through the "
                  "boot-owned window + rhi context, so it cannot present a host-owned surface.  "
                  "Write the explicit block instead: viewport_update() -> rhi()->frame_begin( "
                  "your ctx ) -> your passes -> render( vp, cmd ) -> rhi()->frame_end() -> "
                  "viewport_render_floaters().  See BOOT TIER in gui_api.h.\n" );

    if ( !s_boot.active )
        return false;

    perf_present_begin();   /* arm the present clock: the whole pair, fence wait included */
    gui_viewport_update();

    s_present.begun    = true;
    s_present.cmd_live = false;

    if ( app()->window_is_minimized( s_boot.win_id ) )
        return false;

    rhi_cmd_t cmd = rhi()->frame_begin( s_boot.rhi_ctx );
    if ( !rhi_cmd_valid( cmd ) )
        return false;

    rhi()->cmd_begin_rendering( cmd, &( rhi_color_attachment_t ){
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_CLEAR,
        .store_op = RHI_STORE_OP_STORE,
        .clear    = { s_boot.clear[ 0 ], s_boot.clear[ 1 ], s_boot.clear[ 2 ], s_boot.clear[ 3 ] },
    }, 1, NULL );
    rhi()->cmd_end_rendering( cmd );

    s_present.cmd      = cmd;
    s_present.cmd_live = true;
    if ( out_cmd )
        *out_cmd = cmd;

    return true;
}

/* Draw the gui over whatever the host recorded, present the main surface, then present every
   owned floater.  No-ops without a matching boot_present_begin; off the boot path `begun`
   never latches so this is a complete no-op. */

void
gui_boot_present_end( void )
{
    if ( !s_present.begun )
        return;

    if ( s_present.cmd_live )
    {
        gui_render( s_boot.vp_id, s_present.cmd );
        rhi()->frame_end( s_boot.rhi_ctx );
    }

    gui_viewport_render_floaters();

    s_present.begun    = false;
    s_present.cmd_live = false;

    /* close the present clock: pair wall - render flush = present overhead */

    perf_present_end();
}

/*==============================================================================================
    boot_pace -- the boot loop's end-of-frame sleep.

    Call once at the bottom of the loop, after the present.  This is boot's pacing POLICY, not a
    service a real host reuses: a runtime host schedules against a frame deadline (budget, catch-up
    resync, realtime gate) and reads gui's settle state through the public queries -- wants_redraw,
    frame_dirty, volatile_live -- to make the same decision on its own terms.  That is the shared
    part, and it is already public.  See run_host.c's editor_sleep for the worked example.

    The two parameters set the cadence; passing 0 opts that branch out entirely:

        spin_sleep_ms -- sleep between frames when idle skip is off (or unavailable).
                         4 ~= 250 Hz; 0 = free-run.
        anim_sleep_ms -- sleep while idle skip is ON but the UI is still settling: an animation
                         is mid-transition (s_any_redraw) OR this frame emitted widgets
                         (gui_frame_dirty()).  Frames keep pumping until a clean frame lands.
                         16 ~= 60 Hz; 0 = free-run.

    With idle skip on and the UI settled, the loop blocks on OS input (500 ms safety cap),
    burning no frames -- unless gui_volatile_live() blocks are on screen, which keep the
    anim_sleep_ms cadence so their idle-frame patches present without stutter.

    Requires sleep / wait hooks from frame_set_hooks; without them this is a no-op.

    s_hook_sleep, s_hook_wait, s_idle_skip, and s_any_redraw live in
    gui_frame_overlay.c / gui_frame_loop.c -- both included before this unit.
==============================================================================================*/

void
gui_boot_pace( i32 spin_sleep_ms, i32 anim_sleep_ms )
{
    f64 t_wait = perf_span_open();   /* time the whole pace phase: this IS the loop's "wait time" */

    if ( s_idle_skip && s_hook_wait )
    {
        /* Keep pumping while the UI has not settled: animation in flight, or this frame still
           emitted (retained-mode needs a follow-up frame to re-stabilize).  Block only once a
           fully clean frame is produced -- otherwise deferred / multi-frame updates freeze until
           the next input.  (Same gate the runtime host's editor_sleep uses.) */
        if ( s_any_redraw || gui_frame_dirty() )
        {
            if ( s_hook_sleep && anim_sleep_ms > 0 )
                 s_hook_sleep( anim_sleep_ms );   /* settling: pump frames until it goes clean */
        }
        else if ( gui_volatile_live() )
        {
            /* Clean frame, but volatile blocks are on screen: they advance only when a frame
               runs, so a blocking wait would freeze them until a timeout / spurious wakeup --
               stutter at the wait interval.  Keep presenting at the animation cadence instead;
               these frames stay on the cheap path (patch + upload, no emit, no full tess). */
            if ( s_hook_sleep && anim_sleep_ms > 0 )
                 s_hook_sleep( anim_sleep_ms );
        }
        else
        {
            s_hook_wait( 500 );                  /* idle: wake on input (500 ms safety cap) */
        }
    }
    else if ( s_hook_sleep && spin_sleep_ms > 0 )
    {
        s_hook_sleep( spin_sleep_ms );           /* spin cadence between frames */
    }

    perf_span_ema( &s_perf.s_wait_ms, t_wait );
}

// clang-format on
/*============================================================================================*/
