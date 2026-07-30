/*==============================================================================================

    runtime_service/gui/frame/gui_frame_overlay.c -- Built-in perf / state HUD overlays + debug driver.

    Two hidden-chrome debug readouts drawn through the ordinary GUI pipeline, plus the frame-timing
    instrumentation the perf overlay reads.  The timing helpers (perf_frame_begin / perf_frame_end /
    perf_render_begin / perf_render_end / perf_present_begin / perf_present_end) are the private half
    of this unit: they are called from the frame lifecycle in gui_frame_loop.c (emit / render) and
    the boot present pair in gui_boot.c (present), which is why this file is included BEFORE both in
    the gui_frame.c unity build -- those statics must be in scope where the lifecycle brackets them.

    The overlays are NOT host-called: debug_enable( true ) arms an internal hotkey driver
    (debug_hotkeys, run from frame_begin) that cycles the overlay tiers, and the lifecycle emits
    them into the default context at its ctx_end (debug_overlays_emit).  The host's only jobs are
    debug_enable() and a one-time frame_set_hooks() to hand gui the OS clock / sleep / wait
    callbacks it cannot reach itself; frame_pace() then owns the end-of-loop sleep or idle wait.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Performance overlay

    A built-in, hidden-chrome FPS / cost readout (the host used to hand-roll this).  gui owns no
    clock -- it is a leaf of rhi + app -- so the host hands it a monotonic seconds callback through
    perf_overlay(); gui brackets the frame with it.  The emit clock opens at frame_begin and is
    latched on the first render() of the frame; the render clock sums the render() flush calls; the
    present clock (boot path only) times the present pair and subtracts the render flush already
    counted, so it reports the non-render present overhead -- dominated by the frame_begin fence wait
    (GPU backpressure).  emit + render + present then account for the whole CPU frame.  All three
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
    f64             t_present_start;    /* clock() at boot_present_begin entry (0 = not armed) */
    f64             pres_ms;            /* this frame: present pair wall minus render (ms)    */
    f32             fps;                /* smoothed readouts shown by the overlay             */
    f32             s_emit_ms;
    f32             s_rend_ms;
    f32             s_pres_ms;
    f32             s_poll_ms;          /* smoothed boot_poll (pump + input) span            */
    f32             s_wait_ms;          /* smoothed frame_pace sleep / idle wait span         */

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
    f32 pm = (f32)s_perf.pres_ms;
    s_perf.s_emit_ms = s_perf.s_emit_ms <= 0.0f ? em : s_perf.s_emit_ms * 0.9f + em * 0.1f;
    s_perf.s_rend_ms = s_perf.s_rend_ms <= 0.0f ? rm : s_perf.s_rend_ms * 0.9f + rm * 0.1f;
    s_perf.s_pres_ms = s_perf.s_pres_ms <= 0.0f ? pm : s_perf.s_pres_ms * 0.9f + pm * 0.1f;

    s_perf.emit_ms         = 0.0;
    s_perf.rend_ms         = 0.0;
    s_perf.pres_ms         = 0.0;
    s_perf.emit_captured   = false;
    s_perf.t_present_start = 0.0;
    s_perf.t_emit_start    = s_perf.clock ? s_perf.clock() : 0.0;
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

/* Present bracket -- boot-tier only (gui_boot_present_begin / _end, gui_boot.c).  Arm the
   clock at begin entry so the span covers the whole pair: floater reconcile, the frame_begin
   fence wait (CPU parks on GPU completion here), swapchain acquire, the gui_render flush, submit,
   present, and floater presents.  At the end's exit, subtract rend_ms -- the render flush is shown
   on its own row and lives inside this span -- leaving the NON-render present overhead, which is
   dominated by the fence wait (GPU backpressure).  emit + render + present then sum to the CPU frame.
   Runtime-path hosts never call the present pair, so pres_ms stays 0 and the row reads zero for them. */
static void
perf_present_begin( void )
{
    s_perf.t_present_start = s_perf.clock ? s_perf.clock() : 0.0;
}

static void
perf_present_end( void )
{
    if ( !s_perf.clock || s_perf.t_present_start <= 0.0 )
        return;
    f64 span = ( s_perf.clock() - s_perf.t_present_start ) * 1000.0 - s_perf.rend_ms;
    s_perf.pres_ms = span > 0.0 ? span : 0.0;   /* clamp: render flush can't exceed the pair span */
}

/* Poll + pace brackets (gui_boot_poll / gui_frame_pace, gui_boot.c).  The poll row is boot-path
   only; the pace row appears for any host that calls frame_pace.  These are the
   last two unmeasured phases of the loop: boot_poll (OS pump + gamepad + input snapshot) and
   frame_pace (the end-of-loop sleep / idle wait).  The pace span is the "wait time" -- a
   frame_pace(4,16) sleep otherwise hides ~4 ms from the breakdown, which is exactly what made the
   totals not add up.  With these, emit + render + present + poll + wait accounts for the whole
   frame.  Both are single spans per frame (unlike render's accumulator), so they EMA directly at
   close via perf_span_ema -- open() returns the clock, close folds the span into *dst. */
static f64
perf_span_open( void )
{
    return s_perf.clock ? s_perf.clock() : 0.0;
}

static void
perf_span_ema( f32* dst, f64 t0 )
{
    if ( !s_perf.clock || t0 <= 0.0 )
        return;
    f32 ms = ( f32 )( ( s_perf.clock() - t0 ) * 1000.0 );
    *dst = ( *dst <= 0.0f ) ? ms : *dst * 0.9f + ms * 0.1f;
}

/* The debug-lever state read by the overlay's status rows below -- gui_set_force_redraw /
   gui_force_redraw (frame/gui_frame_loop.c) and gui_idle_skip (further down this file) -- is declared
   on the frame unit's public face (gui_host.h), in scope here via the render header. */

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
overlay_perf( int mode )
{
    if ( mode <= 0 )
        return;

    f32 fps = s_perf.fps;

    /* Below the viewport's chrome, not below a hardcoded guess at it: gui_viewport_content_y is
       where host content starts -- the native caption band when the surface is gui-shelled
       (borderless) plus the main menu bar on frames one is emitted -- the same work top the
       maximize pin and the window drag clamp use.  Adding the bar HEIGHT to a fixed offset (what
       this did) ignores where the bar actually sits, so every overlay landed ON the caption band
       of a borderless window instead of under the bar below it. */
    f32 top_y = gui_viewport_content_y( 0 ) + 34.0f;

    float left_x = 8.0f;

    /* A root region: no chrome to hide (no window body/border to paint transparent), fixed
       top-left, hugging its content (w/h <= 0 autosize both axes).  NO_INPUT: pure text readout,
       a region is interactive by default and this one has no business entering the hover_win
       contest or eating the mouse wheel.  DEBUG_BAND: a self-measuring readout -- its own
       ever-changing digits must not count in the stats it displays or poison idle-skip.
       GUI_REGION_FG: the diagnostic HUD renders in the foreground band (above every popup depth
       AND a GUI_WIN_MODAL overlay window like the dev console), so it is never occluded -- a debug
       readout you cannot see is useless. */
    gui_region_begin( "perf_overlay", left_x, top_y, 0.0f, 0.0f, GUI_REGION_FG,
                      GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT | GUI_WIN_DEBUG_BAND );
    {
        overlay_backdrop( id_hash( "perf_overlay" ), left_x, top_y );
        gui_stack();
        gui_scale_push( GUI_SCALE_DENSE );   /* tight row pitch -- a HUD, not a form */

        /* FPS, graded by health: >=60 green, >=30 amber, else red. */
        u32 fps_col = fps >= 60.0f ? GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
                    : fps >= 30.0f ? GUI_COLOR( 0xE0, 0xC0, 0x40, 0xFF )
                    :                GUI_COLOR( 0xEE, 0x55, 0x44, 0xFF );
        char line[ 64 ];
        fmt_snprintf( line, sizeof( line ), "FPS %5.1f  (%4.2f ms)", fps, fps > 0.0f ? 1000.0f / fps : 0.0f );
        gui_text_colored( fps_col, line );

        bool show_timing_rows = ( mode >= 2 );
        if ( show_timing_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "emit    %5.2f ms", s_perf.s_emit_ms );
            gui_textf( "render  %5.2f ms", s_perf.s_rend_ms );

            /* Full loop breakdown -- tier 2 ONLY.  present = non-render present overhead (fence
               wait + acquire + submit + present); poll = OS pump + input; wait = frame_pace sleep /
               idle (the "wait time" -- a paced loop's sleep shows here instead of hiding).  total
               sums the five phases and should track the FPS ms above (small residual = loop
               arithmetic + self-measurement lag).  Tiers 3+ trade all this for the deep geometry /
               pool stats below, where these fence/sleep-dominated numbers are just noise. */
            if ( mode == 2 )
            {
                gui_textf( "present %5.2f ms", s_perf.s_pres_ms );
                gui_textf( "poll    %5.2f ms", s_perf.s_poll_ms );
                gui_textf( "wait    %5.2f ms", s_perf.s_wait_ms );
                gui_textf( "total   %5.2f ms", s_perf.s_emit_ms + s_perf.s_rend_ms
                                             + s_perf.s_pres_ms + s_perf.s_poll_ms
                                             + s_perf.s_wait_ms );
            }
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
            gui_textf( "clips   %6u", rs.clip_count );

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
                gui_textf( "vol rows  %u/%u", volatile_row_count(), GUI_MAX_VOLATILE );

                /* Upload stats: GPU memory bandwidth. */
                gui_new_line( 2.0f );
                gui_textf( "up batch  %u", rs.upload_batches );
                gui_textf( "up bytes  %u", rs.upload_bytes   );

                /* Keyed state pool load per class: live (touched within a frame) / occupied
                   (live + unreclaimed tombstones) / capacity.  The partition-tuning metric. */
                gui_state_usage_t su = gui_state_usage();
                gui_new_line( 2.0f );
                gui_textf( "st tiny  %u/%u/%u", su.tiny_live,  su.tiny_used,  su.tiny_cap  );
                gui_textf( "st small %u/%u/%u", su.small_live, su.small_used, su.small_cap );
                gui_textf( "st big   %u/%u/%u", su.big_live,   su.big_used,   su.big_cap   );

                /* Fixed-pool pressure: used vs cap for the per-frame emit pools that fail
                   silently (or nearly so) when they fill.  Watch these approach their caps
                   under load and raise the caps BEFORE labels drop / clips go wrong / nav
                   items fall off the list.  nav is this frame's live count (the overlay
                   emits last, after every window has registered its items). */
                gui_new_line( 2.0f );
                gui_textf( "cmds  %u/%u", rs.cmd_count,      (u32)GUI_MAX_CMDS       );
                gui_textf( "segs  %u/%u", rs.seg_count,      (u32)GUI_MAX_SEGS       );
                gui_textf( "clips %u/%u", rs.clip_count,     (u32)GUI_MAX_CLIP_RECTS );
                gui_textf( "text  %u/%u", rs.text_pool_used, (u32)GUI_MAX_TEXT_POOL  );
                gui_textf( "nav   %u/%u", g_ctx->nav.item_count, (u32)GUI_NAV_ITEMS_MAX );
            }
        }

        /* Debug-lever status (mode >= 3): the emit / tessellation / pacing toggles, live, so
           the console log is not needed to know which regime the numbers above were measured
           in.  Toggled from the selector menu (right edge of the viewport), not a hotkey of
           their own.  Fixed-width states keep the footprint stable. */
        bool show_status_rows = ( mode >= 3 );
        if ( show_status_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "emit  %s", gui_force_redraw()        ? "forced  " : "on-dirty" );
            gui_textf( "tess  %s", build_retained_skip() ? "cached  " : "always  " );
            gui_textf( "pace  %s", gui_idle_skip()           ? "idleskip" : "spin    " );
        }

        gui_scale_pop();
    }
    gui_region_end();
}

/*==============================================================================================
    State overlay

    A built-in text readout of the live interaction state -- hover/active/focused widget, hover
    window, keyboard nav cursor -- resolved to the source label/title string via the id name
    registry (gui_debug_name, gui_debug_overlay.c) instead of a raw hash.  Debug builds populate
    that registry at every id mint point (DBG_NAME in item_id / window_begin_ex / region /
    child / table); Release builds leave it empty and every id shows as hex, same shape as
    perf_overlay's always-available-but-more-useful-in-Debug pattern.
==============================================================================================*/

/* id -> "name" or "0x########" -- round-robins through a few static scratch buffers so multiple
   ids can be formatted into the same gui_textf() call without clobbering each other. */
static const char*
overlay_id_str( gui_id_t id )
{
    static char   bufs[ 4 ][ 24 ];
    static u32    next = 0;

    if ( id == GUI_ID_NONE ) return "-";

    const char* name = gui_debug_name( id );
    if ( name ) return name;

    char* b = bufs[ next ];
    next    = ( next + 1u ) & 3u;
    fmt_snprintf( b, sizeof( bufs[ 0 ] ), "0x%08X", id );
    return b;
}

static void
overlay_state( int mode )
{
    if ( mode <= 0 )
        return;

    /* Same work top + base offset as perf_overlay, so the two HUDs share one top edge. */
    f32 top_y = gui_viewport_content_y( 0 ) + 34.0f;

    /* Fixed offset to the right of perf_overlay's top-left HUD so both can be shown at once
       without overlap -- perf_overlay hugs its content and stays narrow, so a flat offset is
       simpler than coordinating widths through a shared channel. */
    /* GUI_REGION_FG: foreground band, above popups and the modal console -- see perf_overlay. */
    gui_region_begin( "state_overlay", 260.0f, top_y, 0.0f, 0.0f, GUI_REGION_FG,
                      GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT );
    {
        overlay_backdrop( id_hash( "state_overlay" ), 260.0f, top_y );
        gui_stack();
        gui_scale_push( GUI_SCALE_DENSE );   /* tight row pitch -- a HUD, not a form */

        gui_textf( "Hover   %s", overlay_id_str( s_interaction.hover_id ) );
        gui_textf( "Active  %s (btn %u)", overlay_id_str( s_interaction.active_id ), s_interaction.active_button );
        gui_textf( "Window  %s", overlay_id_str( s_interaction.hover_win ) );

        bool show_extended_rows = ( mode >= 2 );
        if ( show_extended_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "Focused %s", overlay_id_str( s_interaction.focused_id ) );
            gui_textf( "Nav id  %s", overlay_id_str( g_ctx->nav.id ) );
            gui_textf( "Nav win %s", overlay_id_str( g_ctx->nav.win ) );
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
                gui_textf( "Top pop %s", overlay_id_str( top_popup ) );
            }
            gui_textf( "Ctx salt 0x%08X", g_ctx->retained.id_salt );
        }

        gui_scale_pop();
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
gui_frame_set_hooks( gui_clock_fn clock, gui_sleep_fn sleep_ms, gui_wait_events_fn wait_events )
{
    s_perf.clock = clock;        /* adopted by perf_frame_begin / render brackets next frame */
    s_hook_sleep = sleep_ms;
    s_hook_wait  = wait_events;
}

/*==============================================================================================
    Debug driver -- hotkeys + internal overlay emission (armed by debug_enable( true ))

    The debug modes that used to be host-side loop state (perf/state overlay tiers, pipeline
    dashboard, render mode, retained/idle skip) live here, behind hotkeys read from the frame's
    own IO snapshot.  debug_hotkeys() runs from frame_begin after io_frame_begin; the overlays are
    emitted by debug_overlays_emit(), called from ctx_end while the DEFAULT context is still
    bound -- last in its build, so they draw on top and their cost is counted like any widget.

    Every hotkey below is gated behind a master ARM so the broad single-letter keys never fire
    during normal use:

        NP_DOT  master arm ('.'): toggle EVERY debug hotkey on / off as a group.  Off by default;
                everything below is inert until it is armed, and disarming resets every debug mode
                back to normal (overlays off, render mode normal, layers cleared).  The main-row
                '.' arms too (laptop keyboards), except while a stepper freeze owns it for scrub.
        NP1-NP6 debug layers (window / interact / resize / layout / clip / content rects)
        F8      command stepper: show / hide the control window (Capture there freezes the frame)
        F9      render mode: normal -> wireframe -> batch tint
        F10     pipeline dashboard window
        NP+     perf overlay tier  (off / fps / +timings / +counts / +retained)
        NP-     state overlay tier (off / ids / +focus,nav / +popups)
        , .     command stepper (while frozen): step the replay cursor back / forward
                (repeat-aware, so holding scrubs; shift steps by 16)
                (picking a command under the mouse is the stepper window's Pick toggle -- a
                hotkey fought the focused window's keyboard nav / type-ahead)

    While armed, a dense checkbox-list selector (debug_selector_menu, right edge of the
    viewport) is also up, mirroring NP+ / NP- as sliders alongside the levers that no longer have
    keys of their own: retained skip (tessellation cache), force redraw, and idle skip -- toggled
    there now instead of the old C / F / I letters. It is part of debug rendering, so it never
    perturbs perf-stats or counts (same GUI_WIN_DEBUG_BAND exemption as the overlays).

    Letter and numpad keys are fenced by want_capture_keyboard so typing in a text field never
    toggles them (numpad digits are text input with Num Lock on).

    NOTE (F): a host that writes set_force_redraw itself every frame (sb_gui_editor pins it for
    play mode / always-emit) owns the flag -- its per-frame write overrides the hotkey toggle.
==============================================================================================*/


/* The master switch everything in this section is gated on: gui_debug_enable( true ) opts a host
   into the debug driver at all, and NP_DOT then arms the individual hotkeys below. */

static bool s_debug_enabled;

void gui_debug_enable( bool enable )
{
    s_debug_enabled = enable;
    if ( enable )
        printf( "[gui] debug driver on -- press '.' (main row or numpad) to arm the debug hotkeys\n" );
}

bool gui_debug_is_enabled( void ) { return s_debug_enabled; }

static int  s_dbg_perf_mode;     /* perf overlay tier, NP_ADD cycles 0..4                   */
static int  s_dbg_state_mode;    /* state overlay tier, NP_SUB cycles 0..3                  */
static bool s_dbg_dash_open;     /* pipeline dashboard, F10 toggles (X button writes false) */
static bool s_dbg_step_open;     /* command stepper window, F8 opens (X button hides)       */
static bool s_idle_skip;         /* frame_pace: block on OS input when idle, selector menu toggles */
static bool s_dbg_hotkeys_armed; /* master arm: every hotkey below is inert until NP_DOT arms it */

/* Query for hosts that own a debug lever themselves (e.g. sb_gui_editor's own set_force_redraw
   write for its scene pass): while armed, the selector menu's checkboxes are the sole owner of
   force redraw / retained skip / idle skip, so a host's own per-frame write should stand down and
   let the menu's value stick instead of fighting it every frame it changes. */
bool gui_debug_hotkeys_armed( void ) { return s_dbg_hotkeys_armed; }

/* Remembered selector-menu lever values -- snapshotted by debug_reset() when the arm goes off
   (so disarming can still force the live flags back to normal) and re-applied by debug_restore()
   when the arm goes back on, so reopening the menu picks up exactly where it left off instead of
   the arm's "normal" defaults.  Perf/state tier and idle skip need no separate shadow: they are
   plain local state (or already left alone -- idle skip) that debug_reset() no longer touches, so
   they are already sitting at their last value when the menu reopens. */
static bool s_dbg_force_redraw_saved;
static bool s_dbg_retained_skip_saved = true;   /* default: cached (skip tess when unchanged) */

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
/* Return every debug mode to normal -- called when the master arm is switched off so disarming
   visibly clears the screen (overlays, selector menu, layer rects, render mode) and returns the
   live perf levers to their defaults, rather than leaving whatever was toggled on frozen in
   place.  The two levers that are real engine flags (force redraw, retained skip) are snapshotted
   into s_dbg_*_saved first so debug_restore() can put them back on re-arm -- everything else the
   selector menu shows (perf/state tier, idle skip) is already plain local state debug_reset()
   does not touch, so it is untouched here too and simply sits at its last value until the arm's
   gate (debug_overlays_emit / gui_idle_skip) hides its effect meanwhile. */
static void
debug_reset( void )
{
    s_dbg_dash_open  = false;  /* dashboard closed  */
    s_dbg_step_open  = false;  /* stepper window closed */

    gui_render_set_mode( GUI_RENDER_NORMAL );   /* wireframe / batch tint -> normal */
    gui_debug_set_layers( 0 );                  /* clear all NP1-7 layer rects      */

    s_dbg_retained_skip_saved = build_retained_skip();
    s_dbg_force_redraw_saved  = gui_force_redraw();
    build_set_retained_skip( true );        /* normal: skip tess when unchanged */
    gui_set_force_redraw( false );              /* normal: allow clean-frame emit skip */

#ifdef GUI_CMD_STEPPER
    if ( step_frozen() )
        step_release();                     /* unfreeze back to live emission */
#endif

    redraw_request();
}

/* Put the remembered selector-menu lever values back -- called when the master arm is switched
   back on, so the panel (and the behavior it drives) reopens exactly as the user left it instead
   of debug_reset()'s normal defaults.  Perf/state tier and idle skip need no restore call: they
   were never reset, so they are already correct. */
static void
debug_restore( void )
{
    build_set_retained_skip( s_dbg_retained_skip_saved );
    gui_set_force_redraw( s_dbg_force_redraw_saved );
}

static void
debug_hotkeys( void )
{
    /* Master arm: numpad '.' (APP_KEY_NP_DOT) is the one always-live debug key -- it gates every
       other hotkey below so the broad single-letter (C/F/I/P/O) and function keys are inert during
       normal use and only respond after an explicit opt-in.  Disarming resets every debug mode to
       normal (debug_reset), so one press returns the view to a clean state.  Fenced by
       want_capture_keyboard like the letter keys (numpad '.' is text input with Num Lock on), so it
       never fires while a text field is focused.  Chosen because it is rarely bound elsewhere.
       The MAIN-ROW '.' arms too -- laptop keyboards have no numpad -- except while a stepper
       freeze is active, where '.' is the scrub-forward key below and owns the row; NP_DOT still
       disarms during a freeze. */
    bool arm_toggle = gui_is_key_pressed( APP_KEY_NP_DOT );
#ifdef GUI_CMD_STEPPER
    if ( !step_frozen() )
#endif
        arm_toggle = arm_toggle || gui_is_key_pressed( APP_KEY_PERIOD );
    if ( !gui_want_capture_keyboard() && arm_toggle )
    {
        s_dbg_hotkeys_armed = !s_dbg_hotkeys_armed;
        printf( "[gui] debug hotkeys: %s\n", s_dbg_hotkeys_armed ? "ARMED" : "off" );
        if ( s_dbg_hotkeys_armed )
            debug_restore();
        else
            debug_reset();
        redraw_request();
    }
    if ( !s_dbg_hotkeys_armed )
        return;

    /* Function keys are never text input -- no keyboard fence needed. */
    if ( gui_is_key_pressed( APP_KEY_F9 ) )
    {
        gui_render_mode_t m = ( gui_render_get_mode() + 1 ) % GUI_RENDER_MODE_COUNT;
        gui_render_set_mode( m );
        static const char* names[] = { "normal", "wireframe", "batch" };
        printf( "[gui] render mode: %s\n", names[ m ] );
        redraw_request();
    }
    if ( gui_is_key_pressed( APP_KEY_F10 ) )
    {
        s_dbg_dash_open = !s_dbg_dash_open;
        printf( "[gui] pipeline dashboard: %s\n", s_dbg_dash_open ? "open" : "closed" );
        redraw_request();
    }

#ifdef GUI_CMD_STEPPER
    /* F8 just shows/hides the control window (gui_step_window.c) -- it does NOT freeze.  Merely
       opening the stepper leaves the scene live; only the window's Capture button freezes this
       frame's band-0 command list for stepped replay (the , . step hotkeys scrub a freeze that
       is already active).  The window's X button hides it and never releases an active freeze. */
    if ( gui_is_key_pressed( APP_KEY_F8 ) )
    {
        s_dbg_step_open = !s_dbg_step_open;
        printf( "[gui] command stepper window: %s\n", s_dbg_step_open ? "open" : "closed" );
        redraw_request();
    }
#endif

    /* Letter and numpad keys: fenced so a focused text field owns them (numpad digits ARE text
       input with Num Lock on, unlike the function keys above). */
    if ( gui_want_capture_keyboard() )
        return;

    /* NP1-NP7 toggle the debug layer mask (window / interact / resize / layout / clip / content
       / region geometry).
       Read from the frame IO like every other debug hotkey -- initial-press only, so holding a
       key never flickers the layer.  The layer setters compile to no-ops in Release
       (render/gui_debug_overlay.c), so no build guard is needed here.  CONTENT differs from the
       rest in where it draws -- the MAIN list at region pop (gui_scroll.c), not the overlay list,
       so that toggle changes every scrollable window's emitted commands; the shared wants_redraw
       below makes the flip land instead of sitting behind the clean-frame emit skip. */
    {
        static const struct { app_key_t key; u32 layer; } k_dbg_layer[] = {
            { APP_KEY_NP_1, GUI_DBG_WINDOW   }, { APP_KEY_NP_2, GUI_DBG_INTERACT },
            { APP_KEY_NP_3, GUI_DBG_RESIZE   }, { APP_KEY_NP_4, GUI_DBG_LAYOUT   },
            { APP_KEY_NP_5, GUI_DBG_CLIP     }, { APP_KEY_NP_6, GUI_DBG_CONTENT  },
            { APP_KEY_NP_7, GUI_DBG_REGION   },
        };
        for ( u32 i = 0; i < sizeof( k_dbg_layer ) / sizeof( k_dbg_layer[ 0 ] ); ++i )
            if ( gui_is_key_pressed( k_dbg_layer[ i ].key ) )
            {
                gui_debug_set_layers( gui_debug_get_layers() ^ k_dbg_layer[ i ].layer );
                redraw_request();
            }
    }

    /* Perf / state overlay tiers keep a quick keyboard cycle (numpad +/-, away from the letter
       row so they read as a pair) alongside their checkbox-list slider (debug_selector_menu
       below) -- these two are flipped often enough while chasing a frame that a click is friction.
       C/F/I lost their letter keys entirely: single booleans toggled rarely, better discovered as
       checkboxes than memorized as hotkeys. */
    if ( gui_is_key_pressed( APP_KEY_NP_ADD ) )
    {
        s_dbg_perf_mode = ( s_dbg_perf_mode + 1 ) % 5;
        redraw_request();
    }

    if ( gui_is_key_pressed( APP_KEY_NP_SUB ) )
    {
        s_dbg_state_mode = ( s_dbg_state_mode + 1 ) % 4;
        redraw_request();
    }

#ifdef GUI_CMD_STEPPER
    /* , . step the frozen replay cursor (repeat-aware so holding scrubs; shift steps by 16).
       The seek latches -- it applies at the next frame's restore -- so wants_redraw is required
       here or the clean-frame emit skip would sit on the stale cursor (the deferred-update rule). */
    if ( step_frozen() )
    {
        u32  stride = ( gui_is_key_down( APP_KEY_LSHIFT ) || gui_is_key_down( APP_KEY_RSHIFT ) )
                          ? 16u : 1u;
        bool back   = gui_is_key_pressed_repeat( APP_KEY_COMMA );
        bool fwd    = gui_is_key_pressed_repeat( APP_KEY_PERIOD );
        if ( back || fwd )
        {
            u32 c = step_cursor();
            if ( back )
                c = c > stride ? c - stride : 0u;
            else
                c = c + stride;               /* seek clamps to the frozen command count */
            step_seek( c );
            printf( "[gui] command stepper: %u/%u\n", step_cursor(), step_count() );
            redraw_request();
        }
    }
#endif
}

/*==============================================================================================
    Debug selector menu -- dense checkbox/slider list, right edge of the viewport

    Where C/F/I/P/O used to be single letters read out of the raw key stream, this is an actual
    UI: three checkboxes (retained skip, force redraw, idle skip) and the perf/state overlay tier
    sliders, all dense-packed in one panel.  Shown exactly while the master arm is on (NP_DOT),
    same as every other debug lever in this file -- press it again and debug_reset() clears the
    levers back to default the same frame this panel disappears.

    GUI_WIN_DEBUG_BAND (not GUI_WIN_NO_INPUT, unlike the read-only overlays above): this panel
    must be clickable, but its own geometry still has to stay out of the very stats/counts it is
    used to tweak -- the same arena-band exemption the perf/state overlays get. */
static void
debug_selector_menu( void )
{
    /* Work top (caption band + menu bar) + this panel's own margin -- see perf_overlay. */
    f32 top_y = gui_viewport_content_y( 0 ) + 8.0f;

    f32 w = 190.0f;
    f32 x = (f32)s_io.display_w - w - 8.0f;

    gui_region_begin( "debug_selector", x, top_y, w, 0.0f, GUI_REGION_FG,
                      GUI_WIN_NOSCROLL | GUI_WIN_DEBUG_BAND );
    {
        overlay_backdrop( id_hash( "debug_selector" ), x, top_y );
        gui_stack();

        bool force = gui_force_redraw();
        if ( gui_checkbox( "Force redraw", &force ) )
            gui_set_force_redraw( force );

        bool cached = build_retained_skip();
        if ( gui_checkbox( "Tess cache", &cached ) )
            build_set_retained_skip( cached );

        gui_checkbox( "Idle skip", &s_idle_skip );

        gui_new_line( 2.0f );
        gui_slider_int( "Perf tier", &s_dbg_perf_mode,  0, 4 );
        gui_slider_int( "IO tier",   &s_dbg_state_mode, 0, 3 );
    }
    gui_region_end();
}

/* Emit the debug overlays into the currently bound (default) context -- called from ctx_end
   before it rebinds, so this is exactly where a host used to hand-place them: last in the
   default context's build, drawing on top of everything it emitted. */
static void
debug_overlays_emit( void )
{
    dash_window( &s_dbg_dash_open );
    step_window( &s_dbg_step_open );
    if ( s_dbg_hotkeys_armed )
        debug_selector_menu();
    /* Tier state is no longer zeroed on disarm (debug_reset) so the selector menu can remember
       it -- gate visibility on the arm here instead, the same net effect (hidden while off). */
    overlay_perf ( s_dbg_hotkeys_armed ? s_dbg_perf_mode  : 0 );
    overlay_state( s_dbg_hotkeys_armed ? s_dbg_state_mode : 0 );
}

/* NOTE: gui_frame_pace() -- the end-of-loop idle sleep -- lives in gui_boot.c, the boot-tier
   loop it belongs to.  It still reads the frame hooks (s_hook_sleep/wait) and s_idle_skip set
   here and s_any_redraw folded in gui_frame_loop.c; the gui_frame.c unity includes gui_boot.c
   last, so those statics are all in scope there. */

// clang-format on
/*============================================================================================*/
