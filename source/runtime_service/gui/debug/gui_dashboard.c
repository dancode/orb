/*==============================================================================================

    runtime_service/gui/debug/gui_dashboard.c -- Pipeline dashboard: window shell + panel painters.

    A visual diagnostic of the gui render backend: memory maps of the shared vertex/index
    arena (per-window slots, pads, volatile sub-slots, the debug-band boundary), frames in
    flight with upload spans, the dispatch-order batch inspector with batch-cut causes, EMIT
    pool usage bars, the volatile registry, and a frame-stats strip.

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

    Data comes from the backend capture (render/gui_dash_capture.c) via gui_dash_snapshot():
    a coherent copy taken at the end of cache_build_frame / gui_render_flush.  The shell emits
    one frame after a capture, so the display lags the pipeline by one frame -- the standard
    self-measurement lag.  Hover tooltips resolve against the same snapshot the bars were drawn
    from, so they always agree.

    Emitted internally (debug_overlays_emit, gui_frame_overlay.c) at the default context's
    ctx_end while debug_enable is on; the F10 hotkey owns the open flag and the window's X
    button writes it back to false.  Included by gui.c after the popup tier (tooltips) and
    before gui_frame_overlay.c / gui_frame.c.  Compiled out unless GUI_PIPELINE_DASHBOARD
    (gui_render.h); gui_pipeline_dashboard stays a no-op stub then.

==============================================================================================*/
// clang-format off

/* The dashboard window's id -- stays 0 when the feature is compiled out or never emitted.
   Used to mark the dashboard's own slot in the memory map and to gate the hover tooltips. */
gui_id_t g_gui_dash_window_id = 0;

#ifdef GUI_PIPELINE_DASHBOARD

#define DASH_SHELL_TITLE "Pipeline Dashboard"   /* id_hash of this = g_gui_dash_window_id */

/*==============================================================================================
    Colors
==============================================================================================*/

#define DASH_COL_TEXT       GUI_COLOR( 0xD8, 0xD8, 0xD8, 0xFF )
#define DASH_COL_TEXT_DIM   GUI_COLOR( 0x90, 0x90, 0x90, 0xFF )
#define DASH_COL_BG         GUI_COLOR( 0x14, 0x14, 0x18, 0xFF )
#define DASH_COL_HWM        GUI_COLOR( 0xF0, 0xE0, 0x40, 0xE0 )
#define DASH_COL_OVERFLOW   GUI_COLOR( 0xEE, 0x40, 0x30, 0xFF )
#define DASH_COL_CHANGED    GUI_COLOR( 0xFF, 0xFF, 0xFF, 0xD0 )
#define DASH_COL_SELF       GUI_COLOR( 0x50, 0xE0, 0xF0, 0xFF )
#define DASH_COL_VOLATILE   GUI_COLOR( 0xFF, 0xFF, 0xFF, 0x60 )
#define DASH_COL_FIF_ACTIVE GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
#define DASH_COL_FIF_IDLE   GUI_COLOR( 0x40, 0x40, 0x48, 0xFF )
#define DASH_COL_SPAN_VERT  GUI_COLOR( 0x66, 0xBB, 0xEE, 0xFF )
#define DASH_COL_SPAN_IDX   GUI_COLOR( 0xBB, 0x88, 0xEE, 0xFF )
#define DASH_COL_OK         GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
#define DASH_COL_WARN       GUI_COLOR( 0xE0, 0xC0, 0x40, 0xFF )
#define DASH_COL_BAD        GUI_COLOR( 0xEE, 0x55, 0x44, 0xFF )
#define DASH_COL_CUT_TEX    GUI_COLOR( 0xF0, 0xC0, 0x40, 0xFF )
#define DASH_COL_CUT_CLIP   GUI_COLOR( 0xE0, 0x50, 0xE0, 0xFF )
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

static void
dash_outline( gui_rect_t r, u32 abgr )
{
    if ( r.w <= 0.0f || r.h <= 0.0f ) return;
    gui_draw_round_rect( r, 0.0f, 0.0f, 0.0f, 0.0f, false, 1.0f, abgr );
}

/* Dashed vertical marker (high-water / band-boundary lines). */
static void
dash_vline( f32 x, f32 y0, f32 y1, u32 abgr )
{
    gui_draw_dashed_line( x, y0, x, y1, 3.0f, 3.0f, 1.0f, abgr );
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
    if ( s_interaction.hover_win != g_gui_dash_window_id )
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

/* Shared memory-map body: the whole tess vertex (or index) arena as one horizontal bar.
   Main-band slots first, the dashed band boundary, then (only when "Show second band" is on) the
   debug band's own slots dimmed -- the observer marked, not hidden. */
static void
dash_panel_memmap( gui_rect_t r, bool vb_axis, const dash_snapshot_t* sn )
{
    const f32 lh  = font_line_h();
    const u32 cap = vb_axis ? GUI_MAX_VERTS : GUI_MAX_IDX;

    /* Header + arena scale honor the "Second band" toggle.  OFF (default): every figure -- used,
       high-water and the bar scale -- is the MAIN band alone, so the map reads as a real
       application's arena with the self-measuring dashboard filtered out.  ON: totals include the
       debug band and the band0/debug split is spelled out. */
    const char* axis  = vb_axis ? "verts" : "indices";
    u32         total = vb_axis ? sn->tess_verts     : sn->tess_idx;
    u32         main0 = vb_axis ? sn->band0_vert_end : sn->band0_idx_end;
    u32         pad   = vb_axis ? (u32)SLOT_VERT_PAD : (u32)SLOT_IDX_PAD;
    u32         used  = s_show_second_band ? total : main0;
    u32         hwm   = s_show_second_band ? ( vb_axis ? sn->vert_hwm       : sn->idx_hwm )
                                           : ( vb_axis ? sn->band0_vert_hwm : sn->band0_idx_hwm );
    if ( s_show_second_band )
        dash_textf( r.x + 2.0f, r.y, r.x + r.w, DASH_COL_TEXT_DIM,
                    "%s  %u / %u   band0 %u  debug %u   hwm %u   pad %u",
                    axis, used, cap, main0, total - main0, hwm, pad );
    else
        dash_textf( r.x + 2.0f, r.y, r.x + r.w, DASH_COL_TEXT_DIM,
                    "%s  %u / %u   hwm %u   pad %u   (band 0)", axis, used, cap, hwm, pad );

    gui_rect_t bar = { r.x, r.y + lh + 2.0f, r.w, r.h - lh - 4.0f };
    if ( bar.h < 12.0f ) return;

    gui_draw_rect( bar.x, bar.y, bar.w, bar.h, DASH_COL_BG );   /* free space: flat backdrop */

    /* Scale the arena to the full cap, or (default) to the high-water mark so the live slots
       render wider.  Everything drawn (base + alloc) sits at or below hwm, so nothing clips. */
    u32 scale  = ( s_full_range || hwm == 0 ) ? cap : hwm;
    f32 px_per = bar.w / (f32)scale;

    for ( u32 i = 0; i < sn->slot_count; ++i )
    {
        const dash_slot_t* sl = &sn->slots[ i ];
        if ( !sl->valid ) continue;
        if ( sl->band != 0 && !s_show_second_band ) continue;   /* second band omitted by default */

        u32 base  = vb_axis ? sl->vert_base  : sl->idx_base;
        u32 count = vb_axis ? sl->vert_count : sl->idx_count;
        u32 alloc = vb_axis ? sl->vert_alloc : sl->idx_alloc;

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
            gui_draw_rect( x0, bar.y, xa - x0, bar.h, col );   /* whole reservation as one flat fill */
        }

        /* Volatile sub-slots: brighter strips over the full height of their owner's extent. */
        if ( vb_axis )
        {
            for ( u32 v = 0; v < sn->vol_count; ++v )
            {
                const dash_vol_t* vo = &sn->vols[ v ];
                if ( !vo->active || vo->win != sl->win ) continue;
                f32 vx0 = bar.x + (f32)( base + vo->lvert_base ) * px_per;
                f32 vx1 = bar.x + (f32)( base + vo->lvert_base + vo->vert_alloc ) * px_per;
                gui_rect_t vr = { vx0, bar.y + 1.0f, vx1 - vx0, bar.h - 2.0f };
                gui_draw_rect( vr.x, vr.y, vr.w, vr.h, DASH_COL_VOLATILE );

                if ( dash_tip_at( vr ) )
                {
                    char nb[ 12 ], wb[ 12 ];
                    if ( gui_tooltip_begin() )
                    {
                        gui_stack();
                        gui_textf( "volatile %s  in %s", dash_name( vo->id, nb, sizeof( nb ) ),
                                   dash_name( vo->win, wb, sizeof( wb ) ) );
                        gui_textf( "verts +%u  %u / %u reserved", vo->lvert_base, vo->vert_count,
                                   vo->vert_alloc );
                        gui_textf( "idx   +%u  %u / %u reserved", vo->lidx_base, vo->idx_count,
                                   vo->idx_alloc );
                    }
                    gui_tooltip_end();
                }
            }
        }

        /* Diff pulse / self-marker outlines. */
        gui_rect_t seg = { x0, bar.y, xa - x0, bar.h };
        if ( sl->win == g_gui_dash_window_id )
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
                           sl->win == g_gui_dash_window_id ? "  (this dashboard)" : "",
                           sl->band != 0 ? "  [debug band]" : "" );
                gui_textf( "verts   [%u..%u)  alloc %u  (pad %u)", sl->vert_base,
                           sl->vert_base + sl->vert_count, sl->vert_alloc,
                           sl->vert_alloc - sl->vert_count );
                gui_textf( "indices [%u..%u)  alloc %u  (pad %u)", sl->idx_base,
                           sl->idx_base + sl->idx_count, sl->idx_alloc,
                           sl->idx_alloc - sl->idx_count );
                gui_textf( "cmds    [%u..%u)   z %u  vp %u  gen %u", sl->cmd_base,
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
    const bool frozen = gui_dash_frozen();   /* the active region rotates every frame; call it out
                                                only when frozen so it never strobes at framerate */

    for ( u32 vp = 0; vp < GUI_MAX_VIEWPORTS; ++vp )
    {
        if ( !sn->surf[ vp ].live ) continue;
        if ( y + row_h > r.y + r.h ) break;

        const dash_surf_t* sf = &sn->surf[ vp ];

        dash_textf( r.x + 2.0f, y + 3.0f, r.x + 40.0f, DASH_COL_TEXT, "vp%u", vp );

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
            if ( act && sf->vtx_hi > sf->vtx_lo )
            {
                /* Two stacked sub-bars in the box interior: vertex span on top, index span below,
                   parted by a gutter so the two regions read as distinct rather than one block. */
                f32 inner_x = box.x + 1.0f;
                f32 inner_w = box.w - 2.0f;
                f32 gutter  = 2.0f;
                f32 half    = ( box.h - 4.0f - gutter ) * 0.5f;
                f32 vy      = box.y + 2.0f;
                f32 iy      = vy + half + gutter;
                f32 vx0     = inner_x + inner_w * (f32)sf->vtx_lo / (f32)GUI_MAX_VERTS;
                f32 vx1     = inner_x + inner_w * (f32)sf->vtx_hi / (f32)GUI_MAX_VERTS;
                gui_draw_rect( vx0, vy, vx1 - vx0, half, DASH_COL_SPAN_VERT );
                f32 ix0     = inner_x + inner_w * (f32)sf->idx_lo / (f32)GUI_MAX_IDX;
                f32 ix1     = inner_x + inner_w * (f32)sf->idx_hi / (f32)GUI_MAX_IDX;
                gui_draw_rect( ix0, iy, ix1 - ix0, half, DASH_COL_SPAN_IDX );
            }
        }

        dash_textf( r.x + r.w * 0.55f + 8.0f, y + 3.0f, r.x + r.w, DASH_COL_TEXT_DIM,
                    "up %u B  %u wr  %u draw", sf->up_bytes, sf->up_batches, sf->draw_calls );

        if ( dash_tip_at( ( gui_rect_t ){ r.x, y, r.w, row_h } ) )
        {
            if ( gui_tooltip_begin() )
            {
                gui_stack();
                gui_textf( "surface vp%u  in-flight region %u of %u", vp, sf->frame_index,
                           (u32)RHI_MAX_FRAMES_IN_FLIGHT );
                if ( sf->vtx_hi > sf->vtx_lo )
                    gui_textf( "uploaded verts [%u..%u)  idx [%u..%u)", sf->vtx_lo, sf->vtx_hi,
                               sf->idx_lo, sf->idx_hi );
                else
                    gui_textf( "nothing uploaded (fully retained)" );
                gui_textf( "%u B in %u writes   %u draw calls", sf->up_bytes, sf->up_batches,
                           sf->draw_calls );
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
    const f32 bars_x = r.x + 320.0f;   /* wider name + meta column before the command bars */

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
        char               nb[ 12 ];

        gui_draw_rect( r.x + 2.0f, y + 3.0f, 8.0f, 8.0f, col );
        dash_text( r.x + 14.0f, y, r.x + 190.0f, DASH_COL_TEXT,
                   dash_name( sl->win, nb, sizeof( nb ) ) );
        dash_textf( r.x + 194.0f, y, bars_x - 4.0f, DASH_COL_TEXT_DIM, "z%-2u v%u g%u",
                    sl->z, sl->vp, sl->tess_gen );

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
                /* Why this command split from the previous one -- the batch-cut cause. */
                const dash_cmd_t* pc  = &sn->cmds[ sl->cmd_base + k - 1 ];
                u32               cut = ( dc->tex_idx != pc->tex_idx ) ? DASH_COL_CUT_TEX
                                      : ( dc->clip.x != pc->clip.x || dc->clip.y != pc->clip.y
                                       || dc->clip.w != pc->clip.w || dc->clip.h != pc->clip.h )
                                                                       ? DASH_COL_CUT_CLIP
                                                                       : DASH_COL_CUT_FORCE;
                gui_draw_rect( bx - 2.0f, y + 1.0f, 1.0f, row_h - 3.0f, cut );
            }

            u32 bcol = ( dc->tex_idx == sn->font_atlas )
                     ? ( col | 0xFF000000u )
                     : GUI_COLOR( 0x50, 0xC0, 0xB0, 0xFF );   /* icon/other atlas */
            gui_rect_t bar = { bx, y + 2.0f, bw, row_h - 5.0f };
            gui_draw_rect( bar.x, bar.y, bar.w, bar.h, bcol );

            if ( dash_tip_at( bar ) )
            {
                if ( gui_tooltip_begin() )
                {
                    gui_stack();
                    gui_textf( "draw %u of %s", k, dash_name( sl->win, nb, sizeof( nb ) ) );
                    gui_textf( "%u indices (%u tris)  tex %u", dc->elem_count,
                               dc->elem_count / 3u, dc->tex_idx );
                    gui_textf( "clip %.0f,%.0f  %.0fx%.0f", dc->clip.x, dc->clip.y,
                               dc->clip.w, dc->clip.h );
                    gui_textf( "vbase %u  first_index %u", dc->vbase, dc->ibase );
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
                gui_textf( "window  %s", dash_name( sl->win, nb, sizeof( nb ) ) );
                gui_textf( "%u draw cmds  %u verts  %u tris", sl->cmd_count, sl->vert_count,
                           sl->idx_count / 3u );
                gui_textf( "z %u  vp %u  gen %u  %s", sl->z, sl->vp, sl->tess_gen,
                           sl->changed ? "re-tessellated" : "retained" );
            }
            gui_tooltip_end();
        }
        y += row_h;
    }
}

/* EMIT/BUILD usage bars vs caps, graded green -> amber -> red, with hwm ticks where tracked,
   plus the debug band's own attributed footprint on the last line. */
static void
dash_panel_emit( gui_rect_t r, const dash_snapshot_t* sn )
{
    static const char* tip[] = { "semantic draw commands", "command segments",
                                 "polyline points", "draw_rects batch entries",
                                 "text pool bytes", "clip rects",
                                 "tessellated vertices", "tessellated indices",
                                 "GPU draw commands" };
    /* With "Second band" OFF (default) each bar is the main band alone (total minus the debug-band
       share the capture attributed), so the bars measure a real application against the caps.  The
       debug band's own footprint is always spelled out on the summary line below. */
    const bool inc = s_show_second_band;
    struct { const char* name; u32 used, cap, hwm; } rows[] = {
        { "cmds",  inc ? sn->emit_cmds  : sn->emit_cmds  - sn->emit_cmds_dbg,  GUI_MAX_CMDS,       inc ? sn->emit_cmds_hwm : 0 },
        { "segs",  inc ? sn->emit_segs  : sn->emit_segs  - sn->emit_segs_dbg,  GUI_MAX_SEGS,       0                           },
        { "pts",   inc ? sn->emit_pts   : sn->emit_pts   - sn->emit_pts_dbg,   GUI_MAX_PATH_PTS,   0                           },
        { "rects", inc ? sn->emit_rects : sn->emit_rects - sn->emit_rects_dbg, GUI_MAX_RECT_ENTRIES, 0                         },
        { "text",  inc ? sn->emit_text  : sn->emit_text  - sn->emit_text_dbg,  GUI_MAX_TEXT_POOL,  0                           },
        { "clips", inc ? sn->emit_clips : sn->emit_clips - sn->emit_clips_dbg, GUI_MAX_CLIP_RECTS, 0                           },
        { "verts", inc ? sn->tess_verts : sn->band0_vert_end, GUI_MAX_VERTS,   inc ? sn->vert_hwm : sn->band0_vert_hwm         },
        { "idx",   inc ? sn->tess_idx   : sn->band0_idx_end,  GUI_MAX_IDX,     inc ? sn->idx_hwm  : sn->band0_idx_hwm          },
        { "draws", inc ? sn->tess_cmds  : sn->tess_cmds - sn->tess_cmds_dbg,   GUI_MAX_CMDS,       0                           },
    };
    const u32 n     = sizeof( rows ) / sizeof( rows[ 0 ] );
    const f32 lh    = font_line_h();
    const f32 row_h = ( r.h - lh - 4.0f ) / (f32)n;
    const f32 bar_x = r.x + 84.0f;    /* label column + a clear gap before the bars */
    const f32 val_w = 132.0f;         /* value column: fits a full "NNNNN / NNNNN" with no ellipsis */
    const f32 bar_w = r.w - 84.0f - val_w;

    for ( u32 i = 0; i < n; ++i )
    {
        f32 y    = r.y + 2.0f + (f32)i * row_h;
        f32 frac = rows[ i ].cap ? (f32)rows[ i ].used / (f32)rows[ i ].cap : 0.0f;
        u32 col  = frac > 0.90f ? DASH_COL_BAD : frac > 0.75f ? DASH_COL_WARN : DASH_COL_OK;

        dash_text( r.x + 2.0f, y, bar_x - 10.0f, DASH_COL_TEXT_DIM, rows[ i ].name );
        gui_draw_rect( bar_x, y + 2.0f, bar_w, row_h - 4.0f, DASH_COL_BG );
        gui_draw_rect( bar_x, y + 2.0f, bar_w * frac, row_h - 4.0f, col );
        if ( rows[ i ].hwm )
            dash_vline( bar_x + bar_w * (f32)rows[ i ].hwm / (f32)rows[ i ].cap,
                        y + 1.0f, y + row_h - 1.0f, DASH_COL_HWM );
        dash_num( bar_x + bar_w + 6.0f, y, DASH_COL_TEXT_DIM, "%u / %u",
                  rows[ i ].used, rows[ i ].cap );

        if ( dash_tip_at( ( gui_rect_t ){ r.x, y, r.w, row_h } ) )
        {
            if ( gui_tooltip_begin() )
            {
                gui_stack();
                gui_textf( "%s", tip[ i ] );
            }
            gui_tooltip_end();
        }
    }

    /* The observer's own share of the shared pools -- honest attribution, not hidden. */
    dash_textf( r.x + 2.0f, r.y + r.h - lh - 1.0f, r.x + r.w, DASH_COL_TEXT_DIM,
                "debug band:  %u cmds   %u verts   %u idx", sn->emit_cmds_dbg,
                sn->tess_verts - sn->band0_vert_end, sn->tess_idx - sn->band0_idx_end );
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

        gui_draw_rect( r.x + 2.0f, y + 4.0f, 6.0f, 6.0f,
                       vo->active ? ( vo->hidden ? DASH_COL_WARN : DASH_COL_OK ) : DASH_COL_FIF_IDLE );
        dash_text( r.x + 14.0f,  y, r.x + 116.0f, DASH_COL_TEXT, dash_name( vo->id, nb, sizeof( nb ) ) );
        dash_text( r.x + 120.0f, y, r.x + 210.0f, DASH_COL_TEXT_DIM, dash_name( vo->win, wb, sizeof( wb ) ) );

        f32 bx = r.x + 216.0f, bw = 70.0f, bh = row_h - 6.0f;
        f32 vfrac = vo->vert_alloc ? (f32)vo->vert_count / (f32)vo->vert_alloc : 0.0f;
        f32 ifrac = vo->idx_alloc  ? (f32)vo->idx_count  / (f32)vo->idx_alloc  : 0.0f;
        gui_draw_rect( bx, y + 3.0f, bw, bh, DASH_COL_BG );
        gui_draw_rect( bx, y + 3.0f, bw * vfrac, bh, DASH_COL_SPAN_VERT );
        gui_draw_rect( bx + bw + 6.0f, y + 3.0f, bw, bh, DASH_COL_BG );
        gui_draw_rect( bx + bw + 6.0f, y + 3.0f, bw * ifrac, bh, DASH_COL_SPAN_IDX );

        dash_textf( bx + 2.0f * bw + 14.0f, y, r.x + r.w, stale ? DASH_COL_BAD : DASH_COL_TEXT_DIM,
                    stale ? "gen %u STALE" : "gen %u", vo->tess_gen );

        if ( dash_tip_at( ( gui_rect_t ){ r.x, y, r.w, row_h } ) )
        {
            if ( gui_tooltip_begin() )
            {
                gui_stack();
                gui_textf( "volatile %s  in %s", dash_name( vo->id, nb, sizeof( nb ) ),
                           dash_name( vo->win, wb, sizeof( wb ) ) );
                gui_textf( "verts +%u  %u / %u reserved", vo->lvert_base, vo->vert_count,
                           vo->vert_alloc );
                gui_textf( "idx   +%u  %u / %u reserved", vo->lidx_base, vo->idx_count,
                           vo->idx_alloc );
                gui_textf( "cmds  %u / %u   gen %u   %s%s", vo->cmd_count, vo->cmd_alloc,
                           vo->tess_gen, vo->active ? "active" : "retired",
                           vo->hidden ? " (hidden)" : "" );
            }
            gui_tooltip_end();
        }
        y += row_h;
    }
}

/* Frame stats strip (last published frame + capture state). */
static void
dash_panel_stats( gui_rect_t r, const dash_snapshot_t* sn )
{
    const gui_render_stats_t* st = &sn->stats;
    const f32                 lh = font_line_h();
    bool                      fz = gui_dash_frozen();

    dash_textf( r.x + 2.0f, r.y, r.x + r.w, DASH_COL_TEXT,
                "draws %u (hwm %u)   upload %u B / %u wr   vol patched %u",
                st->draw_calls, sn->draw_call_hwm, st->upload_bytes, st->upload_batches,
                st->volatile_patched );
    dash_textf( r.x + 2.0f, r.y + lh, r.x + r.w, DASH_COL_TEXT,
                "wins ret %u/%u   verts ret %u/%u   tris ret %u/%u",
                st->win_retained, st->win_total, st->vert_retained, st->vert_count,
                st->tri_retained, st->tri_count );
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
    painter( gui_canvas( h ), gui_dash_snapshot() );
}

/* The memory maps share one painter parameterized by axis -- wrap them for the panel table. */
static void dash_panel_vbmap( gui_rect_t r, const dash_snapshot_t* sn ) { dash_panel_memmap( r, true,  sn ); }
static void dash_panel_ibmap( gui_rect_t r, const dash_snapshot_t* sn ) { dash_panel_memmap( r, false, sn ); }

void
gui_pipeline_dashboard( bool* open )
{
    /* Gate the backend captures first, open or not -- a closed dashboard costs two branches. */
    bool is_open = ( open && *open );
    gui_dash_set_enabled( is_open );
    if ( !is_open )
        return;

    g_gui_dash_window_id = id_hash( DASH_SHELL_TITLE );

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
        bool frozen = gui_dash_frozen();
        if ( gui_checkbox( "Freeze", &frozen ) )
            gui_dash_set_freeze( frozen );
        gui_same_line( -1.0f );
        gui_checkbox( "Second band", &s_show_second_band );
        gui_same_line( -1.0f );
        gui_checkbox( "Full range", &s_full_range );
        gui_same_line( -1.0f );
        gui_checkbox( "Show pad", &s_show_pad );

        /* Panel heights derive from the live line height so text rows never clip mid-glyph. */
        f32 lh = font_line_h();

        dash_shell_panel( "Vertex arena (slot memory map)", lh + 58.0f,                  dash_panel_vbmap    );
        dash_shell_panel( "Index arena (slot memory map)",  lh + 58.0f,                  dash_panel_ibmap    );
        dash_shell_panel( "Frames in flight / uploads",     96.0f,                       dash_panel_fif      );
        dash_shell_panel( "Draw batches (dispatch order)",  8.0f * ( lh + 3.0f ) + 6.0f, dash_panel_batch    );
        dash_shell_panel( "Emit + build buffers",           8.0f * ( lh + 2.0f ) + lh + 8.0f, dash_panel_emit );
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
gui_pipeline_dashboard( bool* open )
{
    (void)open;
}

#endif /* GUI_PIPELINE_DASHBOARD */

// clang-format on
/*============================================================================================*/
