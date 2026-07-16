/*==============================================================================================

    run_perf.c -- Host-side diagnostics: perf HUD (draw backend) + profiler capture verbs.

    The runtime host owns a per-phase frame profiler the gui knows nothing about: the integer
    microsecond timings stamped through the main loop (events / update / gui / render / work /
    wait / frame) and published via run()->frame_stats(), plus the authoritative frame clock
    (run()->clock()).  This reads the HOST loop's real phase breakdown -- the numbers that tell
    you where a frame's milliseconds actually went, engine-wide, not just inside the UI.

    It renders through the draw service's built-in 5x7 bitmap font (draw_font.c) -- no atlas, no
    gui.  That is deliberate: an earlier gui-drawn version of this overlay was removed because a
    gui readout is stale by design (its DEBUG_BAND region is exempt from dirty tracking and the
    retained cache replays last frame's geometry on clean frames), so it froze on exactly the
    light-gui frames you most want to measure.  The draw backend samples and paints every frame
    the overlay is active, outside all of gui's dirty / cache logic -- always live, always precise.
    (gui_frame_overlay.c still carries gui's OWN self-measured emit/render overlay; this is the
    orthogonal host-loop view.)

    Two entry points, called from the host loop:

        host_perf_tick()         -- poll the toggle (F8) + fold this frame's stats.  Once per
                                    frame, unconditionally.
        host_draw_perf_content() -- paint the readout (backdrop rect + bitmap text) into an
                                    already-open draw pass at a render composite point.

    plus host_perf_active(), so the host can skip opening a composite pass it would fill with
    nothing.  The profiler capture verbs (prof_dump / prof_hitch) and the sys/app key-table
    invariants live at the bottom of this unit -- the host loop's diagnostic machinery, kept
    out of run_host.c.  Toggle HOST_PERF_KEY (F8) cycles the tier:

        0  off
        1  FPS + frame period
        2  + frame / work / wait split
        3  + per-phase breakdown (events / update / gui / render)

    A function key is used (not a letter) so no keyboard-capture fence is needed -- it reads the
    app input snapshot directly (app()->key_pressed), which works with or without gui.

    See [[project_run_host_gui_loop]] for the host loop this hangs off and draw_font.c for the
    bitmap font.

==============================================================================================*/
// clang-format off

#define HOST_PERF_KEY   APP_KEY_F8   /* cycles the perf overlay tier (0..3) */

/*==============================================================================================
    Readout state -- EMA-folded so the panel is legible instead of a blur of jitter.
    Same one-frame self-measurement lag as any in-frame overlay (it reads last frame's stats).
==============================================================================================*/

static struct
{
    int s_mode;      /* perf overlay tier, HOST_PERF_KEY cycles 0..3     */
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

/* Poll the toggle and fold this frame's stats.  Called once per frame from the host loop. */
void
host_perf_tick( void )
{
    if ( !run() )
        return;

    /* Function key -- never text input, so no keyboard-capture fence. */
    if ( app() && app()->key_pressed( HOST_PERF_KEY ) )
    {
        s_host_perf.s_mode = ( s_host_perf.s_mode + 1 ) % 4;
        printf( "[host] perf overlay: tier %d\n", s_host_perf.s_mode );
    }

    host_perf_sample();
}

/* True while the overlay is on (tier > 0).  Lets the host skip opening a composite pass it would
   fill with nothing. */
bool
host_perf_active( void )
{
    return s_host_perf.s_mode > 0;
}

/*==============================================================================================
    Paint -- backdrop rect + bitmap text through the draw service's built-in 5x7 font.  The
    caller has already opened a draw pass (begin_overlay / begin_pass), so this manages no pass
    state; it only lays down geometry.
==============================================================================================*/

/* Linear rgba palette (draw takes f32[4], not packed abgr). */
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

    /* Top-right; clamp so a tiny window still shows it. */
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

/*==============================================================================================
    Profiler capture verbs -- registered by the host loop via host_prof_commands_register().

    "prof_dump [frames] [path]"
        Starts a Chrome-trace capture of the next N frames (default 120) into path (default
        artifacts/prof_dump.json); load the file in chrome://tracing or
        https://ui.perfetto.dev.  While active the loop drains every frame through
        host_prof_frame_flush -- the host is the profiler's single drain consumer.
        Re-running the command stops an active capture early.

    "prof_hitch [ms] [prefix]"
        Arms the profiler's hitch capture: every frame host_prof_hitch_frame folds events
        into the rolling history, and any frame whose WORK time (pacing wait excluded)
        crosses the threshold (default 33.3 ms -- two missed 60 Hz periods) auto-writes
        "<prefix>_<frame>.json" (default prefix artifacts/hitch).  Re-running disarms.

    Both default into artifacts/ -- the repo's gitignored dump folder for generated files --
    created on demand so a fresh checkout works.
==============================================================================================*/

static i32 s_prof_dump_frames = 0;    /* frames left in an active capture; 0 = idle */

static void
host_cmd_prof_dump( int argc, char** argv )
{
    if ( prof_dump_active() )
    {
        prof_dump_end();
        s_prof_dump_frames = 0;
        printf( "[host] prof_dump: capture stopped early\n" );
        return;
    }

    i32         frames = argc > 1 ? atoi( argv[ 1 ] ) : 0;
    const char* path   = argc > 2 ? argv[ 2 ] : "artifacts/prof_dump.json";
    if ( frames <= 0 )
        frames = 120;

    if ( argc <= 2 )
        sys_dir_make( "artifacts" );

    if ( !prof_dump_begin( path ) )
    {
        printf( "[host] prof_dump: cannot open '%s'\n", path );
        return;
    }

    s_prof_dump_frames = frames;
    printf( "[host] prof_dump: capturing %d frames to '%s'\n", frames, path );
}

static void
host_cmd_prof_hitch( int argc, char** argv )
{
    if ( prof_hitch_armed() )
    {
        prof_hitch_arm( 0.0, NULL );
        printf( "[host] prof_hitch: disarmed\n" );
        return;
    }

    f64         ms     = argc > 1 ? atof( argv[ 1 ] ) : 0.0;
    const char* prefix = argc > 2 ? argv[ 2 ] : "artifacts/hitch";
    if ( ms <= 0.0 )
        ms = 33.3;

    if ( argc <= 2 )
        sys_dir_make( "artifacts" );

    prof_hitch_arm( ms, prefix );
    if ( prof_hitch_armed() )
        printf( "[host] prof_hitch: armed at %.1f ms (captures to '%s_<frame>.json')\n", ms, prefix );
    else
        printf( "[host] prof_hitch: unavailable (ORB_PROFILE_HITCH compiled out)\n" );
}

/* Install both verbs; called once at boot after core's cmd system is live. */
void
host_prof_commands_register( void )
{
    s_prof_dump_frames = 0;
    cmd_register( "prof_dump", host_cmd_prof_dump,
                  "Capture N frames to a Chrome trace (prof_dump [frames] [path])" );
    cmd_register( "prof_hitch", host_cmd_prof_hitch,
                  "Auto-capture a trace when frame work exceeds a threshold (prof_hitch [ms] [prefix])" );
}

/* Drain an active prof_dump capture: move this frame's events (all rings) into the trace
   file, closing it on the last frame.  Called once per frame by the host loop; a cheap
   branch while idle. */
void
host_prof_frame_flush( void )
{
    if ( s_prof_dump_frames <= 0 )
        return;

    prof_dump_flush();
    if ( --s_prof_dump_frames == 0 )
    {
        prof_dump_end();
        printf( "[host] prof_dump: capture complete\n" );
    }
}

/* Feed the hitch monitor this frame's WORK milliseconds (pacing wait excluded -- deliberate
   idling is not a hitch).  Two cheap branches per frame while disarmed. */
void
host_prof_hitch_frame( f64 work_ms )
{
    u32 hitch_events = prof_hitch_update( work_ms );
    if ( hitch_events )
        printf( "[host] prof_hitch: %.1f ms frame -- wrote '%s' (%u events)\n",
                work_ms, prof_hitch_last_path(), hitch_events );
}

/*==============================================================================================
    Console / windowed key-table agreement

    sys_key_t values are pinned to app_key_t for the shared range so console-input polling
    (sys_key_pressed) and windowed input (app()->key_pressed) agree on key constants -- the
    host loop and its dev hotkeys rely on it.  C5287 (newer MSVC) flags the mixed-enum
    comparison even through the casts; comparing the two tables is the whole point here, so
    it is silenced for this block only.
==============================================================================================*/

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

// clang-format on
/*============================================================================================*/
