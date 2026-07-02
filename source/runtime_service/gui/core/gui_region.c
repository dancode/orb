/*==============================================================================================

    runtime_service/gui/core/gui_region.c -- Root-level region: a fixed-rect layout primitive
    with no window chrome.

    window_begin_ex (window/gui_widget_window.c) and child_begin (gui_layout_child.c) are both
    callers of the shared scroll-region engine in gui_layout_region.c, each layered with its own
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

    Root-level only for now: paints on viewport 0 (the main surface) at a fixed z band above
    ordinary windows and below the popup band, and is NO_INPUT only -- window_nominate_hover
    lives in the window tier (compiled after this file) and a root region has no z-order policy
    to arbitrate against windows yet.  Extend when an interactive root region is needed.

    Included by gui.c after gui_layout_child.c (provides layout_push/pop_region, GUI_STATE,
    REGION_PAD_DEFAULT) -- no window/ dependency, like gui_table.c.

==============================================================================================*/
// clang-format off

/* Paint band for root regions: above every ordinary window (whose z comes from z_counter, a
   small monotonic count nowhere near this) and below the popup band (GUI_POPUP_Z_BASE), so a
   HUD element always draws over normal windows but under a popup / tooltip. */
#define GUI_REGION_Z   0x40000000u

/* Persistent scroll + content-measure state, keyed by id -- exactly gui_region_t's scroll link,
   but standalone since a root region has no user_w/user_h (no resize grip). */
static gui_scroll_link_t*
region_root_scroll_get( gui_id_t id )
{
    return GUI_STATE( gui_scroll_link_t, id );
}

bool
gui_region_begin( const char* id_str, f32 x, f32 y, f32 w, f32 h, gui_win_flags_t flags )
{
    gui_id_t            id     = id_hash( id_str );
    gui_scroll_link_t*  scroll = region_root_scroll_get( id );

    /* Autosize on either axis, exactly like child_begin's h <= 0 (AutoResizeY): hug last
       frame's measured content once one exists, else open one widget-row tall. */
    if ( w <= 0.0f ) w = ( scroll->content_w > 0.0f ) ? scroll->content_w + WIDGET_GAP : WIDGET_H * 4.0f;
    if ( h <= 0.0f ) h = ( scroll->content_h > 0.0f ) ? scroll->content_h + WIDGET_GAP : WIDGET_H;

    gui_rect_t box = { x, y, w, h };

    /* Stamp the draw state a window would: a stable id for the retained-cache key, a fixed HUD
       z band, and the main surface -- a root region does not yet route to other viewports. */
    draw_set_window( id );
    draw_set_sort_key( GUI_REGION_Z );
    draw_set_viewport( 0 );

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
