/*==============================================================================================

    host_gui.c -- Host-side perf HUD (two render backends).

    The runtime host owns a per-phase frame profiler the gui knows nothing about: the integer
    microsecond timings stamped through the main loop (events / update / gui / render / work /
    wait / frame) and published via run()->frame_stats(), plus the authoritative frame clock
    (run()->clock()).  gui_frame_overlay.c's built-in perf overlay reads gui's OWN self-measured
    emit / render cost; this one reads the HOST loop's real phase breakdown -- the numbers that
    tell you where a frame's milliseconds actually went, engine-wide, not just inside the UI.

    ONE feature, TWO backends -- the overlay renders through whichever surface the host has:

        host_perf_tick()        -- backend-agnostic: poll the toggle (F8) + fold this frame's
                                   stats.  Called once per frame from the loop, unconditionally.
        host_gui_debug_frame()  -- GUI backend: emit the readout as a chrome-less region + text.
                                   Called inside the default context's build (after on_gui, before
                                   ctx_end); no-op when gui() is absent.  The richer path.
        host_draw_perf_content() -- DRAW backend: emit the readout as bitmap text through the draw
                                   service's built-in 5x7 font (draw_font.c) -- no gui, no atlas.
                                   Called at a composite point inside an open draw pass when gui is
                                   NOT present; the gui-less fallback.

    The host loop picks the backend: if gui is live it draws the gui version; otherwise, if draw is
    live, it composites the draw version over the frame.  Same F8 toggle + same stats feed both.

    Toggle: HOST_GUI_PERF_KEY (F8) cycles the tier:

        0  off
        1  FPS + frame period
        2  + frame / work / wait split
        3  + per-phase breakdown (events / update / gui / render)

    A function key is used (not a letter) so no keyboard-capture fence is needed -- it reads the
    app input snapshot directly (app()->key_pressed), which works with or without gui.

    See [[project_run_host_gui_loop]] for the host loop this hangs off, gui_frame_overlay.c for the
    sibling gui-internal overlay, and draw_font.c for the bitmap font the draw backend uses.

==============================================================================================*/
// clang-format off

#define HOST_GUI_PERF_KEY   APP_KEY_F8   /* cycles the perf overlay tier (0..3) */

/*==============================================================================================
    Shared readout state -- EMA-folded so the panel is legible instead of a blur of jitter.
    Same one-frame self-measurement lag as any in-frame overlay (it reads last frame's stats).
==============================================================================================*/

static struct
{
    int  s_mode;       /* perf overlay tier, HOST_GUI_PERF_KEY cycles 0..3 */
    bool s_force_draw; /* force the draw (bitmap-font) backend even when gui is live */
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

/* Poll the toggle and fold this frame's stats.  Backend-agnostic: called once per frame from the
   host loop regardless of which surface (gui / draw) will paint it. */
void
host_perf_tick( void )
{
    if ( !run() )
        return;

    /* Function key -- never text input, so no keyboard-capture fence. */
    if ( app() && app()->key_pressed( HOST_GUI_PERF_KEY ) )
    {
        s_host_perf.s_mode = ( s_host_perf.s_mode + 1 ) % 4;
        printf( "[host] perf overlay: tier %d\n", s_host_perf.s_mode );
    }

    host_perf_sample();
}

/* True while the overlay is on (tier > 0).  Lets the host skip opening an overlay pass it would
   fill with nothing -- used to gate the draw-backend composite when a scene is already present. */
bool
host_perf_active( void )
{
    return s_host_perf.s_mode > 0;
}

/* Force the DRAW (bitmap-font) backend even when gui is present: the gui backend then yields
   (host_gui_debug_frame no-ops) and the host composites the draw HUD over the gui frame instead.
   For comparing the two readouts, or a precise low-overhead HUD while the full UI is up.  Off by
   default -- gui wins when it is live.  Settable from a host at any time (on_ready / on_update). */
void
host_perf_set_force_draw( bool on )
{
    s_host_perf.s_force_draw = on;
}

bool
host_perf_force_draw( void )
{
    return s_host_perf.s_force_draw;
}

/*==============================================================================================
    GUI backend -- a chrome-less HUD region, top-right so it never fights gui's own top-left
    overlays.  Only reachable when gui() is live.
==============================================================================================*/

static void
host_perf_emit_gui( int mode )
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

/* GUI entry point -- called once per frame from the host loop inside the default context's build.
   Emits only; the toggle + stats fold happened earlier in host_perf_tick. */
void
host_gui_debug_frame( f32 dt )
{
    UNUSED( dt );
    /* Yield to the draw backend when force-draw is set -- the host composites it at render time. */
    if ( gui() && !s_host_perf.s_force_draw )
        host_perf_emit_gui( s_host_perf.s_mode );
}

/*==============================================================================================
    DRAW backend -- the gui-less fallback.  Emits the same readout as bitmap text through the
    draw service's built-in 5x7 font.  The caller has already opened a draw pass (begin_overlay /
    begin_pass), so this only lays down the backdrop rect + text; it manages no pass state.
==============================================================================================*/

/* Linear rgba palette for the draw backend (draw takes f32[4], not packed abgr). */
static const f32 HOST_PERF_WHITE[ 4 ] = { 0.85f, 0.85f, 0.88f, 1.0f };
static const f32 HOST_PERF_GREEN[ 4 ] = { 0.40f, 0.87f, 0.33f, 1.0f };
static const f32 HOST_PERF_AMBER[ 4 ] = { 0.88f, 0.75f, 0.25f, 1.0f };
static const f32 HOST_PERF_RED  [ 4 ] = { 0.93f, 0.33f, 0.27f, 1.0f };
static const f32 HOST_PERF_DARK [ 4 ] = { 0.06f, 0.06f, 0.08f, 1.0f };

void
host_draw_perf_content( i32 win_w, i32 win_h )
{
    UNUSED( win_h );

    int mode = s_host_perf.s_mode;
    if ( mode <= 0 || !draw() )
        return;

    /* Build the visible lines for this tier.  line[0] is the graded FPS row. */
    char line[ 8 ][ 40 ];
    int  n = 0;
    snprintf( line[ n++ ], sizeof( line[ 0 ] ), "HOST %5.1f fps (%5.2f ms)",
              s_host_perf.s_fps, s_host_perf.s_frame_ms );
    if ( mode >= 2 )
    {
        snprintf( line[ n++ ], sizeof( line[ 0 ] ), "frame  %5.2f ms", s_host_perf.s_frame_ms );
        snprintf( line[ n++ ], sizeof( line[ 0 ] ), "work   %5.2f ms", s_host_perf.s_work_ms  );
        snprintf( line[ n++ ], sizeof( line[ 0 ] ), "wait   %5.2f ms", s_host_perf.s_wait_ms  );
    }
    if ( mode >= 3 )
    {
        snprintf( line[ n++ ], sizeof( line[ 0 ] ), "events %5.2f ms", s_host_perf.s_events_ms );
        snprintf( line[ n++ ], sizeof( line[ 0 ] ), "update %5.2f ms", s_host_perf.s_update_ms );
        snprintf( line[ n++ ], sizeof( line[ 0 ] ), "gui    %5.2f ms", s_host_perf.s_gui_ms    );
        snprintf( line[ n++ ], sizeof( line[ 0 ] ), "render %5.2f ms", s_host_perf.s_render_ms );
    }

    /* Scale the 5x7 font up 2x for legibility; widest line drives the panel width. */
    const f32 scale   = 2.0f;
    const f32 line_px = ( f32 )DRAW_FONT_LINE * scale;
    const f32 bpad    = 6.0f;

    f32 text_w = 0.0f;
    for ( int i = 0; i < n; ++i )
    {
        f32 w = draw()->text_width( scale, line[ i ] );
        if ( w > text_w ) text_w = w;
    }
    f32 text_h = ( f32 )n * line_px;

    /* Top-right, mirroring the gui backend's placement; clamp so a tiny window still shows it. */
    f32 bx = ( f32 )win_w - text_w - bpad * 3.0f;
    if ( bx < 8.0f ) bx = 8.0f;
    f32 by = 8.0f;
    f32 bw = text_w + bpad * 2.0f;
    f32 bh = text_h + bpad * 2.0f;

    /* Backdrop first (submission order = paint order; no depth), then text on top. */
    draw()->rect( bx + bw * 0.5f, by + bh * 0.5f, bw, bh, HOST_PERF_DARK );

    f32 tx = bx + bpad;
    f32 ty = by + bpad;

    /* FPS graded by health: >=60 green, >=30 amber, else red. */
    f32 fps = s_host_perf.s_fps;
    const f32* fps_col = fps >= 60.0f ? HOST_PERF_GREEN
                       : fps >= 30.0f ? HOST_PERF_AMBER
                       :                HOST_PERF_RED;
    draw()->text( tx, ty, scale, fps_col, line[ 0 ] );

    for ( int i = 1; i < n; ++i )
        draw()->text( tx, ty + ( f32 )i * line_px, scale, HOST_PERF_WHITE, line[ i ] );
}

// clang-format on
/*============================================================================================*/
