/*==============================================================================================

    runtime_service/gui/debug/gui_dashboard.c -- Pipeline dashboard: window shell + panel painters.

    A visual diagnostic of the gui render backend: a memory map of the shared QUAD ARENA
    (per-window slots, pads, volatile sub-slots, the debug-band boundary), frames in flight with
    upload spans, the dispatch-order batch inspector with batch-cut causes, pool usage bars for
    every capped EMIT and BUILD buffer, the volatile registry, and a frame-stats strip.

    UNITS: the maps and bars count ENTRIES (quads, records, commands), never bytes -- a quad index
    is a position in the arena, not a size.  Anything measured in bytes says so, and derived byte
    figures go through dash_bytes so the unit is always on screen next to the number.

    An ORDINARY window drawn through the normal pipeline with the standard draw API -- it
    docks, tears off into its own viewport, and z-orders under overlap like any other window.
    What keeps it honest is GUI_WIN_DEBUG_BAND: the band system packs its geometry (and its
    tooltips', via popup band inheritance) AFTER every main-band slot and excludes it from the
    stats and any_changed signals, so the diagnostic never perturbs the arena layout or the
    metrics it displays.  Its own geometry lives in the debug (second) band; by default every panel
    -- arena maps, high-water marks, and the emit/build usage bars -- filters that band out and
    reports the MAIN band alone, so what you read is the arena a real application would use.  The
    "Second band" toggle folds the debug band back in (dimmed and marked in the maps, split out in
    the headers) when you want to see the observer's own cost.

    Data comes from the backend capture (render/gui_dash_capture.c) via dash_snapshot():
    a coherent copy taken at the end of cache_build_frame / gui_render_flush.  The shell emits
    one frame after a capture, so the display lags the pipeline by one frame -- the standard
    self-measurement lag.  Hover tooltips resolve against the same snapshot the bars were drawn
    from, so they always agree.

    Emitted internally (debug_overlays_emit, gui_frame_overlay.c) at the default context's
    ctx_end while debug_enable is on; the F10 hotkey owns the open flag and the window's X
    button writes it back to false.  Included by gui_debug.c; the frame unit's overlay driver
    opens it across the boundary through debug/gui_debug.h.  Compiled out unless GUI_PIPELINE_DASHBOARD
    (gui_render.h); dash_window stays a no-op stub then.

==============================================================================================*/
// clang-format off

/* The dashboard window's id -- stays 0 when the feature is compiled out or never emitted.
   Used to mark the dashboard's own slot in the memory map and to gate the hover tooltips. */
static gui_id_t g_dash_window_id = 0;

#ifdef GUI_PIPELINE_DASHBOARD

#define DASH_SHELL_TITLE "Pipeline Dashboard"   /* id_hash of this = g_dash_window_id */

/*==============================================================================================
    Colors

    Two kinds live here, and the split is the point.

    SEVERITY is the engine's, and reads out of the extended palette -- GUI_EXT_OK / WARN / ERROR.
    The dashboard used to carry its own literals for these, which is how a diagnostic ends up
    shouting a saturated dark-theme green at you on a light theme.  A severity is a claim the
    whole application makes, so it belongs to the palette; other consumers were making the same
    claim in their own hex, which is what earned the severity ladder its reserved slots.

    The rest genuinely IS private and stays that way: an instrument's near-black backdrop, its
    highwater marker, its CATEGORICAL region palette.  Those are not severities -- a categorical
    palette's job is to make ten things distinguishable from each other, not to rank them -- and
    a theme has no opinion about them worth inheriting.
==============================================================================================*/

#define DASH_COL_OK         style_ext( GUI_EXT_OK )
#define DASH_COL_WARN       style_ext( GUI_EXT_WARN )
#define DASH_COL_BAD        style_ext( GUI_EXT_ERROR )
#define DASH_COL_OVERFLOW   style_ext( GUI_EXT_ERROR )
#define DASH_COL_FIF_ACTIVE style_ext( GUI_EXT_OK )

#define DASH_COL_TEXT       GUI_COLOR( 0xD8, 0xD8, 0xD8, 0xFF )
#define DASH_COL_TEXT_DIM   GUI_COLOR( 0x90, 0x90, 0x90, 0xFF )
#define DASH_COL_BG         GUI_COLOR( 0x14, 0x14, 0x18, 0xFF )
#define DASH_COL_HWM        GUI_COLOR( 0xF0, 0xE0, 0x40, 0xE0 )
#define DASH_COL_CHANGED    GUI_COLOR( 0xFF, 0xFF, 0xFF, 0xD0 )
#define DASH_COL_SELF       GUI_COLOR( 0x50, 0xE0, 0xF0, 0xFF )
#define DASH_COL_VOLATILE   GUI_COLOR( 0xFF, 0xFF, 0xFF, 0x60 )
#define DASH_COL_FIF_IDLE   GUI_COLOR( 0x40, 0x40, 0x48, 0xFF )
#define DASH_COL_SPAN_VERT  GUI_COLOR( 0x66, 0xBB, 0xEE, 0xFF )
/* No CUT_TEX: a texture change stopped cutting batches when the texture moved into the vertex. */
#define DASH_COL_CUT_VP     GUI_COLOR( 0xE0, 0x50, 0xE0, 0xFF )
#define DASH_COL_CUT_FORCE  GUI_COLOR( 0xFF, 0xFF, 0xFF, 0xC0 )

/* Region palette. */
static const u32 s_dash_palette[] = {
    GUI_COLOR( 0x4E, 0x9D, 0xE0, 0xC8 ),  GUI_COLOR( 0xE0, 0x8A, 0x3C, 0xC8 ),
    GUI_COLOR( 0x5E, 0xC2, 0x6A, 0xC8 ),  GUI_COLOR( 0xD9, 0x5C, 0x5C, 0xC8 ),
    GUI_COLOR( 0xA8, 0x7A, 0xE8, 0xC8 ),  GUI_COLOR( 0x8A, 0x6E, 0x5A, 0xC8 ),
    GUI_COLOR( 0xE2, 0x8A, 0xC8, 0xC8 ),  GUI_COLOR( 0x9A, 0xA0, 0xA8, 0xC8 ),
    GUI_COLOR( 0xC8, 0xC8, 0x50, 0xC8 ),  GUI_COLOR( 0x50, 0xC8, 0xC8, 0xC8 ),
};
#define DASH_PALETTE_N ( sizeof( s_dash_palette ) / sizeof( s_dash_palette[ 0 ] ) )

/* Color keyed by ARENA SLOT INDEX, not window id: consecutive memory regions always land on
   consecutive palette entries, so neighbouring bands never share a color -- an id hash collides
   (the menu bar and the Demo Window rendered identically).  The memory map and the batch inspector
   both key on the same slot index, so a window's arena region and its batch row still match by
   color.  Stable frame-to-frame while the window set is (a window appearing/disappearing may shift
   the assignment -- fine for a diagnostic). */
static u32
dash_slot_color( u32 slot_index )
{
    return s_dash_palette[ slot_index % DASH_PALETTE_N ];
}

/*==============================================================================================
    Painter helpers -- thin wrappers over the standard draw API so the panel code reads like
    the diagram it draws.  All coordinates are absolute (gui_canvas rects).
==============================================================================================*/

/* Glyph run truncated at max_x (self-fit rule: never bleed past the cell).  y is the line top. */
static void
dash_text( f32 x, f32 y, f32 max_x, u32 abgr, const char* str )
{
    if ( max_x - x < 4.0f ) return;
    gui_draw_text_clipped( ( gui_rect_t ){ x, y, max_x - x, font_line_h() },
                           GUI_ALIGN_LEFT, abgr, str );
}

static void
dash_textf( f32 x, f32 y, f32 max_x, u32 abgr, const char* fmt, ... )
{
    char buf[ 128 ];
    va_list args;
    va_start( args, fmt );
    fmt_vsnprintf( buf, sizeof( buf ), fmt, args );
    va_end( args );
    dash_text( x, y, max_x, abgr, buf );
}

/* Number label drawn WITHOUT the self-fit ellipsis -- the caller owns a column wide enough for
   the value, so a count must always read in full rather than truncate to "123..". */
static void
dash_num( f32 x, f32 y, u32 abgr, const char* fmt, ... )
{
    char buf[ 64 ];
    va_list args;
    va_start( args, fmt );
    fmt_vsnprintf( buf, sizeof( buf ), fmt, args );
    va_end( args );
    gui_draw_text( x, y, abgr, buf );
}

/* Bytes as a compact figure -- "912 B", "14.2 KB", "3.14 MB".  Every size on this dashboard is
   DERIVED (a count times a record stride), never stored, so the unit is spelled out wherever one
   appears: a bare number next to a quad index reads as an address, not a size.  buf holds >= 16. */
static const char*
dash_bytes( u32 b, char* buf, u32 bufsz )
{
    if ( b < 1024u )              fmt_snprintf( buf, bufsz, "%u B",     b );
    else if ( b < 1024u * 1024u ) fmt_snprintf( buf, bufsz, "%.1f KB",  (f32)b / 1024.0f );
    else                          fmt_snprintf( buf, bufsz, "%.2f MB",  (f32)b / ( 1024.0f * 1024.0f ) );
    return buf;
}

static void
dash_outline( gui_rect_t r, u32 abgr )
{
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    gui_draw_round_rect( r, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, abgr );
}

/* Dashed vertical marker (high-water / band-boundary lines). */
static void
dash_vline( f32 x, f32 y0, f32 y1, u32 abgr )
{
    gui_draw_dashed_line( x, y0, x, y1, 3.0f, 3.0f, 1.0f, abgr );
}

/* Rect batch: queues solid fills for one flush via gui_draw_rects (GUI_CMD_RECT_LIST), so a
   panel drawing many small fills -- per-command bars, per-slot arena segments, per-row progress
   bars -- spends ONE semantic command instead of one gui_draw_rect per shape.  A flush preserves
   push order (entries tessellate in array order, so an overlapping later entry still paints on
   top of an earlier one within the same flush) -- anything that must land on top of a batched
   fill from OUTSIDE the batch (an outline stroke, a text label) needs an explicit flush first. */
#define DASH_RECT_BATCH_CAP 256

typedef struct
{
    gui_rect_col_t buf[ DASH_RECT_BATCH_CAP ];
    u32            n;
} dash_rect_batch_t;

static void
dash_rb_flush( dash_rect_batch_t* rb )
{
    if ( rb->n == 0 ) return;
    gui_draw_rects( rb->buf, rb->n );
    rb->n = 0;
}

static void
dash_rb_push( dash_rect_batch_t* rb, f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    if ( rb->n >= DASH_RECT_BATCH_CAP )
        dash_rb_flush( rb );
    rb->buf[ rb->n++ ] = ( gui_rect_col_t ){ x, y, w, h, abgr };
}

/* id -> registered source string (debug overlay's registry) or hex.  buf must hold >= 12. */
static const char*
dash_name( gui_id_t id, char* buf, u32 bufsz )
{
    const char* n = gui_debug_name( id );
    if ( n ) return n;
    fmt_snprintf( buf, bufsz, "%08X", id );
    return buf;
}

/* Compact z label: the dispenser's plain window z values print as-is, but the fixed high bands
   (region / overlay / foreground / dock-drag-overlay -- the z band map in gui_surface.c) are
   large constants plus a small offset, and printing them raw is a 10-digit number that blows any
   column meant for "z3 v0 g12".  Print the band's tag and its offset instead.  buf must hold >= 12. */
static const char*
dash_fmt_z( u32 z, char* buf, u32 bufsz )
{
    if ( z >= 0xF8000000u )          fmt_snprintf( buf, bufsz, "DOCK+%u", z - 0xF8000000u );
    else if ( z >= GUI_REGION_FG_Z ) fmt_snprintf( buf, bufsz, "FG+%u",   z - GUI_REGION_FG_Z );
    else if ( z >= GUI_Z_OVERLAY )   fmt_snprintf( buf, bufsz, "OVL+%u",  z - GUI_Z_OVERLAY );
    else if ( z >= GUI_REGION_Z )    fmt_snprintf( buf, bufsz, "REG+%u",  z - GUI_REGION_Z );
    else                              fmt_snprintf( buf, bufsz, "%u", z );
    return buf;
}

/* One tooltip per emit frame, gated on the dashboard owning the hover (a window floating above
   must not probe through).  A hit claims the frame so overlapping rects lower in the paint
   order (a slot segment under a volatile strip) cannot double-tooltip. */
static bool s_tip_done;

/* View toggles, set from the shell checkboxes each frame and read by the painters (which take
   only rect + snapshot).  Defaults chosen so the dashboard's own debug (second) band stays out
   of the arena maps, and the maps scale to the high-water mark rather than the full cap so the
   live bars render wide enough to read. */
static bool s_show_second_band;   /* include band != 0 slots (the dashboard itself) in the maps */
static bool s_full_range;         /* scale the arena maps to the full cap vs the high-water mark */
static bool s_show_pad;           /* break out each slot's alloc pad vs. one flat fill over it all */

static bool
dash_tip_at( gui_rect_t r )
{
    if ( s_tip_done )
        return false;
    if ( s_interaction.hover_win != g_dash_window_id )
        return false;
    if ( !gui_is_mouse_hovering_rect( r ) )
        return false;
    s_tip_done = true;
    return true;
}

/*==============================================================================================
    Panel painters -- each draws the snapshot into its gui_canvas rect with the standard draw
    API and raises normal tooltips on hover (they inherit the debug band automatically).
==============================================================================================*/

/* Memory-map body: the whole quad arena as one horizontal bar.  Main-band slots first, the
   dashed band boundary, then (only when "Show second band" is on) the debug band's own slots
   dimmed -- the observer marked, not hidden. */
static void
dash_panel_memmap( gui_rect_t r, const dash_snapshot_t* sn )
{
    const f32 lh  = font_line_h();
    const u32 cap = GUI_MAX_QUADS;

    /* Header + arena scale honor the "Second band" toggle.  OFF (default): every figure -- used,
       high-water and the bar scale -- is the MAIN band alone, so the map reads as a real
       application's arena with the self-measuring dashboard filtered out.  ON: totals include the
       debug band and the band0/debug split is spelled out. */
    u32 total = sn->tess_quads;
    u32 main0 = sn->band0_quad_end;
    u32 pad   = (u32)SLOT_QUAD_PAD;
    u32 used  = s_show_second_band ? total : main0;
    u32 hwm   = s_show_second_band ? sn->quad_hwm : sn->band0_quad_hwm;

    /* Every figure on this bar is a QUAD INDEX into the arena, so the header carries the byte
       equivalent once: the map's numbering is positions, not sizes, and the two are easy to
       confuse when both are four digits. */
    char ub[ 16 ], cb[ 16 ];
    dash_bytes( used * (u32)GUI_QUAD_BYTES, ub, sizeof( ub ) );
    dash_bytes( cap  * (u32)GUI_QUAD_BYTES, cb, sizeof( cb ) );

    if ( s_show_second_band )
        dash_textf( r.x + 2.0f, r.y, r.x + r.w, DASH_COL_TEXT_DIM,
                    "%u / %u quads  (%s / %s)   band0 %u  debug %u   hwm %u   slot pad %u",
                    used, cap, ub, cb, main0, total - main0, hwm, pad );
    else
        dash_textf( r.x + 2.0f, r.y, r.x + r.w, DASH_COL_TEXT_DIM,
                    "%u / %u quads  (%s / %s)   hwm %u   slot pad %u   (band 0)",
                    used, cap, ub, cb, hwm, pad );

    gui_rect_t bar = { r.x, r.y + lh + 2.0f, r.w, r.h - lh - 4.0f };
    if ( bar.h < 12.0f ) return;

    gui_draw_rect( bar.x, bar.y, bar.w, bar.h, DASH_COL_BG );   /* free space: flat backdrop */

    /* Scale the arena to the full cap, or (default) to the high-water mark so the live slots
       render wider.  Everything drawn (base + alloc) sits at or below hwm, so nothing clips. */
    u32 scale  = ( s_full_range || hwm == 0 ) ? cap : hwm;
    f32 px_per = bar.w / (f32)scale;

    /* Pass 1: every slot's flat fill + its volatile strips, batched into one draw_rects flush --
       drawn before pass 2 below so the outline stroke and text label of pass 2 still land on top
       of them, exactly as the old single-pass order had it.  Span cached per slot so pass 2 does
       not need to redo the base/count/alloc -> pixel math.  Pad mode ("Show pad") mixes a hatch
       pattern in with the headroom fill, so it stays on the immediate path -- the hatch must land
       on top of ITS OWN fill, which a deferred batch flush cannot guarantee mid-loop. */
    typedef struct { u32 slot_i; f32 x0, xa; } dash_map_span_t;
    dash_map_span_t   spans[ RENDER_MAX_WIN ];
    u32               span_n = 0;
    dash_rect_batch_t rb     = { 0 };

    for ( u32 i = 0; i < sn->slot_count; ++i )
    {
        const dash_slot_t* sl = &sn->slots[ i ];
        if ( !sl->valid ) continue;
        if ( sl->band != 0 && !s_show_second_band ) continue;   /* second band omitted by default */

        u32 base  = sl->quad_base;
        u32 count = sl->quad_count;
        u32 alloc = sl->quad_alloc;

        f32 x0 = bar.x + (f32)base * px_per;
        f32 xc = bar.x + (f32)( base + count ) * px_per;
        f32 xa = bar.x + (f32)( base + alloc ) * px_per;
        u32 col = dash_slot_color( i );
        if ( sl->band != 0 )
            col = ( col & 0x00FFFFFFu ) | 0x60000000u;   /* debug band: dimmed strip at the tail */

        if ( s_show_pad )
        {
            gui_draw_rect( x0, bar.y, xc - x0, bar.h, col );                       /* live geometry   */
            gui_draw_rect( xc, bar.y, xa - xc, bar.h, ( col & 0x00FFFFFFu ) | 0x30000000u );
            gui_draw_hatch( ( gui_rect_t ){ xc, bar.y, xa - xc, bar.h }, 5.0f,     /* padded headroom */
                            1.0f, ( col & 0x00FFFFFFu ) | 0x80000000u );
        }
        else
        {
            dash_rb_push( &rb, x0, bar.y, xa - x0, bar.h, col );   /* whole reservation, one flat fill */
        }

        /* Volatile sub-slots: brighter strips over the full height of their owner's extent. */
        for ( u32 v = 0; v < sn->vol_count; ++v )
        {
            const dash_vol_t* vo = &sn->vols[ v ];
            if ( !vo->active || vo->win != sl->win ) continue;

            f32 vx0 = bar.x + (f32)( base + vo->lquad_base ) * px_per;
            f32 vx1 = bar.x + (f32)( base + vo->lquad_base + vo->quad_alloc ) * px_per;
            gui_rect_t vr = { vx0, bar.y + 1.0f, vx1 - vx0, bar.h - 2.0f };
            dash_rb_push( &rb, vr.x, vr.y, vr.w, vr.h, DASH_COL_VOLATILE );

            /* Hover hit-test is independent of when the fill actually draws -- keep it here. */
            if ( dash_tip_at( vr ) )
            {
                char nb[ 12 ], wb[ 12 ];
                if ( gui_tooltip_begin() )
                {
                    gui_stack();
                    gui_textf( "volatile %s  in %s", dash_name( vo->id, nb, sizeof( nb ) ),
                               dash_name( vo->win, wb, sizeof( wb ) ) );
                    gui_textf( "at +%u in the slot: %u / %u quads reserved",
                               vo->lquad_base, vo->quad_count, vo->quad_alloc );
                }
                gui_tooltip_end();
            }
        }

        if ( span_n < RENDER_MAX_WIN )
            spans[ span_n++ ] = ( dash_map_span_t ){ i, x0, xa };
    }

    dash_rb_flush( &rb );   /* every slot's fill + strips land before any outline/label below */

    /* Pass 2: diff-pulse / self-marker outlines and window-name labels -- must draw on top of
       every slot's fill above, so it is a second pass over the cached spans rather than
       interleaved into pass 1. */
    for ( u32 s = 0; s < span_n; ++s )
    {
        const dash_slot_t* sl = &sn->slots[ spans[ s ].slot_i ];
        f32                x0 = spans[ s ].x0, xa = spans[ s ].xa;

        gui_rect_t seg = { x0, bar.y, xa - x0, bar.h };
        if ( sl->win == g_dash_window_id )
            dash_outline( seg, DASH_COL_SELF );
        else if ( sl->changed )
            dash_outline( seg, DASH_COL_CHANGED );

        /* Window name inside the segment when it is wide enough to read. */
        if ( xa - x0 >= 40.0f )
        {
            char nb[ 12 ];
            gui_draw_text_clipped( ( gui_rect_t ){ x0 + 3.0f, bar.y, xa - x0 - 5.0f, bar.h },
                                   GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, DASH_COL_TEXT,
                                   dash_name( sl->win, nb, sizeof( nb ) ) );
        }

        if ( dash_tip_at( seg ) )
        {
            char nb[ 12 ];
            if ( gui_tooltip_begin() )
            {
                gui_stack();
                gui_textf( "window  %s%s%s", dash_name( sl->win, nb, sizeof( nb ) ),
                           sl->win == g_dash_window_id ? "  (this dashboard)" : "",
                           sl->band != 0 ? "  [debug band]" : "" );
                char lb[ 16 ], rb2[ 16 ];
                gui_textf( "arena   [%u .. %u)   %u quads reserved, %u live",
                           sl->quad_base, sl->quad_base + sl->quad_alloc,
                           sl->quad_alloc, sl->quad_count );
                gui_textf( "        %s live / %s reserved  (%u quads pad)",
                           dash_bytes( sl->quad_count * (u32)GUI_QUAD_BYTES, lb, sizeof( lb ) ),
                           dash_bytes( sl->quad_alloc * (u32)GUI_QUAD_BYTES, rb2, sizeof( rb2 ) ),
                           sl->quad_alloc - sl->quad_count );
                gui_textf( "cmds    [%u .. %u)   z %u  vp %d  gen %u", sl->cmd_base,
                           sl->cmd_base + sl->cmd_count, sl->z, sl->vp, sl->tess_gen );
                gui_textf( "%s this frame", sl->changed ? "re-tessellated" : "retained" );
            }
            gui_tooltip_end();
        }
    }

    /* Band boundary (only meaningful with the debug band shown; off, it coincides with `used`),
       high-water marker, lifetime overflow flag. */
    if ( s_show_second_band && main0 > 0 && main0 < cap )
        dash_vline( bar.x + (f32)main0 * px_per, bar.y, bar.y + bar.h, DASH_COL_SELF );
    dash_vline( bar.x + (f32)hwm * px_per, bar.y, bar.y + bar.h, DASH_COL_HWM );
    if ( sn->overflow_ever )
        gui_draw_rect( bar.x + bar.w - 8.0f, bar.y, 8.0f, 8.0f, DASH_COL_OVERFLOW );
}

/* Frames-in-flight: per live surface, one box per in-flight region; the region written last is
   highlighted with its upload spans drawn inside. */
static void
dash_panel_fif( gui_rect_t r, const dash_snapshot_t* sn )
{
    const f32  row_h  = 22.0f;
    f32        y      = r.y + 2.0f;
    const bool frozen = dash_frozen();   /* the active region rotates every frame; call it out
                                                only when frozen so it never strobes at framerate */

    for ( i32 vp = 0; vp < GUI_MAX_VIEWPORTS; ++vp )
    {
        if ( !sn->surf[ vp ].live ) continue;
        if ( y + row_h > r.y + r.h ) break;

        const dash_surf_t* sf = &sn->surf[ vp ];

        dash_textf( r.x + 2.0f, y + 3.0f, r.x + 40.0f, DASH_COL_TEXT, "vp%d", vp );

        /* Region boxes sit in the band [40 .. 0.55*w]; size box_w so the N boxes + (N-1) gaps fill
           it exactly (the old calc ignored the gaps and overran the band) and center each box
           vertically in the row. */
        const f32 gap    = 6.0f;
        f32       band_x = r.x + 40.0f;
        f32       band_w = r.w * 0.55f - 40.0f;
        f32       box_h  = row_h - 6.0f;
        f32       box_y  = y + ( row_h - box_h ) * 0.5f;
        f32       box_w  = ( band_w - gap * (f32)( RHI_MAX_FRAMES_IN_FLIGHT - 1 ) )
                           / (f32)RHI_MAX_FRAMES_IN_FLIGHT;
        for ( u32 f = 0; f < RHI_MAX_FRAMES_IN_FLIGHT; ++f )
        {
            gui_rect_t box = { band_x + (f32)f * ( box_w + gap ), box_y, box_w, box_h };
            bool       act = ( f == sf->frame_index );
            gui_draw_rect( box.x, box.y, box.w, box.h, DASH_COL_BG );
            /* Grey outline on every box by default; the green active-region call-out only when
               frozen -- live it would strobe between regions at framerate. */
            dash_outline( box, ( frozen && act ) ? DASH_COL_FIF_ACTIVE : DASH_COL_FIF_IDLE );
            if ( act && sf->quad_hi > sf->quad_lo )
            {
                /* The uploaded quad span as one sub-bar in the box interior. */
                f32 inner_x = box.x + 1.0f;
                f32 inner_w = box.w - 2.0f;
                f32 vx0     = inner_x + inner_w * (f32)sf->quad_lo / (f32)GUI_MAX_QUADS;
                f32 vx1     = inner_x + inner_w * (f32)sf->quad_hi / (f32)GUI_MAX_QUADS;
                gui_draw_rect( vx0, box.y + 2.0f, vx1 - vx0, box.h - 4.0f, DASH_COL_SPAN_VERT );
            }
        }

        char upb[ 16 ];
        dash_textf( r.x + r.w * 0.55f + 8.0f, y + 3.0f, r.x + r.w, DASH_COL_TEXT_DIM,
                    "up %s  %u wr  %u draw",
                    dash_bytes( sf->up_bytes, upb, sizeof( upb ) ),
                    sf->up_batches, sf->draw_calls );

        if ( dash_tip_at( ( gui_rect_t ){ r.x, y, r.w, row_h } ) )
        {
            if ( gui_tooltip_begin() )
            {
                gui_stack();
                gui_textf( "surface vp%d  in-flight region %u of %u", vp, sf->frame_index,
                           (u32)RHI_MAX_FRAMES_IN_FLIGHT );
                char tub[ 16 ];
                if ( sf->quad_hi > sf->quad_lo )
                    gui_textf( "uploaded quads [%u .. %u)  =  %u quads", sf->quad_lo, sf->quad_hi,
                               sf->quad_hi - sf->quad_lo );
                else
                    gui_textf( "nothing uploaded (fully retained)" );
                gui_textf( "%s in %u writes   %u draw calls",
                           dash_bytes( sf->up_bytes, tub, sizeof( tub ) ),
                           sf->up_batches, sf->draw_calls );
            }
            gui_tooltip_end();
        }
        y += row_h;
    }

    if ( y == r.y + 2.0f )
        dash_text( r.x + 2.0f, y, r.x + r.w, DASH_COL_TEXT_DIM, "no surfaces flushed yet" );
}

/* Batch inspector: dispatch-order rows (back to front), each slot's cached GPU commands as
   bars, with a colored tick at each batch CUT (tex change / clip change / forced). */
static void
dash_panel_batch( gui_rect_t r, const dash_snapshot_t* sn )
{
    const f32 row_h  = font_line_h() + 3.0f;
    f32       y      = r.y + 2.0f;
    const f32 name_x = r.x + 150.0f;   /* window name column */
    const f32 bars_x = r.x + 330.0f;   /* meta column (z/vp/gen), wide enough for the compact
                                           z label below, before the command bars */

    /* Every row marker, batch-cut tick and command bar is a flat fill with nothing else drawn
       into the same pixels anywhere in the panel (names/meta live in their own column, tooltips
       are a separate overlay band), so the whole panel can defer to one flush at the end instead
       of one gui_draw_rect per command -- this loop is the dashboard's biggest command spender,
       one to two bars/ticks per cached GPU command across every visible row. */
    dash_rect_batch_t rb = { 0 };

    for ( u32 d = 0; d < sn->dispatch_count; ++d )
    {
        if ( y + row_h > r.y + r.h )
        {
            dash_textf( r.x + 2.0f, y - row_h, r.x + r.w, DASH_COL_TEXT_DIM,
                        "+%u more", sn->dispatch_count - d );
            break;
        }

        const dash_slot_t* sl  = &sn->slots[ sn->dispatch[ d ] ];
        if ( sl->band != 0 && !s_show_second_band ) continue;   /* second band omitted by default */
        u32                col = dash_slot_color( sn->dispatch[ d ] );
        char               nb[ 12 ], zb[ 12 ];

        dash_rb_push( &rb, r.x + 2.0f, y + 3.0f, 8.0f, 8.0f, col );
        dash_text( r.x + 14.0f, y, name_x, DASH_COL_TEXT,
                   dash_name( sl->win, nb, sizeof( nb ) ) );
        dash_textf( name_x + 4.0f, y, bars_x - 4.0f, DASH_COL_TEXT_DIM, "z%-8s v%u g%u",
                    dash_fmt_z( sl->z, zb, sizeof( zb ) ), sl->vp, sl->tess_gen );

        /* One bar per cached GPU command: width ~ log2(elem_count), colored by texture. */
        f32 bx = bars_x;
        for ( u32 k = 0; k < sl->cmd_count && sl->cmd_base + k < sn->cmd_count; ++k )
        {
            const dash_cmd_t* dc = &sn->cmds[ sl->cmd_base + k ];
            if ( dc->vp == GUI_VP_INVALID ) continue;   /* dormant volatile pad -- not drawn */

            f32 bw = 4.0f;
            for ( u32 e = dc->elem_count; e > 1; e >>= 1 ) bw += 3.0f;   /* ~3px per log2 step */
            if ( bw > 44.0f ) bw = 44.0f;
            if ( bx + bw > r.x + r.w - 4.0f ) break;

            if ( k > 0 )
            {
                /* Why this command split from the previous one -- the batch-cut cause.  Within one
                   window's row, tess_ensure_gpu_cmd (gui_build_tess.c) only ever cuts on a viewport
                   change or a forced boundary (a volatile block/pad opening); clip and texture ride
                   the quad now and cut nothing.  vp is therefore the whole test: anything else that
                   split the row was forced. */
                const dash_cmd_t* pc  = &sn->cmds[ sl->cmd_base + k - 1 ];
                u32               cut = ( dc->vp != pc->vp ) ? DASH_COL_CUT_VP : DASH_COL_CUT_FORCE;
                dash_rb_push( &rb, bx - 2.0f, y + 1.0f, 1.0f, row_h - 3.0f, cut );
            }

            /* One colour per batch: a batch can now MIX atlases, so "which texture is this" is no
               longer a property of the bar -- it belongs to the individual vertices inside it. */
            u32 bcol = col | 0xFF000000u;
            gui_rect_t bar = { bx, y + 2.0f, bw, row_h - 5.0f };
            dash_rb_push( &rb, bar.x, bar.y, bar.w, bar.h, bcol );

            if ( dash_tip_at( bar ) )
            {
                if ( gui_tooltip_begin() )
                {
                    gui_stack();
                    char qb[ 16 ];
                    gui_textf( "draw %u of %s", k, dash_name( sl->win, nb, sizeof( nb ) ) );
                    /* One record expands to 6 vertices / 2 triangles in the vertex stage, and that
                       is literally the draw: cmd_draw asks for elem_count * 6 bare vertices. */
                    gui_textf( "%u quads  ->  %u verts, %u tris   (%s)",
                               dc->elem_count, dc->elem_count * 6u, dc->elem_count * 2u,
                               dash_bytes( dc->elem_count * (u32)GUI_QUAD_BYTES, qb, sizeof( qb ) ) );
                    gui_textf( "vp %d   first tex %u", dc->vp, dc->tex_idx );
                    gui_textf( "arena base %u quads", dc->qbase );
                }
                gui_tooltip_end();
            }
            bx += bw + 3.0f;
        }

        if ( dash_tip_at( ( gui_rect_t ){ r.x, y, bars_x - r.x - 4.0f, row_h } ) )
        {
            if ( gui_tooltip_begin() )
            {
                gui_stack();
                char qb[ 16 ];
                gui_textf( "window  %s", dash_name( sl->win, nb, sizeof( nb ) ) );
                gui_textf( "%u draw cmds  %u quads  ->  %u verts, %u tris   (%s)",
                           sl->cmd_count, sl->quad_count, sl->quad_count * 6u,
                           sl->quad_count * 2u,
                           dash_bytes( sl->quad_count * (u32)GUI_QUAD_BYTES, qb, sizeof( qb ) ) );
                gui_textf( "z %u  vp %d  gen %u  %s", sl->z, sl->vp, sl->tess_gen,
                           sl->changed ? "re-tessellated" : "retained" );
            }
            gui_tooltip_end();
        }
        y += row_h;
    }

    dash_rb_flush( &rb );
}

/* EMIT/BUILD usage bars vs caps, graded green -> amber -> red, with hwm ticks where tracked,
   plus the debug band's own attributed footprint on the last line. */
static void
dash_panel_emit( gui_rect_t r, const dash_snapshot_t* sn )
{
    /* Up to three short lines per row rather than one long one: the dashboard docks narrow, and a
       tooltip is laid out against ITS OWN width, so a single sentence wraps to a strip several
       hundred pixels wide.  NULL ends the row's lines. */
    static const char* const tip[][ 3 ] = {
        { "EMIT: semantic draw commands",
          "one per draw_push_*  (GUI_MAX_CMDS)", NULL },
        { "EMIT: command segments",
          "one span per (window, z, viewport, band)", "(GUI_MAX_SEGS)" },
        { "EMIT: polyline / path points", "(GUI_MAX_PATH_PTS)", NULL },
        { "EMIT: draw_rects batch entries", "(GUI_MAX_RECT_ENTRIES)", NULL },
        { "EMIT: text pool, in BYTES",
          "one copy of every string drawn  (GUI_MAX_TEXT_POOL)", NULL },
        { "EMIT: distinct clip rects this frame", "(GUI_MAX_CLIP_RECTS)", NULL },
        { "BUILD: quad records, 16 B each -- one per SHAPE,",
          "plus each slot's reservation padding  (GUI_MAX_QUADS)", NULL },
        { "BUILD: the style arena, 128 B a record  (GUI_MAX_PRIMS).",
          "STYLES: one per distinct look, deduped hard.",
          "FX PAGES (inner bar): four per-instance records each --"
          " turn, phase, border colour, uv rect." },
        { "RENDER: GPU draw calls the batcher produced",
          "these consume the command table too  (GUI_MAX_CMDS)", NULL },
    };
    /* With "Second band" OFF (default) each bar is the main band alone (total minus the debug-band
       share the capture attributed), so the bars measure a real application against the caps.  The
       debug band's own footprint is always spelled out on the summary line below. */
    const bool inc = s_show_second_band;
    /* `sub` is an inner segment drawn over the fill: the share of this pool that is a DIFFERENT
       kind of occupant than its label suggests.  Only the style arena has one today. */
    struct { const char* name; u32 used, cap, hwm, sub; } rows[] = {
        { "emit cmd", inc ? sn->emit_cmds  : sn->emit_cmds  - sn->emit_cmds_dbg,  GUI_MAX_CMDS,       inc ? sn->emit_cmds_hwm : 0, 0 },
        { "segs",     inc ? sn->emit_segs  : sn->emit_segs  - sn->emit_segs_dbg,  GUI_MAX_SEGS,       0, 0 },
        { "pts",      inc ? sn->emit_pts   : sn->emit_pts   - sn->emit_pts_dbg,   GUI_MAX_PATH_PTS,   0, 0 },
        { "rects",    inc ? sn->emit_rects : sn->emit_rects - sn->emit_rects_dbg, GUI_MAX_RECT_ENTRIES, 0, 0 },
        { "text",     inc ? sn->emit_text  : sn->emit_text  - sn->emit_text_dbg,  GUI_MAX_TEXT_POOL,  0, 0 },
        { "clips",    inc ? sn->emit_clips : sn->emit_clips - sn->emit_clips_dbg, GUI_MAX_CLIP_RECTS, 0, 0 },
        { "quads",    inc ? sn->tess_quads : sn->band0_quad_end, GUI_MAX_QUADS,   inc ? sn->quad_hwm : sn->band0_quad_hwm, 0 },
        { "styles",   sn->tess_prims,                            GUI_MAX_PRIMS,   sn->prim_hwm, sn->tess_fx_pages },
        { "gpu cmd",  inc ? sn->tess_cmds  : sn->tess_cmds - sn->tess_cmds_dbg,   GUI_MAX_CMDS,       0, 0 },
    };
    const u32 n     = sizeof( rows ) / sizeof( rows[ 0 ] );
    const f32 lh    = font_line_h();
    const f32 row_h = ( r.h - lh * 4.0f - 4.0f ) / (f32)n;   /* four summary lines below the bars */
    const f32 bar_x = r.x + 84.0f;    /* label column + a clear gap before the bars */
    const f32 val_w = 132.0f;         /* value column: fits a full "NNNNN / NNNNN" with no ellipsis */
    const f32 bar_w = r.w - 84.0f - val_w;

    /* Pass 1: every row's bg + fill, batched -- flushed before pass 2's hwm tick so the tick
       still lands on top of the fill it marks, exactly as the old single-pass draw order had it. */
    dash_rect_batch_t rb = { 0 };
    for ( u32 i = 0; i < n; ++i )
    {
        f32 y    = r.y + 2.0f + (f32)i * row_h;
        f32 frac = rows[ i ].cap ? (f32)rows[ i ].used / (f32)rows[ i ].cap : 0.0f;
        u32 col  = frac > 0.90f ? DASH_COL_BAD : frac > 0.75f ? DASH_COL_WARN : DASH_COL_OK;

        dash_rb_push( &rb, bar_x, y + 2.0f, bar_w, row_h - 4.0f, DASH_COL_BG );
        dash_rb_push( &rb, bar_x, y + 2.0f, bar_w * frac, row_h - 4.0f, col );

        /* The pool's other occupant, drawn as a lighter band inside the fill at its tail end, so
           the eye reads one bar (the pool) split into what is filling it. */
        if ( rows[ i ].sub && rows[ i ].cap )
        {
            f32 sw = bar_w * (f32)rows[ i ].sub / (f32)rows[ i ].cap;
            dash_rb_push( &rb, bar_x + bar_w * frac - sw, y + 2.0f, sw, row_h - 4.0f,
                          ( col & 0x00FFFFFFu ) | 0x60000000u );
        }
    }
    dash_rb_flush( &rb );

    for ( u32 i = 0; i < n; ++i )
    {
        f32 y = r.y + 2.0f + (f32)i * row_h;

        dash_text( r.x + 2.0f, y, bar_x - 10.0f, DASH_COL_TEXT_DIM, rows[ i ].name );
        if ( rows[ i ].hwm )
            dash_vline( bar_x + bar_w * (f32)rows[ i ].hwm / (f32)rows[ i ].cap,
                        y + 1.0f, y + row_h - 1.0f, DASH_COL_HWM );

        /* A split pool prints its two occupants where the single count would go -- the number has
           to answer the same question the bar does, or the inner band has nothing to read against. */
        if ( rows[ i ].sub )
            dash_num( bar_x + bar_w + 6.0f, y, DASH_COL_TEXT_DIM, "%u+%u / %u",
                      rows[ i ].used - rows[ i ].sub, rows[ i ].sub, rows[ i ].cap );
        else
            dash_num( bar_x + bar_w + 6.0f, y, DASH_COL_TEXT_DIM, "%u / %u",
                      rows[ i ].used, rows[ i ].cap );

        if ( dash_tip_at( ( gui_rect_t ){ r.x, y, r.w, row_h } ) )
        {
            if ( gui_tooltip_begin() )
            {
                gui_stack();
                for ( u32 t = 0; t < 3 && tip[ i ][ t ]; ++t )
                    gui_text( tip[ i ][ t ] );
            }
            gui_tooltip_end();
        }
    }

    /* Glyph share of the DRAWN quads -- what a per-run text record would collapse.  Follows the
       "quads" bar's band filter so the two numbers are read against each other, and spells out
       glyphs-per-run: that is the factor one 16-byte quad record would amortise over.

       Measured against live_quads, not the arena write head the bar above shows: the head includes
       every slot's quad_alloc padding (SLOT_QUAD_PAD minimum each), which on a small UI can outweigh
       the geometry itself.  The trailing "reserved" figure spells out that gap. */
    u32 tq = inc ? sn->text_quads : sn->band0_text_quads;
    u32 tr = inc ? sn->text_runs  : sn->band0_text_runs;
    u32 vq = inc ? sn->live_quads : sn->band0_live_quads;
    u32 rq = inc ? sn->tess_quads : sn->band0_quad_end;

    /* Four short lines rather than two long ones -- the dashboard docks narrow, and a single wide
       line is the first thing to run off the panel and ellipsize.  They ride the BARS' two columns:
       label at r.x, value at bar_x, so the whole panel reads as one aligned table instead of a
       chart with a paragraph under it. */
    char gb[ 16 ];
    dash_bytes( sn->tess_quads * (u32)GUI_QUAD_BYTES + sn->tess_prims * (u32)GUI_PRIM_BYTES,
                gb, sizeof( gb ) );

    f32 sy = r.y + r.h - lh * 4.0f - 1.0f;
    struct { const char* label; char val[ 72 ]; } sum[ 4 ];

    fmt_snprintf( sum[ 0 ].val, sizeof( sum[ 0 ].val ), "%u / %u quads drawn  (%.0f%%)",
                  tq, vq, vq ? 100.0f * (f32)tq / (f32)vq : 0.0f );
    fmt_snprintf( sum[ 1 ].val, sizeof( sum[ 1 ].val ), "%u  (%.1f glyphs/run)   %u quads reserved",
                  tr, tr ? (f32)tq / (f32)tr : 0.0f, rq );
    /* The observer's own share of the shared pools -- honest attribution, not hidden. */
    fmt_snprintf( sum[ 2 ].val, sizeof( sum[ 2 ].val ), "%u cmds  %u quads",
                  sn->emit_cmds_dbg, sn->tess_quads - sn->band0_quad_end );
    /* What this frame's tessellation occupies in ONE of the GPU tables' (frame-in-flight,
       viewport) regions -- quads plus styles.  It is the frame's own footprint, not a cap. */
    fmt_snprintf( sum[ 3 ].val, sizeof( sum[ 3 ].val ), "%s of quads + styles", gb );

    sum[ 0 ].label = "glyphs";
    sum[ 1 ].label = "runs";
    sum[ 2 ].label = "dbg band";
    sum[ 3 ].label = "gpu bytes";

    for ( u32 i = 0; i < 4; ++i )
    {
        f32 ly = sy + (f32)i * lh;
        dash_text( r.x + 2.0f, ly, bar_x - 10.0f, DASH_COL_TEXT_DIM, sum[ i ].label );
        dash_text( bar_x,      ly, r.x + r.w,     DASH_COL_TEXT_DIM, sum[ i ].val   );
    }
}

/* Volatile registry: one row per captured sub-slot with live-vs-reserved mini bars. */
static void
dash_panel_volatile( gui_rect_t r, const dash_snapshot_t* sn )
{
    const f32 row_h = font_line_h() + 2.0f;
    f32       y     = r.y + 2.0f;

    if ( sn->vol_count == 0 )
    {
        dash_text( r.x + 2.0f, y, r.x + r.w, DASH_COL_TEXT_DIM, "no volatile widgets registered" );
        return;
    }

    /* Nothing else in this panel draws into the status dot / bar-pair pixels, so every row's
       fills can batch into one flush at the end instead of 5 gui_draw_rect calls each. */
    dash_rect_batch_t rb = { 0 };

    for ( u32 v = 0; v < sn->vol_count; ++v )
    {
        if ( y + row_h > r.y + r.h ) break;
        const dash_vol_t* vo = &sn->vols[ v ];
        char nb[ 12 ], wb[ 12 ];

        /* Generation check against the owner slot: a mismatch means patches are not landing.  The
           same scan yields the owner's band, so a debug-band volatile is filtered with the toggle. */
        u32  slot_gen  = 0;
        u32  slot_band = 0;
        bool have_own  = false;
        for ( u32 i = 0; i < sn->slot_count; ++i )
            if ( sn->slots[ i ].win == vo->win )
            { slot_gen = sn->slots[ i ].tess_gen; slot_band = sn->slots[ i ].band; have_own = true; break; }
        if ( slot_band != 0 && !s_show_second_band ) continue;   /* debug-band owner: omit by default */
        bool stale = have_own && vo->active && vo->tess_gen != slot_gen;

        dash_rb_push( &rb, r.x + 2.0f, y + 4.0f, 6.0f, 6.0f,
                      vo->active ? ( vo->hidden ? DASH_COL_WARN : DASH_COL_OK ) : DASH_COL_FIF_IDLE );
        dash_text( r.x + 14.0f,  y, r.x + 116.0f, DASH_COL_TEXT, dash_name( vo->id, nb, sizeof( nb ) ) );
        dash_text( r.x + 120.0f, y, r.x + 210.0f, DASH_COL_TEXT_DIM, dash_name( vo->win, wb, sizeof( wb ) ) );

        f32 bx = r.x + 216.0f, bw = 70.0f, bh = row_h - 6.0f;
        f32 vfrac = vo->quad_alloc ? (f32)vo->quad_count / (f32)vo->quad_alloc : 0.0f;
        dash_rb_push( &rb, bx, y + 3.0f, bw, bh, DASH_COL_BG );
        dash_rb_push( &rb, bx, y + 3.0f, bw * vfrac, bh, DASH_COL_SPAN_VERT );

        dash_textf( bx + bw + 14.0f, y, r.x + r.w, stale ? DASH_COL_BAD : DASH_COL_TEXT_DIM,
                    stale ? "gen %u STALE" : "gen %u", vo->tess_gen );

        if ( dash_tip_at( ( gui_rect_t ){ r.x, y, r.w, row_h } ) )
        {
            if ( gui_tooltip_begin() )
            {
                gui_stack();
                gui_textf( "volatile %s  in %s", dash_name( vo->id, nb, sizeof( nb ) ),
                           dash_name( vo->win, wb, sizeof( wb ) ) );
                gui_textf( "quads +%u  %u / %u reserved", vo->lquad_base, vo->quad_count,
                           vo->quad_alloc );
                gui_textf( "cmds  %u / %u   gen %u   %s%s", vo->cmd_count, vo->cmd_alloc,
                           vo->tess_gen, vo->active ? "active" : "retired",
                           vo->hidden ? " (hidden)" : "" );
            }
            gui_tooltip_end();
        }
        y += row_h;
    }

    dash_rb_flush( &rb );
}

/* Frame stats strip (last published frame + capture state). */
static void
dash_panel_stats( gui_rect_t r, const dash_snapshot_t* sn )
{
    const gui_render_stats_t* st = &sn->stats;
    const f32                 lh = font_line_h();
    bool                      fz = dash_frozen();

    char upb[ 16 ];
    dash_textf( r.x + 2.0f, r.y, r.x + r.w, DASH_COL_TEXT,
                "draws %u (hwm %u)   upload %s / %u wr   vol patched %u",
                st->draw_calls, sn->draw_call_hwm,
                dash_bytes( st->upload_bytes, upb, sizeof( upb ) ), st->upload_batches,
                st->volatile_patched );
    /* Application cost, debug band excluded (gui_render_stats_t) -- so these are the numbers a
       real UI is answerable for, not the ones this window inflates. */
    dash_textf( r.x + 2.0f, r.y + lh, r.x + r.w, DASH_COL_TEXT,
                "app: %u/%u wins ret   %u/%u quads ret   %u quads -> %u tris   %u style recs",
                st->win_retained, st->win_total, st->quad_retained, st->quad_count,
                st->quad_count, st->quad_count * 2u, st->prim_unique );
    dash_textf( r.x + 2.0f, r.y + 2.0f * lh, r.x + r.w,
                fz ? DASH_COL_WARN : DASH_COL_TEXT_DIM,
                "capture #%u%s   any_changed %u   unchanged %u   gen %u%s",
                sn->serial, fz ? "  FROZEN" : "", (u32)sn->any_changed,
                sn->diff_unchanged, sn->tess_gen_next,
                sn->overflow_ever ? "   OVERFLOWED" : "" );
}

/*==============================================================================================
    Window shell
==============================================================================================*/

static void
dash_shell_panel( const char* title, f32 h,
                  void ( *painter )( gui_rect_t, const dash_snapshot_t* ) )
{
    gui_separator_text( title );
    painter( gui_canvas( h ), dash_snapshot() );
}


void
dash_window( bool* open )
{
    /* Gate the backend captures first, open or not -- a closed dashboard costs two branches. */
    bool is_open = ( open && *open );
    dash_set_enabled( is_open );
    if ( !is_open )
        return;

    g_dash_window_id = id_hash( DASH_SHELL_TITLE );

    /* The host said open: reopen the pool entry if the X button hid it on an earlier run. */
    gui_window_set_open( DASH_SHELL_TITLE, true );

    gui_window_set_next_size( 580.0f, 780.0f, GUI_COND_ONCE );
    if ( gui_window_begin( DASH_SHELL_TITLE, GUI_WIN_CLOSEABLE | GUI_WIN_DEBUG_BAND ) )
    {
        gui_stack();
        s_tip_done = false;

        /* All view toggles on one line.  Second band folds the dashboard's own debug-band geometry
           back into EVERY figure -- arena maps, high-water marks, map scale, and the emit/build
           bars -- off by default so the whole picture is a real application's usage with the
           self-measuring observer filtered out; full range scales the maps to the cap vs the hwm;
           show pad breaks out each slot's alloc headroom vs one flat fill. */
        bool frozen = dash_frozen();
        if ( gui_checkbox( "Freeze", &frozen ) )
            dash_set_freeze( frozen );
        gui_same_line( -1.0f );
        gui_checkbox( "Second band", &s_show_second_band );
        gui_same_line( -1.0f );
        gui_checkbox( "Full range", &s_full_range );
        gui_same_line( -1.0f );
        gui_checkbox( "Show pad", &s_show_pad );

        /* Panel heights derive from the live line height so text rows never clip mid-glyph. */
        f32 lh = font_line_h();

        dash_shell_panel( "Quad arena -- slot memory map",  lh + 58.0f,                  dash_panel_memmap   );
        dash_shell_panel( "Frames in flight / uploads",     96.0f,                       dash_panel_fif      );
        dash_shell_panel( "Draw batches (dispatch order)",  8.0f * ( lh + 3.0f ) + 6.0f, dash_panel_batch    );
        /* Nine bars plus four summary lines -- sized from the row count so adding a pool does not
           silently squeeze every bar. */
        dash_shell_panel( "Pool fill vs cap",               9.0f * ( lh + 2.0f ) + lh * 4.0f + 8.0f,
                                                                                         dash_panel_emit    );
        dash_shell_panel( "Volatile sub-slots",             4.0f * ( lh + 2.0f ) + lh,   dash_panel_volatile );
        dash_shell_panel( "Frame stats",                    3.0f * lh + 10.0f,           dash_panel_stats    );
    }
    gui_window_end();

    /* The X button closed it this frame: report back so the host toggle stays in sync. */
    if ( !gui_window_is_open( DASH_SHELL_TITLE ) )
        *open = false;
}

#else  /* !GUI_PIPELINE_DASHBOARD */

/* No-op stub: the vtable slot exists in every build so func_api_size is identical across a
   hot-reload (the debug-slot ABI rule, gui_api.h). */
void
dash_window( bool* open )
{
    (void)open;
}

#endif /* GUI_PIPELINE_DASHBOARD */

// clang-format on
/*============================================================================================*/
