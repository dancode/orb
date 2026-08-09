/*==============================================================================================

    runtime_service/gui/frame/gui_boot.c -- THE BOOT PATH: one-call setup and the quick gui loop
    it belongs to.

    TIER NOTE: this is the TEST-BED methodology -- for sandboxes, demos, and quick tools where
    the UI is the whole application and setup boilerplate is pure friction.  It is the
    alternative to the engine's idiomatic path, the runtime host (source/runtime, run_host_main),
    which owns the window, the rhi context, the event drain, the composite and the pacing itself
    and wires gui as one optional service.  Nothing here is required to use gui.

    gui's tear-off floaters already own their OS window + rhi context end to end
    (viewport_spawn); boot() extends that to the main surface (rhi device -> OS window ->
    render context -> gui init -> viewport 0).  Everything here composes the same public
    primitives a host would call itself, so the explicit path keeps working unchanged --
    boot is composition, not replacement.

    WHAT boot_ MEANS HERE: membership in this loop, not a dependency on s_boot.  The engine has
    two host methodologies -- the runtime host (source/runtime), which owns the window, context,
    pump, composite and pacing and treats gui as a service, and THIS path, where gui owns the
    main surface and the loop's shape.  Every boot_ verb below belongs to the second one; a
    runtime-path host calls none of them.  How much each actually leans on s_boot varies and is
    beside the point:

      boot()         -- composition.  Every line of it is a public call, in the order a host
                        makes them.  A shortcut, not a mode.
      boot_poll()    -- reads NO s_boot state, and still belongs here: nothing outside this loop
                        calls it, and a real host needs what it does not offer (its own look at
                        the event ring, a close policy other than close-means-quit).  It is
                        boot's pump.  The naming follows the loop, not the state.
      boot_present_* -- the one true fork.  These reach s_boot's window, context, and viewport,
                        which a runtime-path host cannot supply, so they are a reported no-op
                        off this path rather than a partial frame.  That host writes the
                        explicit render block (spelled out in gui_api.h under BOOT PATH).

    frame_pace() also lives in this file but is NOT boot_: it paces over the frame hooks alone
    and sb_vulkan -- an attach-path host with its own window, context and pump -- calls it.
    Shared verb, shared name; it sits here for company with the loop it usually closes.

    Scope contract: this tier composes PUBLIC app/rhi/gui primitives only, and owns nothing
    beyond the main surface's lifecycle (window + swapchain + viewport 0 + the frame pair).
    Anything that is not window/surface/UI -- job ticks, hot-reload, simulation clocks,
    networking -- belongs to the runtime host (source/runtime), never here.

    Included LAST in the gui_frame.c unity (after gui_frame_loop.c): it calls straight into the
    frame lifecycle, the viewport pool, and the window unit.  gui_frame_loop.c reaches back through
    two forward-declared statics -- boot_shutdown() (teardown from gui_shutdown) and
    boot_shell_emit() (the auto chrome shell at the default ctx_begin).

    Ordering contract (mirrors the explicit loop the sandboxes ran):

        gui()->boot( &desc );                        -- once, after mod_init_all
        while ( gui()->boot_poll( &dt ) )           -- pump + route events; false on close
        {
            if ( gui()->frame_begin( dt ) ) { ctx_begin; ...build...; ctx_end; }
            gui()->frame_end();
            rhi_cmd_t cmd;
            if ( gui()->boot_present_begin( &cmd ) ) -- open the frame; host render passes
                ...record into cmd...
            gui()->boot_present_end();               -- gui draw + present + floaters
            gui()->frame_pace( 4, 16 );
        }
        gui()->shutdown();                           -- also tears down the boot-owned surface

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Boot state -- the handles boot() created and shutdown must destroy
==============================================================================================*/

static struct
{
    bool     active;          /* boot() ran; shutdown tears the surface down          */
    i32      win_id;          /* boot-owned OS window (also the viewport slot)        */
    i32      rhi_ctx;         /* boot-owned rhi render context (swapchain)            */
    gui_vp_t vp;              /* the primary viewport boot opened                     */
    bool     shell;           /* borderless boot: auto-emit the chrome shell          */
    f32      clear[ 4 ];      /* boot_present_begin swapchain clear color             */
    char     title[ 96 ];     /* window title, re-used as the shell caption each frame */

} s_boot;

static f64 s_poll_last;       /* boot_poll dt clock (0 = first frame)                */

static struct
{
    bool      begun;          /* boot_present_begin opened this frame (end resets)    */
    bool      cmd_live;       /* rhi frame is open; present must render + end it      */
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

    /* Device first.  rhi()->init() is idempotent, so a host that already initialized rhi (or
       will also init draw()) loses nothing -- this only guarantees the device is live. */
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
    s_boot.vp      = vp;
    s_boot.shell   = !desc->os_chrome;

    /* Alpha 0 reads as "unset" (a cleared swapchain is always opaque): default dark. */
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
   there): the viewport's GPU buffers are already gone, so release the swapchain context and
   the OS window -- the two handles the host used to own.  rhi()->shutdown() stays host-side:
   boot cannot know what else (draw(), host passes) still needs the device. */
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
   frame), so the shell is the first window in its build and the caption band it publishes is
   live for everything after it.  viewport_shell itself no-ops on an OS-chrome window, so this
   is doubly gated.  Explicit-path hosts (no boot) are untouched: s_boot.shell is false. */
static void
boot_shell_emit( void )
{
    if ( !s_boot.active || !s_boot.shell )
        return;
    gui_viewport_shell( s_boot.vp, s_boot.title, GUI_WIN_NONE );
}

/*==============================================================================================
    boot_poll -- the loop's event pump (boot_ by membership: it reads no s_boot, but nothing
    outside this loop calls it -- see WHAT boot_ MEANS HERE at the top)
==============================================================================================*/

/* Pump the OS, route every event through rhi (swapchain resize) and gui (input, floater
   lifecycle), and hand back the frame dt from the boot clock hook.  Returns false when the
   app should exit: pump_events said quit, or the main window was closed (gui_event consumes
   owned-floater closes, so any WIN_CLOSE that reaches here is the main window's).  dt is
   clamped to 100 ms so a debugger stall or window drag does not step the UI by seconds;
   without a clock hook it reads a nominal 60 Hz frame. */
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
    boot_present_begin / boot_present_end -- the render + present pair, and THE fork in this
    file.  Everything else here composes public calls; this pair reaches into the boot-owned
    window + context + viewport, which an attach-path host cannot supply.  Hence the boot_
    prefix: off the boot path there is no half-measure to offer, only the explicit block (named
    in the contract message below, and written out in gui_api.h under BOOT TIER).
==============================================================================================*/

/* Open the main surface's frame: reconcile floaters (the safe point between build and
   present), guard minimized, begin the rhi frame, and clear the swapchain.  Returns true with
   the live command buffer so the host can record its own passes (offscreen scene renders,
   custom draws) before boot_present_end() draws the gui; false means skip them (minimized,
   swapchain rebuild, or no boot) -- still call boot_present_end() unconditionally, it presents
   the floaters and resets this state either way.  A balanced pair like every other begin/end. */

bool
gui_boot_present_begin( rhi_cmd_t* out_cmd )
{
    /* The fork gate, FIRST -- before the perf clock, before viewport_update, before `begun`.
       Off the boot path this is not a frame that came out empty, it is a call that does not
       apply, so it must leave NOTHING behind: a viewport_update from here would silently double
       the reconcile such a host already runs, and a latched `begun` would hand the floater
       present to a pair that never opened anything. */
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

/* Close the frame: draw the gui over whatever the host recorded, present the main surface,
   then present every owned floater.  No-op without a matching boot_present_begin -- the pair is
   balanced by contract; there is no hidden self-begin.  Off the boot path `begun` never latches,
   so this is a complete no-op and the begin above has already named the rule. */
void
gui_boot_present_end( void )
{
    if ( !s_present.begun )
        return;

    if ( s_present.cmd_live )
    {
        gui_render( s_boot.vp, s_present.cmd );
        rhi()->frame_end( s_boot.rhi_ctx );
    }

    gui_viewport_render_floaters();

    s_present.begun    = false;
    s_present.cmd_live = false;

    perf_present_end();   /* close the present clock: pair wall - render flush = present overhead */
}

/*==============================================================================================
    frame_pace -- the end-of-loop idle sleep.  NOT boot_: it paces over the frame hooks alone,
    and sb_vulkan (own window, own context, own pump) calls it -- a shared verb that happens to
    live in this file because the boot loop is where it usually appears.

    Call once at the bottom of the main loop, after the present.  The two parameters set the
    host's cadence; 0 opts that sleep out entirely (no call), even while the feature is on:

        spin_sleep_ms -- the default sleep between frames when idle skip is off (or unavailable).
                         4 ~= 250 Hz game cadence; 0 = free-run at full speed.
        anim_sleep_ms -- the sleep while idle skip is ON but the UI is still SETTLING: an animation
                         is mid-transition (s_any_redraw) OR this frame still emitted widgets
                         (gui_frame_dirty()).  Frames keep pumping until a genuinely clean frame is
                         produced, so a collapsed window, a snapping popup, or a click's next-frame
                         structural change lands instead of stalling.  16 ~= 60 Hz; 0 = free-run.

    With idle skip on (I, or set_idle_skip) and the UI settled, the loop blocks on OS input instead
    (500 ms safety cap), so a static UI burns no frames -- unless live volatile blocks are on
    screen (gui_volatile_live), which keep the anim_sleep_ms cadence so their idle-frame patches
    actually present.  Requires the sleep / wait hooks from frame_set_hooks; without them this is
    a no-op (the host loop just spins).

    The hooks (s_hook_sleep / s_hook_wait), s_idle_skip, and s_any_redraw live in
    gui_frame_overlay.c / gui_frame_loop.c -- both included before this unit, so they are in scope here.
==============================================================================================*/

void
gui_frame_pace( i32 spin_sleep_ms, i32 anim_sleep_ms )
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
