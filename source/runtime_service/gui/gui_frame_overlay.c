/*==============================================================================================

    runtime_service/gui/gui_frame_overlay.c -- Built-in perf / state HUD overlays.

    Two hidden-chrome debug readouts drawn through the ordinary GUI pipeline, plus the frame-timing
    instrumentation the perf overlay reads.  The timing helpers (perf_frame_begin / perf_frame_end /
    perf_render_begin / perf_render_end) are the private half of this unit: they are called from the
    frame lifecycle in gui_frame.c, which is why this file is included BEFORE gui_frame.c in the
    unity build (gui.c) -- those statics must be in scope where the lifecycle brackets them.
    gui_perf_overlay / gui_state_overlay are the public half, drawn by the host each frame.

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

void
gui_perf_overlay( gui_clock_fn clock, int mode )
{
    /* Adopt the host clock so frame_begin / render can time emit + render (effective next frame). */
    s_perf.clock = clock;

    if ( mode <= 0 )
        return;

    f32 fps = s_perf.fps;

    f32 top_y = 8.0f;
    gui_window_t* mb = window_find( id_hash( "##MainMenuBar" ) );
    if ( mb && mb->last_frame == g_ctx->retained.frame )
        top_y += mb->h;

    /* A root region: no chrome to hide (no window body/border to paint transparent), fixed
       top-left, hugging its content (w/h <= 0 autosize both axes).  NO_INPUT: pure text readout,
       a region is interactive by default and this one has no business entering the hover_win
       contest or eating the mouse wheel.  DEBUG_BAND: a self-measuring readout -- its own
       ever-changing digits must not count in the stats it displays or poison idle-skip. */
    gui_region_begin( "perf_overlay", 8.0f, top_y, 0.0f, 0.0f,
                      GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT | GUI_WIN_DEBUG_BAND );
    {
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
            gui_spacing( 2.0f );
            gui_textf( "emit   %5.2f ms", s_perf.s_emit_ms );
            gui_textf( "render %5.2f ms", s_perf.s_rend_ms );
        }

        bool show_geometry_rows = ( mode >= 3 );
        if ( show_geometry_rows )
        {
            gui_render_stats_t rs = gui_build_stats();
            gui_spacing( 2.0f );
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
                gui_spacing( 2.0f );
                gui_textf( "wins ret  %u/%u", rs.win_retained,  rs.win_total   );
                gui_textf( "verts ret %u/%u", rs.vert_retained, rs.vert_count  );
                gui_textf( "tris ret  %u/%u", rs.tri_retained,  rs.tri_count   );
                gui_textf( "vol patch %u",    rs.volatile_patched              );

                /* Upload stats: GPU memory bandwidth. */
                gui_spacing( 2.0f );
                gui_textf( "up batch  %u", rs.upload_batches );
                gui_textf( "up bytes  %u", rs.upload_bytes   );
            }
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

void
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
    gui_region_begin( "state_overlay", 260.0f, top_y, 0.0f, 0.0f, GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT );
    {
        gui_stack();

        gui_textf( "Hover   %s", dbg_id_str( s_interaction.hover_id ) );
        gui_textf( "Active  %s (btn %u)", dbg_id_str( s_interaction.active_id ), s_interaction.active_button );
        gui_textf( "Window  %s", dbg_id_str( s_interaction.hover_win ) );

        bool show_extended_rows = ( mode >= 2 );
        if ( show_extended_rows )
        {
            gui_spacing( 2.0f );
            gui_textf( "Focused %s", dbg_id_str( s_interaction.focused_id ) );
            gui_textf( "Nav id  %s", dbg_id_str( s_nav.id ) );
            gui_textf( "Nav win %s", dbg_id_str( s_nav.win ) );
            gui_textf( "Mouse   %6.1f, %6.1f", s_io.mouse_x, s_io.mouse_y );
        }

        bool show_popup_rows = ( mode >= 3 );
        if ( show_popup_rows )
        {
            gui_spacing( 2.0f );
            gui_textf( "Popups  %u", s_popup_open_count );
            if ( s_popup_open_count )
                gui_textf( "Top pop %s", dbg_id_str( s_popups_open[ s_popup_open_count - 1u ].id ) );
            gui_textf( "Ctx salt 0x%08X", g_ctx->retained.id_salt );
        }
    }
    gui_region_end();
}

// clang-format on
/*============================================================================================*/
