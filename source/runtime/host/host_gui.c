/*==============================================================================================

    host_gui.c -- Host-side debug overlays (gui-gated).

    The runtime host owns a per-phase frame profiler the gui knows nothing about: the integer
    microsecond timings stamped through the main loop (events / update / gui / render / work /
    wait / frame) and published via run()->frame_stats(), plus the authoritative frame clock
    (run()->clock()).  gui_frame_overlay.c's built-in perf overlay reads gui's OWN self-measured
    emit / render cost; this one reads the HOST loop's real phase breakdown -- the numbers that
    tell you where a frame's milliseconds actually went, engine-wide, not just inside the UI.

    It is drawn THROUGH gui (region + text) because gui already owns a text rasterizer and the
    host does not -- so every function here early-outs when gui() is absent (headless / console
    hosts have no surface to draw on).  host_gui_debug_frame() is the single entry point: the
    host loop calls it once per frame inside the default context's build (after on_gui, before
    ctx_end), so the overlay lands last and draws on top of the host's own windows.

    Toggle: HOST_GUI_PERF_KEY (F8) cycles the tier, mirroring gui's P-key perf overlay:

        0  off
        1  FPS + frame period
        2  + frame / work / wait split
        3  + per-phase breakdown (events / update / gui / render)

    A function key is used (not a letter) so no want_capture_keyboard fence is needed -- typing
    in a text field can never toggle it.  Like every DEBUG_BAND readout, the panel is exempt from
    dirty tracking: its own changing digits never force a redraw, so on a fully idle editor frame
    the numbers hold until the next real input.  In a game loop every frame is dirty and they run
    live.

    See [[project_run_host_gui_loop]] for the host loop this hangs off, and gui_frame_overlay.c
    for the sibling gui-internal overlay.

==============================================================================================*/
// clang-format off

#define HOST_GUI_PERF_KEY   APP_KEY_F8   /* cycles the perf overlay tier (0..3) */

/*==============================================================================================
    Smoothed readout state -- EMA-folded so the panel is legible instead of a blur of jitter.
    Same one-frame self-measurement lag as any in-frame overlay (it reads last frame's stats).
==============================================================================================*/

static struct
{
    int s_mode;      /* perf overlay tier, HOST_GUI_PERF_KEY cycles 0..3 */
    f32 s_fps;       /* smoothed frames/sec, derived from frame_us       */
    f32 s_frame_ms;  /* smoothed full frame period                       */
    f32 s_work_ms;   /* smoothed work (frame minus pacing)               */
    f32 s_wait_ms;   /* smoothed pacing sleep / event wait               */
    f32 s_events_ms; /* smoothed per-phase costs                         */
    f32 s_update_ms;
    f32 s_gui_ms;
    f32 s_render_ms;

} s_host_perf;

/* One EMA step: seed on the first sample (avoid a long ramp from zero), then trail. */
static f32
host_perf_ema( f32 cur, f32 sample )
{
    return cur <= 0.0f ? sample : cur * 0.9f + sample * 0.1f;
}

/* Fold this frame's raw host timings into the smoothed readouts.  us -> ms at the boundary. */
static void
host_perf_sample( void )
{
    const run_frame_stats_t* fs = run()->frame_stats();

    f32 frame_ms = ( f32 )fs->frame_us * 0.001f;
    f32 inst_fps = fs->frame_us > 0 ? 1.0e6f / ( f32 )fs->frame_us : 0.0f;

    s_host_perf.s_fps       = host_perf_ema( s_host_perf.s_fps,       inst_fps );
    s_host_perf.s_frame_ms  = host_perf_ema( s_host_perf.s_frame_ms,  frame_ms );
    s_host_perf.s_work_ms   = host_perf_ema( s_host_perf.s_work_ms,   ( f32 )fs->work_us   * 0.001f );
    s_host_perf.s_wait_ms   = host_perf_ema( s_host_perf.s_wait_ms,   ( f32 )fs->wait_us   * 0.001f );
    s_host_perf.s_events_ms = host_perf_ema( s_host_perf.s_events_ms, ( f32 )fs->events_us * 0.001f );
    s_host_perf.s_update_ms = host_perf_ema( s_host_perf.s_update_ms, ( f32 )fs->update_us * 0.001f );
    s_host_perf.s_gui_ms    = host_perf_ema( s_host_perf.s_gui_ms,    ( f32 )fs->gui_us    * 0.001f );
    s_host_perf.s_render_ms = host_perf_ema( s_host_perf.s_render_ms, ( f32 )fs->render_us * 0.001f );
}

/*==============================================================================================
    Emit -- a chrome-less HUD region, top-right so it never fights gui's own top-left overlays.
==============================================================================================*/

static void
host_perf_emit( int mode )
{
    if ( mode <= 0 )
        return;

    /* Anchor to the top-right of the main window.  A fixed width keeps the digits from
       reflowing frame to frame; clamp x so a tiny window still shows it. */
    const f32 panel_w = 190.0f;
    i32 win_w = 0, win_h = 0;
    if ( app() )
        app()->window_get_size( run_host_window(), &win_w, &win_h );

    f32 x = ( f32 )win_w - panel_w - 8.0f;
    if ( x < 8.0f )
        x = 8.0f;
    f32 y = 8.0f;

    /* Count the text lines this tier shows so the backdrop covers exactly the content -- region
       autosize gives us the width hug for free, but the backdrop rect we place ourselves. */
    int lines = 1;                       /* FPS row                       */
    if ( mode >= 2 ) lines += 3;         /* frame / work / wait           */
    if ( mode >= 3 ) lines += 4;         /* events / update / gui / render */

    f32 line_h = gui()->line_h();
    f32 pad    = 4.0f;
    f32 panel_h = pad * 2.0f + ( f32 )lines * line_h;
    panel_h += gui()->h_min() * lines;
    

    /* NOSCROLL: a fixed readout.  NO_INPUT: click-through, never enters the hover contest.
       DEBUG_BAND: self-measuring -- its own digits must not count in the stats or poison
       idle-skip (packs into the debug arena band at the tail). */
    gui()->region_begin( "host_perf", x, y, panel_w, 0.0f,
                         GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT | GUI_WIN_DEBUG_BAND );
    {
        /* Backdrop first so it sits behind the region's text but above the windows beneath. */
        gui()->draw_rect( x, y, panel_w, panel_h, GUI_COLOR( 0x10, 0x10, 0x14, 0xFF ) );

        gui()->stack();

        /* FPS graded by health: >=60 green, >=30 amber, else red. */
        f32 fps = s_host_perf.s_fps;
        u32 fps_col = fps >= 60.0f ? GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
                    : fps >= 30.0f ? GUI_COLOR( 0xE0, 0xC0, 0x40, 0xFF )
                    :                GUI_COLOR( 0xEE, 0x55, 0x44, 0xFF );
        char line[ 64 ];
        snprintf( line, sizeof( line ), "HOST %5.1f fps (%5.2f ms)", fps, s_host_perf.s_frame_ms );
        gui()->text_colored( fps_col, line );

        if ( mode >= 2 )
        {
            gui()->textf( "frame  %5.2f ms", s_host_perf.s_frame_ms );
            gui()->textf( "work   %5.2f ms", s_host_perf.s_work_ms  );
            gui()->textf( "wait   %5.2f ms", s_host_perf.s_wait_ms  );
        }           
        if ( mode >= 3 )
        {
            gui()->textf( "events %5.2f ms", s_host_perf.s_events_ms );
            gui()->textf( "update %5.2f ms", s_host_perf.s_update_ms );
            gui()->textf( "gui    %5.2f ms", s_host_perf.s_gui_ms    );
            gui()->textf( "render %5.2f ms", s_host_perf.s_render_ms );
        }
    }
    gui()->region_end();
}

/*==============================================================================================
    Entry point -- called once per frame from the host loop inside the default context's build,
    only when gui is live.  Polls the toggle, folds this frame's stats, emits the overlay.
==============================================================================================*/

void
host_gui_debug_frame( f32 dt )
{
    UNUSED( dt );

    if ( !gui() || !run() )
        return;

    /* Function key -- never text input, so no keyboard-capture fence. */
    if ( gui()->is_key_pressed( HOST_GUI_PERF_KEY ) )
    {
        s_host_perf.s_mode = ( s_host_perf.s_mode + 1 ) % 4;
        printf( "[host] perf overlay: tier %d\n", s_host_perf.s_mode );
    }

    host_perf_sample();
    host_perf_emit( s_host_perf.s_mode );
}

// clang-format on
/*============================================================================================*/
