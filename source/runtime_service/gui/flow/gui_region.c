/*==============================================================================================

    runtime_service/gui/flow/gui_region.c -- Root-level region: a fixed-rect layout primitive
    with no window chrome.

    window_begin_ex (chrome/window/gui_window_free.c) and child_begin (gui_layout_child.c) are both
    callers of the shared scroll-region engine in gui_scroll.c, each layered with its own
    bookkeeping -- a persisted, draggable, z-ordered, dockable record for a window; a
    parent-pen-relative box with a resize grip for a child.  Neither fits a HUD-style element
    that just wants a caller-positioned rect on screen: a window drags along the whole pool
    record, dock lookup, and native-surface sync even when every one of those paths is disabled
    by flags, and a child_begin box cannot open without an already-active parent frame to carve
    its box from.

    region_begin / region_end are that third, minimal caller: an explicit screen rect, persisted
    scroll + content-measure state (so h/w <= 0 autosizes to last frame's content, exactly like
    child_begin's AutoResizeY), and the draw-state stamping window_begin_ex does
    (draw_set_window/sort_key/viewport) so the retained-cache dispatch keys correctly -- but no
    slot in the window pool, no drag/resize/dock/native path, no title, no background fill.

    Root-level only: paints on viewport 0 (the main surface); FUTURE: routing a region to a
    non-main viewport.  The z tier is the caller's three-way choice (gui_region_tier_t: MID over
    windows / under popups, BG, FG), and it competes for hover_win in the same z contest windows
    and popups use, so it is interactive by default (opt out with GUI_WIN_NO_INPUT, same flag a
    window honors).

    A region enters the same hover_win contest a window does through the surface service
    (surface_hover_nominate, core/gui_surface.c) -- occlusion is a tier-1 concern shared
    by every top-level rect, which is why the contest sits below both callers.

    Included by gui.c after gui_layout_child.c (provides layout_push/pop_region, GUI_STATE,
    REGION_PAD_DEFAULT) -- no window/ dependency, like gui_table.c.

==============================================================================================*/
// clang-format off

/* z tiers for root regions: GUI_REGION_Z (default mid-band: over windows, under popups),
   GUI_REGION_BG_Z (ties the window floor -- any raised window wins), GUI_REGION_FG_Z (above
   every popup depth).  Defined with the rest of the z band map in core/gui_surface.c --
   the surface tier authors all z policy. */

/* Persistent scroll + content-measure state, keyed by id -- exactly gui_region_t's scroll link,
   but standalone since a root region has no user_w/user_h (no resize grip). */
static gui_scroll_link_t*
region_root_scroll_get( gui_id_t id )
{
    return GUI_STATE( gui_scroll_link_t, id );
}

bool
gui_region_begin( const char* id_str, f32 x, f32 y, f32 w, f32 h, gui_region_tier_t tier,
                  gui_win_flags_t flags )
{
    gui_id_t            id     = id_hash( id_str );
    gui_scroll_link_t*  scroll = region_root_scroll_get( id );
    DBG_NAME( id, id_str );

    /* Autosize on either axis, exactly like child_begin's h <= 0 (AutoResizeY): hug last
       frame's measured content once one exists, else open one widget-row tall.  No border term
       added -- scroll->content_w/h already folds in the region's own pad on both edges (pad.l+r,
       pad.t+b), which exceeds WIN_BORDER on every theme, so content never reaches the border-inset
       clip layout_push_region computes below.  Adding one on top double-counts that clearance: a
       widget that fills its column with zero slack (next_item_fit(1.0f) or any always-fill widget)
       reproduces outer.w/h exactly at pop, and any nonzero constant added to an exact reproduction
       has nothing left to absorb into -- it drifts outward forever, one border-width per frame. */
    if ( w <= 0.0f ) w = ( scroll->content_w > 0.0f ) ? scroll->content_w : WIDGET_H * 4.0f;
    if ( h <= 0.0f ) h = ( scroll->content_h > 0.0f ) ? scroll->content_h : WIDGET_H;

    gui_rect_t box = { x, y, w, h };

    /* z tier: the caller's three-way choice (gui_region_tier_t) maps onto the band map. */
    u32 z = ( tier == GUI_REGION_BG ) ? GUI_REGION_BG_Z
          : ( tier == GUI_REGION_FG ) ? GUI_REGION_FG_Z
          :                             GUI_REGION_Z;

    /* The pane open (frame/gui_pane.c): stamp the draw state with this region's tag
       (retained-cache key, z tier, the main surface -- a root region paints only on viewport 0;
       FUTURE: other viewports, arena band) and commit the interaction scope so item_state
       attributes this region's widgets against hover_win.  A region IS a pane + scroll layout. */
    pane_tag( id, z, 0, ( flags & GUI_WIN_DEBUG_BAND ) ? 1u : 0u );

    /* Interactive by default -- enter the same hover_win contest a window does, at this region's
       z tier, so its widgets can go hot/active.  Opt out with GUI_WIN_NO_INPUT for a pure HUD. */
    if ( !( flags & GUI_WIN_NO_INPUT ) )
        surface_hover_nominate( id, box, z, 0 );

    /* layout_push_region intersects its own clip against s_scope.clip as "the parent clip" --
       correct for child_begin, genuinely nested inside a window's body clip.  A root region has no
       real parent: s_scope.clip here is just whatever the last unrelated window left behind
       (e.g. a menu bar's thin strip), and intersecting against it silently empties this region's
       hit-test clip, so no widget inside it can ever pass rect_hit(s_scope.clip) -- hover is
       gone regardless of z / hover_win.  Reset to the full display rect first, exactly like a
       window's own plain (non-intersecting) clip assignment in window_begin_ex, so a region is a
       true root-level context. */
    s_scope.clip = ( gui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };

    /* Chrome-equivalent reset: this open is not an item, so a disabled latch left by a prior
       widget does not leak into the region's first widget. */
    item_flags_chrome_reset();

    layout_push_region( id, box, REGION_PAD_DEFAULT, flags, scroll,
                        /* own_clip */ !( flags & GUI_WIN_NO_CLIP ) );
    return true;
}

void
gui_region_end( void )
{
    layout_pop_region();
}
