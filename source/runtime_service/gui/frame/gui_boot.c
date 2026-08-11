/*==============================================================================================

    runtime_service/gui/frame/gui_boot.c -- convenience runtime setup and loop for GUI TIER.

    We use this for our sandbox and testbed applications that isolate themselves from the full
    runtime. Gui owns the main surface (window + rhi context + viewport 0) and drives the loop. 

    The runtime host (source/runtime, run_host_main) is the alternative: It owns all of that 
    itself and treats gui as an optional service. Nothing here is required to use gui.

    boot() is pure composition -- every line is an ordered public call a host would make itself.
    A true runtime-path host calls none of the boot_ functions.

    boot()              : composes the public setup sequence.
    boot_poll()         : gui loop event pump. 
    boot_present_*      : dispatch to rhi()
    boot_pace()         : loop's end-of-frame sleep (spin / settle / block-on-input).
    boot_shutdown()     : teardown called from gui_shutdown
    boot_shell_emit()   : auto chrome shell emitted at the default ctx_begin

    Ordering contract:

        gui()->boot( &desc );                           -- once, after mod_init_all
        while ( gui()->boot_poll( &dt ) )               -- pump + route events; false on close
        {
            if ( gui()->frame_begin( dt ) ) { ctx_begin; ...build...; ctx_end; }
            gui()->frame_end();
            rhi_cmd_t cmd;
            if ( gui()->boot_present_begin( &cmd ) )    -- open the frame; host render passes
                ...record into cmd...
            gui()->boot_present_end();                  -- gui draw + present + floaters
            gui()->boot_pace ( 4, 16 );
        }
        gui()->shutdown();                              -- also tears down the boot-owned surface

==============================================================================================*/
// clang-format off

/*==============================================================================================

    Boot State -- GUI managed application setup and frame loop state.   

==============================================================================================*/

static struct
{
    bool     active;            // boot() ran; shutdown tears the surface down          
    bool     shell;             // borderless boot: auto-emit the chrome shell 

    i32      win_id;            // boot-owned OS window (also the viewport slot)
    i32      rhi_ctx;           // boot-owned rhi render context (swapchain)
    i32      vp_id;             // the primary viewport boot opened

    f32      clear[ 4 ];        // boot_present_begin swapchain clear color
    char     title[ 64 ];       // window title, re-used as the shell caption each frame

} s_boot;

static f64 s_poll_last;         // boot_poll dt clock (0 = first frame)

static struct
{
    bool      begun;            // boot_present_begin opened this frame (end resets)    
    bool      cmd_live;         // rhi frame is open; present must render + end it      
    rhi_cmd_t cmd;              // boot_present_begin's rhi frame command list.

} s_present;

/*==============================================================================================

    Boot Init -- Convenience composition of the public init sequence  

==============================================================================================*/

i32
gui_boot( const gui_boot_desc_t* desc )
{
    if ( !desc || s_boot.active )
        return GUI_VP_INVALID;

    /* rhi()->init() is safe to call again if already initialized */

    if ( !rhi()->init() )
        return GUI_VP_INVALID;

    /* create window and render context */

    u32  win_flags = desc->os_chrome ? APP_WIN_DEFAULT : APP_WIN_BORDERLESS;
    i32  win = app()->window_open( desc->title ? desc->title : "orb", desc->x, desc->y, desc->w, desc->h, win_flags );
    if ( win == APP_WIN_INVALID )
         return GUI_VP_INVALID;

    i32  rctx = rhi()->context_open( win );
    if ( rctx == RHI_CTX_INVALID ) {
         app()->window_close( win );
         return GUI_VP_INVALID;
    }

    /* the gui requires a valid rhi context to initialize the drawing atlas */

    if ( !gui_init( desc->font ) ) {
         rhi()->context_destroy( rctx );
         app()->window_close( win );
         return GUI_VP_INVALID;
    }

    /* create the main windows viewport render area */

    i32  vp = gui_viewport_open( win );
    if ( vp == GUI_VP_INVALID )
    {
        gui_shutdown();
        rhi()->context_destroy( rctx );
        app()->window_close( win );
        return GUI_VP_INVALID;
    }

    /* hook the host's clock/sleep/wait into the gui loop */

    gui_frame_set_hooks( desc->clock, desc->sleep, desc->wait );

    /* optionally enable debug view */

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

/* Teardown for the boot-owned surface, called at the END of gui_shutdown() -- NOT IN USER SPACE */

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

/* Auto chrome shell -- called from gui_ctx_begin -- NOT IN USER SPACE
   when the DEFAULT context binds (once per frame). 
   viewport_shell no-ops on an OS-chrome window. */

static void
boot_shell_emit( void )
{
    if ( !s_boot.active || !s_boot.shell )
        return;

    gui_viewport_shell( s_boot.vp_id, s_boot.title, GUI_WIN_NONE );
}

/*==============================================================================================

    Boot Poll -- The loop's event pump

    Pump the OS, route events through rhi (swapchain resize) and gui (input, floater lifecycle),
    and return the frame dt.  
    
    - Returns false when the app should exit: pump_events said quit, or the main window was closed.
    - Outputs the dt used to advance the gui frame clock (perf_frame_begin) and drive animations.  
      The host may use it for its own scheduling, but gui does not require it.

    - The gui_event consumes owned-floater closes, WIN_CLOSE inside here is the main window.
    - The dt is clamped to 100 ms to prevent large steps after a debugger stall or window drag.  
    - Without a clock hook, defaults to a nominal 60 Hz frame.    

==============================================================================================*/

bool
gui_boot_poll( f32* out_dt )
{
    f64 t_poll = perf_span_open();          /* start timing the poll phase */
    s_perf.t_loop_start = t_poll;           /* elapsed-time base for this iteration of polling */

    if ( !app()->pump_events() )
        return false;

    app_event_t ev;
    while ( app()->next_event( &ev ) )
    {
        rhi()->event( &ev );                /* render context only handles resize events */

        if ( gui_event( &ev ) )             /* route input + floater events to gui */
            continue;

        if ( ev.type == APP_EV_WIN_CLOSE )  /* exit gui loop on main window */
            return false;
    }

    /* finished polling -- compute the frame dt for the gui clock and animation advance */

    f32 dt = 1.0f / 60.0f;                  /* default to 60 Hz if no clock hook is installed */
    if ( s_perf.clock )
    {
        f64 now = s_perf.clock();
        if ( s_poll_last > 0.0 )            /* first frame has nothing to delta against */
        {            
            dt = ( f32 )( now - s_poll_last );
            if ( dt > 0.1f )                
                 dt = 0.1f;                 /* clamp to avoid huge steps after a stall */
        }
        s_poll_last = now;                  /* store for the next frame's delta */
    }
    if ( out_dt )
        *out_dt = dt;

    /* fold the poll span into the smoothed readout for the overlay -- calls clock() */
    perf_span_ema( &s_perf.s_poll_ms, t_poll );
    return true;
}

/*==============================================================================================

    BOOT PRESENT -- The loop's render + present pair
    
==============================================================================================*/

bool
gui_boot_present_begin( rhi_cmd_t* out_cmd )
{
    GUI_CONTRACT( s_boot.active,
                  "boot_present_begin() without gui()->boot() -- this pair only presents the "
                  "boot-owned window; write the explicit block (BOOT TIER in gui_api.h)." );

    if ( !s_boot.active )
        return false;

    perf_present_begin();   /* arm the present clock: the whole pair, fence wait included */
    gui_viewport_update();  /* reconcile any floater lifecycle requests (tear-off, merge-back, close) */

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

    The two parameters set the cadence; passing 0 opts that branch out entirely.  Both are a
    target period for the WHOLE iteration (poll + emit + render + present + this sleep), not a
    flat sleep tacked on top -- pace_remaining_ms() subtracts what boot_poll..boot_present_end
    already spent this frame, so the loop lands on the target rate instead of undershooting it
    by however long the frame's own work took:

        spin_sleep_ms -- cadence between frames when idle skip is off (or unavailable): a hard
                         rate cap, e.g. a game loop pinning render rate.  4 ~= 250 Hz; 0 =
                         free-run.  Paced by pace_spin_wait() -- sleeps most of the remaining
                         budget, then busy-spins the last couple ms, since Sleep() alone
                         overshoots a target this tight (see pace_spin_wait's comment).
        anim_sleep_ms -- sleep while idle skip is ON but the UI is still settling: an animation
                         is mid-transition (s_any_redraw) OR this frame emitted widgets
                         (gui_frame_dirty()).  Frames keep pumping until a clean frame lands.
                         16 ~= 60 Hz; 0 = free-run.  Sleep-only (pace_remaining_ms) -- this path
                         is a courtesy cadence, not a rate cap, so Sleep()'s slop is fine and a
                         spin tail would waste a core for no benefit here.

    With idle skip on and the UI settled, the loop blocks on OS input (500 ms safety cap),
    burning no frames -- unless gui_volatile_live() blocks are on screen, which keep the
    anim_sleep_ms cadence so their idle-frame patches present without stutter.

    Requires sleep / wait hooks from frame_set_hooks; without them this is a no-op.

    s_hook_sleep, s_hook_wait, s_idle_skip, and s_any_redraw live in
    gui_frame_overlay.c / gui_frame_loop.c -- both included before this unit.
==============================================================================================*/

/* spin_sleep_ms / anim_sleep_ms are a cadence for the WHOLE loop iteration (poll + emit +
   render + present + this sleep), not just the sleep call -- so sleep only the budget left
   after everything before it this frame.  t_loop_start is armed at boot_poll entry.  Returns
   0 once the frame has already eaten the whole budget (or clock/loop-start is unavailable, so
   the caller falls back to the full target -- no worse than the old fixed-sleep behavior). */
static i32
pace_remaining_ms( i32 target_ms )
{
    if ( !s_perf.clock || s_perf.t_loop_start <= 0.0 )
        return target_ms;

    f64 elapsed_ms = ( s_perf.clock() - s_perf.t_loop_start ) * 1000.0;
    i32 remain     = ( i32 )( ( f64 )target_ms - elapsed_ms + 0.5 );
    return remain > 0 ? remain : 0;
}

/* Sleep() only promises "at least N ms" -- OS wake + dispatch latency after the requested
   duration routinely overshoots a millisecond-scale target (observed ~0.3-1 ms on Windows even
   with timeBeginPeriod(1)), which is a large fraction of a 4 ms / 250 Hz period.  Hold back this
   many ms from the sleep and spin them instead: polling the clock has no wake latency, so it
   lands on the deadline exactly, at the cost of busy-waiting the tail on this core. */
#define PACE_SLEEP_MARGIN_MS 2.0

/* Hybrid sleep + spin: sleep for most of target_ms's remaining budget, then busy-spin the last
   PACE_SLEEP_MARGIN_MS to land on the deadline exactly.  No-op (returns immediately) without a
   clock or an armed loop start. */
static void
pace_spin_wait( i32 target_ms )
{
    if ( !s_perf.clock || s_perf.t_loop_start <= 0.0 )
        return;

    f64 target_s = s_perf.t_loop_start + ( f64 )target_ms / 1000.0;

    if ( s_hook_sleep )
    {
        f64 sleep_s = target_s - s_perf.clock() - PACE_SLEEP_MARGIN_MS / 1000.0;
        if ( sleep_s > 0.0 )
             s_hook_sleep( ( i32 )( sleep_s * 1000.0 + 0.5 ) );
    }

    while ( s_perf.clock() < target_s )
        ;   /* spin out the sleep's margin (and any overshoot) to hit the deadline exactly */
}

void
gui_boot_pace( i32 spin_sleep_ms, i32 anim_sleep_ms )
{
    /* time the whole pace phase: this IS the loop's "wait time" */

    f64 t_wait = perf_span_open();

    if ( s_idle_skip && s_hook_wait )
    {
        /* Keep pumping while the UI has not settled: animation in flight, or this frame still
           emitted (retained-mode needs a follow-up frame to re-stabilize).  Block only once a
           fully clean frame is produced -- otherwise deferred / multi-frame updates freeze until
           the next input.  (Same gate the runtime host's editor_sleep uses.) */
        if ( s_any_redraw || gui_frame_dirty() )
        {
            if ( s_hook_sleep && anim_sleep_ms > 0 )
            {
                i32 remain = pace_remaining_ms( anim_sleep_ms );
                if ( remain > 0 )
                     s_hook_sleep( remain );      /* settling: pump frames until it goes clean */
            }
        }
        else if ( gui_volatile_live() )
        {
            /* Clean frame, but volatile blocks are on screen: they advance only when a frame
               runs, so a blocking wait would freeze them until a timeout / spurious wakeup --
               stutter at the wait interval.  Keep presenting at the animation cadence instead;
               these frames stay on the cheap path (patch + upload, no emit, no full tess). */
            if ( s_hook_sleep && anim_sleep_ms > 0 )
            {
                i32 remain = pace_remaining_ms( anim_sleep_ms );
                if ( remain > 0 )
                     s_hook_sleep( remain );
            }
        }
        else
        {
            s_hook_wait( 500 );                  /* idle: wake on input (500 ms safety cap) */
        }
    }
    else if ( s_hook_sleep && spin_sleep_ms > 0 )
    {
        pace_spin_wait( spin_sleep_ms );          /* hybrid sleep+spin: hit the target rate exactly */
    }

    perf_span_ema( &s_perf.s_wait_ms, t_wait );
}

// clang-format on
/*============================================================================================*/
