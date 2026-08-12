/*==============================================================================================

    runtime_service/gui/flow/gui_region.c -- Root-level region: a caller-positioned rect with
    no window chrome, for HUD-style elements that just need to draw and take input somewhere
    on screen.

    window_begin_ex (gui_window_free.c) and child_begin (gui_layout_child.c) both build on the
    shared scroll-region engine in gui_scroll.c, but neither is minimal: a window carries a
    pool record, drag/resize, and dock/native-surface sync even with every one of those paths
    flagged off; a child_begin box needs an already-active parent frame to carve its rect from.
    gui_region_begin/end is the third, minimal caller -- an explicit rect, persisted scroll +
    content-measure state (autosize on h/w <= 0, exactly like child_begin's AutoResizeY), and
    the same draw-state stamp window_begin_ex applies -- with no pool slot, no drag/resize/dock,
    no title, no background fill.

    The rect lives in the target viewport's client space (`vp`; GUI_VP_MAIN = primary,
    GUI_VP_INVALID or a torn-down viewport both resolve to it). It competes for hover_win in
    the same z contest windows and popups use (surface_hover_nominate, core/gui_surface.c), so
    it is interactive by default at the caller's chosen tier (gui_region_tier_t, see gui.h);
    opt out with GUI_WIN_NO_INPUT, the same flag a window honors.

    Included by gui_flow.c after the sub-layout / split files (needs layout_push/pop_region
    and REGION_PAD_DEFAULT) -- no dependency of its own.

==============================================================================================*/
// clang-format off

static gui_scroll_link_t*
region_root_scroll_get( gui_id_t id )
{
    /*  Keyed persistent scroll + content-measure state for a basic region without resize.
        The full gui_region_t state has user_w/user_h (resize grip) we don't use */

    return GUI_STATE( gui_scroll_link_t, id );
}

/*==============================================================================================

    A static region is a HUD-style rect that competes for hover_win at the caller's
    chosen z tier. It is interactive by default, but opt out with GUI_WIN_NO_INPUT.

==============================================================================================*/

bool
gui_region_begin( const char* id_str, f32 x, f32 y, f32 w, f32 h, gui_region_tier_t tier,
                  i32 vp, gui_win_flags_t flags )
{
    gui_id_t            id     = id_hash( id_str );
    gui_scroll_link_t*  scroll = region_root_scroll_get( id );

    DBG_NAME( id, id_str );

    /* this flag isn't used so ensure users just don't apply it */
    ORB_ASSERT( flags & GUI_WIN_ALWAYS_AUTOSIZE );

    vp = vp_resolve( vp ); /* GUI_VP_INVALID and a torn-down surface both mean the primary */

    /* Land this viewport's DPI bake before any metric below (WIDGET_H, REGION_PAD_DEFAULT)
       reads s_style -- differently-scaled monitors carry different bakes. No-op once landed. */

    gui_dpi_land( vp );

    /* Autosize on either axis, exactly like child_begin's h <= 0 (AutoResizeY): hug last frame's
       measured content once one exists, else open one widget-row tall. layout_push_region insets
       the view from this box by WIN_BORDER (view_w by 2x, left+right; view_h by 1x), so the
       border is added back here -- otherwise the view comes out a border short of the content
       that was just measured against a full-width view, and region_gutters reads that shortfall
       as overflow, reserving a scrollbar gutter no actual content needs. */
    if ( w <= 0.0f ) w = ( scroll->content_w > 0.0f ) ? scroll->content_w + 2.0f * WIN_BORDER : WIDGET_H * 4.0f;
    if ( h <= 0.0f ) h = ( scroll->content_h > 0.0f ) ? scroll->content_h + WIN_BORDER : WIDGET_H;

    gui_rect_t box = { x, y, w, h };

    /* z tier maps the caller's three-way choice onto the band map (gui_region_tier_t, gui.h). */
    u32 z = ( tier == GUI_REGION_BG ) ? GUI_REGION_BG_Z
          : ( tier == GUI_REGION_FG ) ? GUI_REGION_FG_Z
          :                             GUI_REGION_Z;

    /* A region IS a pane + scroll layout: pane_tag (frame/gui_pane.c) stamps the draw state
       (retained-cache key, z tier, host viewport, arena band) and commits the interaction
       scope so item_state attributes this region's widgets against hover_win. */
    pane_tag( id, z, vp, ( flags & GUI_WIN_DEBUG_BAND ) ? 1u : 0u );

    /* Base clip = this region's own surface, not whatever painted last -- same root-clip stamp
       window_begin_ex applies; region_end hands the primary's clip back. */

    draw_set_root_clip( vp_w( vp ), vp_h( vp ) );

    /* Interactive by default -- enter the hover_win contest at this region's z tier so its
       widgets can go hot/active. Opt out with GUI_WIN_NO_INPUT for a pure HUD. */

    if ( !( flags & GUI_WIN_NO_INPUT ) )
        surface_hover_nominate( id, box, z, vp );

    /* layout_push_region intersects its clip against s_scope.clip as "the parent clip", which
       is correct for child_begin's real parent frame. A root region has none: s_scope.clip here
       is leftover from whatever unrelated window painted last, and intersecting against it can
       silently empty this region's hit-test clip. Reset to the host surface's full rect first,
       the same plain (non-intersecting) assignment window_begin_ex makes. */

    s_scope.clip = ( gui_rect_t ){ 0.0f, 0.0f, vp_w( vp ), vp_h( vp ) };

    /* Not an item, so clear any disabled latch a prior widget left before this region's first
       widget opens. */
    item_flags_chrome_reset();

    layout_push_region( id, box, REGION_PAD_DEFAULT, flags, scroll,
                        /* own_clip */ !( flags & GUI_WIN_NO_CLIP ) );
    return true;
}

void
gui_region_end( void )
{
    layout_pop_region();

    /* Restore the main display's root clip for whatever paints next at root level -- same
       hand-back window_end performs. */
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );
}

// clang-format on
/*============================================================================================*/
