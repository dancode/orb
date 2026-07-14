/*==============================================================================================

    runtime_service/gui/debug/gui_frame_overlay.c -- Built-in perf / state HUD overlays + debug driver.

    Two hidden-chrome debug readouts drawn through the ordinary GUI pipeline, plus the frame-timing
    instrumentation the perf overlay reads.  The timing helpers (perf_frame_begin / perf_frame_end /
    perf_render_begin / perf_render_end) are the private half of this unit: they are called from the
    frame lifecycle in gui_frame.c, which is why this file is included BEFORE gui_frame.c in the
    unity build (gui.c) -- those statics must be in scope where the lifecycle brackets them.

    The overlays are NOT host-called: debug_enable( true ) arms an internal hotkey driver
    (debug_hotkeys, run from frame_begin) that cycles the overlay tiers, and the lifecycle emits
    them into the default context at its ctx_end (debug_overlays_emit).  The host's only jobs are
    debug_enable() and a one-time set_frame_hooks() to hand gui the OS clock / sleep / wait
    callbacks it cannot reach itself; frame_pace() then owns the end-of-loop sleep or idle wait.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Performance overlay

    A built-in, hidden-chrome FPS / cost readout (the host used to hand-roll this).  gui owns no
    clock -- it is a leaf of rhi + app -- so the host hands it a monotonic seconds callback through
    perf_overlay(); gui brackets the frame with it.  The emit clock opens at frame_begin and is
    latched on the first render() of the frame; the render clock sums the render() flush calls.  Both
    raw measurements are folded into smoothed (EMA) readouts at the next frame_begin, so the panel
    trails the work it describes by one frame -- the standard self-measurement lag for an in-frame
    overlay (the build that reads the numbers is also the one being measured).
==============================================================================================*/

static struct
{
    gui_clock_fn    clock;              /* host monotonic seconds source (NULL = timing off) */
    f64             t_emit_start;       /* clock() captured at frame_begin (0 = not armed)    */
    f64             emit_ms;            /* this frame: frame_begin -> first render() (ms)     */
    f64             rend_ms;            /* this frame: accumulated render() wall time (ms)    */
    bool            emit_captured;      /* emit_ms latched on the first render() this frame   */
    f32             fps;                /* smoothed readouts shown by the overlay             */
    f32             s_emit_ms;
    f32             s_rend_ms;

} s_perf;

/* Publish last frame's raw emit/render times into the smoothed readouts and open a fresh emit clock.
   Called from frame_begin (which owns dt -> fps). */
static void
perf_frame_begin( f32 dt )
{
    if ( dt > 0.0f )
    {
        f32 inst = 1.0f / dt;
        s_perf.fps = s_perf.fps <= 0.0f ? inst : s_perf.fps * 0.92f + inst * 0.08f;
    }
    f32 em = (f32)s_perf.emit_ms;
    f32 rm = (f32)s_perf.rend_ms;
    s_perf.s_emit_ms = s_perf.s_emit_ms <= 0.0f ? em : s_perf.s_emit_ms * 0.9f + em * 0.1f;
    s_perf.s_rend_ms = s_perf.s_rend_ms <= 0.0f ? rm : s_perf.s_rend_ms * 0.9f + rm * 0.1f;

    s_perf.emit_ms       = 0.0;
    s_perf.rend_ms       = 0.0;
    s_perf.emit_captured = false;
    s_perf.t_emit_start  = s_perf.clock ? s_perf.clock() : 0.0;
}

/* Close the emit phase at frame_end -- "build cost = frame_begin -> frame_end".  Latches emit_ms
   from the clock armed in perf_frame_begin; idempotent (the first capture this frame wins, so the
   render fallback below is a no-op once this has run). */
static void
perf_frame_end( void )
{
    if ( !s_perf.clock )
        return;
    if ( !s_perf.emit_captured && s_perf.t_emit_start > 0.0 )
    {
        s_perf.emit_ms       = ( s_perf.clock() - s_perf.t_emit_start ) * 1000.0;
        s_perf.emit_captured = true;
    }
}

/* Close the emit phase and return the clock reading to bracket the render flush.  frame_end normally
   latches emit_ms first; this remains as a fallback so timing still reads if frame_end was skipped.
   Returns 0 when timing is off (clock not yet supplied or t_emit_start unarmed). */
static f64
perf_render_begin( void )
{
    if ( !s_perf.clock )
        return 0.0;
    f64 now = s_perf.clock();
    if ( !s_perf.emit_captured && s_perf.t_emit_start > 0.0 )
    {
        s_perf.emit_ms       = ( now - s_perf.t_emit_start ) * 1000.0;
        s_perf.emit_captured = true;
    }
    return now;
}

/* Fold one render() flush span into this frame's render accumulator (t0 from perf_render_begin). */
static void
perf_render_end( f64 t0 )
{
    if ( s_perf.clock && t0 > 0.0 )
        s_perf.rend_ms += ( s_perf.clock() - t0 ) * 1000.0;
}

/* Debug-lever state read by the overlay's status rows below; defined further down this file
   (idle skip) and in gui_frame.c (force redraw) -- forward declarations, same unity TU. */
void gui_set_force_redraw( bool on );
bool gui_force_redraw( void );
bool gui_idle_skip( void );

/* Backing panel behind an overlay's text -- a plain filled rect emitted FIRST inside the region,
   so it draws behind the region's own text but (region z-band) on top of every ordinary window
   beneath it; over a busy editor UI the digits are unreadable without it.  Sized from the region's
   persisted content measure (the same state its w/h <= 0 autosize reads, gui_region.c), so no
   second content-size tracker is needed; the size is last frame's -- one frame of lag, and the
   very first frame draws no backdrop (no measure exists yet). */
static void
overlay_backdrop( gui_id_t id, f32 x, f32 y )
{
    gui_scroll_link_t* scroll = GUI_STATE( gui_scroll_link_t, id );
    if ( scroll->content_w > 0.0f && scroll->content_h > 0.0f )
        gui_draw_rect( x, y, scroll->content_w, scroll->content_h, GUI_COLOR( 0x10, 0x10, 0x14, 0xFF ) );
                       // GUI_COLOR( 0x10, 0x10, 0x14, 0xC8 ) );
}

static void
gui_perf_overlay( int mode )
{
    if ( mode <= 0 )
        return;

    f32 fps = s_perf.fps;

    f32 top_y = 34.0f;
    gui_window_t* mb = window_find( id_hash( "##MainMenuBar" ) );
    if ( mb && mb->last_frame == g_ctx->retained.frame )
        top_y += mb->h;


    float left_x = 8.0f;

    /* A root region: no chrome to hide (no window body/border to paint transparent), fixed
       top-left, hugging its content (w/h <= 0 autosize both axes).  NO_INPUT: pure text readout,
       a region is interactive by default and this one has no business entering the hover_win
       contest or eating the mouse wheel.  DEBUG_BAND: a self-measuring readout -- its own
       ever-changing digits must not count in the stats it displays or poison idle-skip. */
    gui_region_begin( "perf_overlay", left_x, top_y, 0.0f, 0.0f, GUI_REGION_MID,
                      GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT | GUI_WIN_DEBUG_BAND );
    {
        overlay_backdrop( id_hash( "perf_overlay" ), left_x, top_y );
        gui_stack();
        /* FPS, graded by health: >=60 green, >=30 amber, else red. */
        u32 fps_col = fps >= 60.0f ? GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
                    : fps >= 30.0f ? GUI_COLOR( 0xE0, 0xC0, 0x40, 0xFF )
                    :                GUI_COLOR( 0xEE, 0x55, 0x44, 0xFF );
        char line[ 64 ];
        snprintf( line, sizeof( line ), "FPS %5.1f  (%4.2f ms)", fps, fps > 0.0f ? 1000.0f / fps : 0.0f );
        gui_text_colored( fps_col, line );

        bool show_timing_rows = ( mode >= 2 );
        if ( show_timing_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "emit   %5.2f ms", s_perf.s_emit_ms );
            gui_textf( "render %5.2f ms", s_perf.s_rend_ms );
        }

        bool show_geometry_rows = ( mode >= 3 );
        if ( show_geometry_rows )
        {
            gui_render_stats_t rs = gui_render_stats();
            gui_new_line( 2.0f );
            gui_textf( "verts   %6u", rs.vert_count );
            gui_textf( "tris    %6u", rs.tri_count  );
            gui_textf( "batches %6u", rs.draw_calls );
            gui_textf( "cmds    %6u", rs.cmd_count  );

            bool show_retained_rows = ( mode >= 4 );
            if ( show_retained_rows )
            {
                /* Retained-mode stats: how much geometry was reused vs re-tessellated.
                   volatile patched is a separate signal, not folded into wins ret above -- a
                   window holding an animating volatile widget (gui()->volatile_cb) still counts
                   as fully retained; this is what actually moved inside it this frame. */
                gui_new_line( 2.0f );
                gui_textf( "wins ret  %u/%u", rs.win_retained,  rs.win_total   );
                gui_textf( "verts ret %u/%u", rs.vert_retained, rs.vert_count  );
                gui_textf( "tris ret  %u/%u", rs.tri_retained,  rs.tri_count   );
                gui_textf( "vol patch %u",    rs.volatile_patched              );

                /* Upload stats: GPU memory bandwidth. */
                gui_new_line( 2.0f );
                gui_textf( "up batch  %u", rs.upload_batches );
                gui_textf( "up bytes  %u", rs.upload_bytes   );
            }
        }

        /* Debug-lever status (mode >= 3): the emit / tessellation / pacing toggles, live, so
           the console log is not needed to know which regime the numbers above were measured
           in.  Each line names its hotkey.  Fixed-width states keep the footprint stable. */
        bool show_status_rows = ( mode >= 3 );
        if ( show_status_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "emit  %s (F)", gui_force_redraw()        ? "forced  " : "on-dirty" );
            gui_textf( "tess  %s (C)", gui_build_retained_skip() ? "cached  " : "always  " );
            gui_textf( "pace  %s (I)", gui_idle_skip()           ? "idleskip" : "spin    " );
        }
    }
    gui_region_end();
}

/*==============================================================================================
    State overlay

    A built-in text readout of the live interaction state -- hover/active/focused widget, hover
    window, keyboard nav cursor -- resolved to the source label/title string via the id name
    registry (gui_debug_name, gui_debug_overlay.c) instead of a raw hash.  Debug builds populate
    that registry at every id mint point (DBG_NAME in widget_id / window_begin_ex / region /
    child / table); Release builds leave it empty and every id shows as hex, same shape as
    perf_overlay's always-available-but-more-useful-in-Debug pattern.
==============================================================================================*/

/* id -> "name" or "0x########" -- round-robins through a few static scratch buffers so multiple
   ids can be formatted into the same gui_textf() call without clobbering each other. */
static const char*
dbg_id_str( gui_id_t id )
{
    static char   bufs[ 4 ][ 24 ];
    static u32    next = 0;

    if ( id == GUI_ID_NONE ) return "-";

    const char* name = gui_debug_name( id );
    if ( name ) return name;

    char* b = bufs[ next ];
    next    = ( next + 1u ) & 3u;
    snprintf( b, sizeof( bufs[ 0 ] ), "0x%08X", id );
    return b;
}

static void
gui_state_overlay( int mode )
{
    if ( mode <= 0 )
        return;

    f32 top_y = 8.0f;
    gui_window_t* mb = window_find( id_hash( "##MainMenuBar" ) );
    if ( mb && mb->last_frame == g_ctx->retained.frame )
        top_y += mb->h;

    /* Fixed offset to the right of perf_overlay's top-left HUD so both can be shown at once
       without overlap -- perf_overlay hugs its content and stays narrow, so a flat offset is
       simpler than coordinating widths through a shared channel. */
    gui_region_begin( "state_overlay", 260.0f, top_y, 0.0f, 0.0f, GUI_REGION_MID,
                      GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT );
    {
        overlay_backdrop( id_hash( "state_overlay" ), 260.0f, top_y );
        gui_stack();

        gui_textf( "Hover   %s", dbg_id_str( s_interaction.hover_id ) );
        gui_textf( "Active  %s (btn %u)", dbg_id_str( s_interaction.active_id ), s_interaction.active_button );
        gui_textf( "Window  %s", dbg_id_str( s_interaction.hover_win ) );

        bool show_extended_rows = ( mode >= 2 );
        if ( show_extended_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "Focused %s", dbg_id_str( s_interaction.focused_id ) );
            gui_textf( "Nav id  %s", dbg_id_str( g_ctx->nav.id ) );
            gui_textf( "Nav win %s", dbg_id_str( g_ctx->nav.win ) );
            gui_textf( "Mouse   %6.1f, %6.1f", s_io.mouse_x, s_io.mouse_y );
        }

        bool show_popup_rows = ( mode >= 3 );
        if ( show_popup_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "Popups  %u", g_ctx->popup.open_count );
            if ( g_ctx->popup.open_count )
            {
                gui_id_t top_popup = g_ctx->popup.open[ g_ctx->popup.open_count - 1u ].id;
                gui_textf( "Top pop %s", dbg_id_str( top_popup ) );
            }
            gui_textf( "Ctx salt 0x%08X", g_ctx->retained.id_salt );
        }
    }
    gui_region_end();
}

/*==============================================================================================
    Frame hooks -- the host OS services gui cannot reach itself

    gui links only app + rhi (no sys), so the wall clock, the sleep, and the block-on-input wait
    arrive as callbacks, set once after init().  The clock powers the perf overlay's emit/render
    timing; sleep + wait power frame_pace() below.  Any member may be NULL: the dependent feature
    simply switches off (no clock -> timing reads zero; no wait -> idle skip unavailable).
==============================================================================================*/

static gui_sleep_fn       s_hook_sleep;
static gui_wait_events_fn s_hook_wait;

void
gui_set_frame_hooks( gui_clock_fn clock, gui_sleep_fn sleep_ms, gui_wait_events_fn wait_events )
{
    s_perf.clock = clock;        /* adopted by perf_frame_begin / render brackets next frame */
    s_hook_sleep = sleep_ms;
    s_hook_wait  = wait_events;
}

/*==============================================================================================
    Debug driver -- hotkeys + internal overlay emission (armed by debug_enable( true ))

    The debug modes that used to be host-side loop state (perf/state overlay tiers, pipeline
    dashboard, render mode, retained/idle skip) live here, behind hotkeys read from the frame's
    own IO snapshot.  debug_hotkeys() runs from frame_begin after input_frame_begin; the overlays are
    emitted by debug_overlays_emit(), called from ctx_end while the DEFAULT context is still
    bound -- last in its build, so they draw on top and their cost is counted like any widget.

        F1-F5   debug overlay layers (window / interact / resize / clip / layout)
        F8      command stepper: freeze the frame (opens the control window) / release
        F9      render mode: normal -> wireframe -> batch tint
        F10     pipeline dashboard window
        P       perf overlay tier  (off / fps / +timings / +counts / +retained)
        O       state overlay tier (off / ids / +focus,nav / +popups)
        C       retained skip (tessellation cache) on/off -- RENDER-side: off re-tessellates
                every window every frame (geometry), emit skip is untouched
        F       force redraw on/off -- EMIT-side: on pins frame_dirty true so frame_begin never
                skips the widget emit (the "always dirty" lever; see set_force_redraw)
        I       idle skip (frame_pace blocks on OS input when idle) on/off
        , .     command stepper (while frozen): step the replay cursor back / forward
                (repeat-aware, so holding scrubs; shift steps by 16)

    Letter keys are fenced by want_capture_keyboard so typing in a text field never toggles them.

    NOTE (F): a host that writes set_force_redraw itself every frame (sb_gui_editor pins it for
    play mode / always-emit) owns the flag -- its per-frame write overrides the hotkey toggle.
==============================================================================================*/


static int  s_dbg_perf_mode;     /* perf overlay tier, P cycles 0..4                        */
static int  s_dbg_state_mode;    /* state overlay tier, O cycles 0..3                       */
static bool s_dbg_dash_open;     /* pipeline dashboard, F10 toggles (X button writes false) */
static bool s_dbg_step_open;     /* command stepper window, F8 opens (X button hides)       */
static bool s_idle_skip;         /* frame_pace: block on OS input when idle, I toggles      */

/* True while any context that closed this frame still had an animation in flight -- the OR of
   every ctx_end's wants_redraw, reset each frame_begin.  frame_pace reads it to keep pumping
   ~60 Hz frames while a transition settles instead of blocking on input mid-animation. */
static bool s_any_redraw;

/* Programmatic idle-skip control, for hosts that want it on without the hotkey. */
void gui_set_idle_skip( bool on ) { s_idle_skip = on; }
bool gui_idle_skip( void )        { return s_idle_skip; }

/* Poll the debug hotkeys from this frame's IO snapshot.  Called from frame_end (after nav_new_frame
   and all widget emission, so nav/widgets have already consumed any key they use -- see
   gui_want_capture_keyboard) only while debug_enable is on.  Because this now runs AFTER the
   frame's overlay emit, a mode changed here is one frame too late for THIS frame's draw list; each
   branch that mutates a mode requests g_ctx->retained.wants_redraw so frame_begin sees the frame as
   dirty next time round instead of an idle/retained replay silently sitting on the stale mode. */
static void
debug_hotkeys( void )
{
    /* Function keys are never text input -- no keyboard fence needed. */
    if ( gui_is_key_pressed( APP_KEY_F9 ) )
    {
        gui_render_mode_t m = ( gui_render_get_mode() + 1 ) % GUI_RENDER_MODE_COUNT;
        gui_render_set_mode( m );
        static const char* names[] = { "normal", "wireframe", "batch" };
        printf( "[gui] render mode: %s\n", names[ m ] );
        g_ctx->retained.wants_redraw = true;
    }
    if ( gui_is_key_pressed( APP_KEY_F10 ) )
    {
        s_dbg_dash_open = !s_dbg_dash_open;
        printf( "[gui] pipeline dashboard: %s\n", s_dbg_dash_open ? "open" : "closed" );
        g_ctx->retained.wants_redraw = true;
    }

#ifdef GUI_CMD_STEPPER
    /* F8 freezes the current frame's band-0 command list for stepped replay (capture latches and
       is taken at this frame's build), or releases an active freeze back to live emission.
       Freezing also opens the control window (gui_step_window.c); releasing leaves it up, and
       its X button only hides it -- a hidden window never releases the freeze. */
    if ( gui_is_key_pressed( APP_KEY_F8 ) )
    {
        if ( gui_step_frozen() )
        {
            gui_step_release();
            printf( "[gui] command stepper: released\n" );
        }
        else
        {
            gui_step_capture();
            s_dbg_step_open = true;
            printf( "[gui] command stepper: frame frozen (, . step the cursor; shift x16)\n" );
        }
        g_ctx->retained.wants_redraw = true;
    }
#endif

    /* F1-F5 toggle the debug overlay layer mask (window / interact / resize / clip / layout).  Read
       from the frame IO here like every other debug hotkey -- initial-press only, so holding a key no
       longer flickers the layer -- rather than the old event-time path in gui_event (foundation/
       gui_io.c), which was a second, separately-fenced consumption channel for the same feature.  The
       layer setters compile to no-ops in Release (backend/gui_debug_overlay.c), so no build guard is
       needed here.  Function keys, so no want_capture_keyboard fence -- they are never text input. */
    {
        static const struct { app_key_t key; u32 layer; } k_dbg_layer[] = {
            { APP_KEY_F1, GUI_DBG_WINDOW   }, { APP_KEY_F2, GUI_DBG_INTERACT },
            { APP_KEY_F3, GUI_DBG_RESIZE   }, { APP_KEY_F4, GUI_DBG_CLIP     },
            { APP_KEY_F5, GUI_DBG_LAYOUT   },
        };
        for ( u32 i = 0; i < sizeof( k_dbg_layer ) / sizeof( k_dbg_layer[ 0 ] ); ++i )
            if ( gui_is_key_pressed( k_dbg_layer[ i ].key ) )
            {
                gui_debug_set_layers( gui_debug_get_layers() ^ k_dbg_layer[ i ].layer );
                g_ctx->retained.wants_redraw = true;
            }
    }

    /* Letter keys: fenced so a focused text field owns them. */
    if ( gui_want_capture_keyboard() )
        return;

    if ( gui_is_key_pressed( APP_KEY_P ) )
    {
        s_dbg_perf_mode = ( s_dbg_perf_mode + 1 ) % 5;
        g_ctx->retained.wants_redraw = true;
    }

    if ( gui_is_key_pressed( APP_KEY_O ) )
    {
        s_dbg_state_mode = ( s_dbg_state_mode + 1 ) % 4;
        g_ctx->retained.wants_redraw = true;
    }

    if ( gui_is_key_pressed( APP_KEY_C ) )
    {
        bool on = !gui_build_retained_skip();
        gui_build_set_retained_skip( on );
        printf( "[gui] retained skip: %s\n", on ? "on (skip tess if unchanged)" : "off (always tess)" );
        g_ctx->retained.wants_redraw = true;
    }
    if ( gui_is_key_pressed( APP_KEY_F ) )
    {
        bool on = !gui_force_redraw();
        gui_set_force_redraw( on );
        printf( "[gui] force redraw: %s\n", on ? "on (always emit, frame_dirty pinned)"
                                               : "off (skip emit on clean frames)" );
        g_ctx->retained.wants_redraw = true;
    }
    if ( gui_is_key_pressed( APP_KEY_I ) )
    {
        s_idle_skip = !s_idle_skip;
        printf( "[gui] idle skip: %s\n", s_idle_skip ? "on (block on input)" : "off (spin)" );
        g_ctx->retained.wants_redraw = true;
    }

#ifdef GUI_CMD_STEPPER
    /* , . step the frozen replay cursor (repeat-aware so holding scrubs; shift steps by 16).
       The seek latches -- it applies at the next frame's restore -- so wants_redraw is required
       here or the clean-frame emit skip would sit on the stale cursor (the deferred-update rule). */
    if ( gui_step_frozen() )
    {
        u32  stride = ( gui_is_key_down( APP_KEY_LSHIFT ) || gui_is_key_down( APP_KEY_RSHIFT ) )
                          ? 16u : 1u;
        bool back   = gui_is_key_pressed_repeat( APP_KEY_COMMA );
        bool fwd    = gui_is_key_pressed_repeat( APP_KEY_PERIOD );
        if ( back || fwd )
        {
            u32 c = gui_step_cursor();
            if ( back )
                c = c > stride ? c - stride : 0u;
            else
                c = c + stride;               /* seek clamps to the frozen command count */
            gui_step_seek( c );
            printf( "[gui] command stepper: %u/%u\n", gui_step_cursor(), gui_step_count() );
            g_ctx->retained.wants_redraw = true;
        }
    }
#endif
}

/* Emit the debug overlays into the currently bound (default) context -- called from ctx_end
   before it rebinds, so this is exactly where a host used to hand-place them: last in the
   default context's build, drawing on top of everything it emitted. */
static void
debug_overlays_emit( void )
{
    gui_pipeline_dashboard( &s_dbg_dash_open );
    gui_step_window( &s_dbg_step_open );
    gui_perf_overlay( s_dbg_perf_mode );
    gui_state_overlay( s_dbg_state_mode );
}

/* NOTE: gui_frame_pace() -- the end-of-loop idle sleep -- moved to gui_boot.c, the boot-tier
   loop it belongs to.  It still reads the frame hooks (s_hook_sleep/wait) and s_idle_skip set
   here and s_any_redraw folded in gui_frame.c; the gui.c unity includes gui_boot.c last, so
   those statics are all in scope there. */

// clang-format on
/*============================================================================================*/
