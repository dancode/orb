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

    Root-level only for now: paints on viewport 0 (the main surface).  z tier defaults to a fixed
    band above ordinary windows and below the popup band (GUI_WIN_REGION_BG / _FG override it --
    see below), and it competes for hover_win in the same z contest windows and popups use, so it
    is interactive by default (opt out with GUI_WIN_NO_INPUT, same flag a window honors).

    gui_hover_nominate (below) is window_nominate_hover's body, relocated here so it has no
    window/ dependency: it touches only s_interaction / g_ctx / s_io / rect_hit, all in scope by
    this point in the unity build, and both a region and a window need to enter the same global
    contest.  The window tier (compiled after this file) calls it under its original name via a
    thin call at each of its three sites -- see gui_window.c.

    Included by gui.c after gui_layout_child.c (provides layout_push/pop_region, GUI_STATE,
    REGION_PAD_DEFAULT) -- no window/ dependency, like gui_table.c.

==============================================================================================*/
// clang-format off

/* z tiers for root regions.  GUI_REGION_Z is the default mid-band: above every ordinary window
   (whose z comes from z_counter, a small monotonic count nowhere near this) and below the popup
   band (GUI_POPUP_Z_BASE), so a HUD element always draws over normal windows but under a popup /
   tooltip.  GUI_REGION_BG_Z ties the floor windows start/dock at (0), so a raised window (z >= 1)
   always wins the contest over it.  GUI_REGION_FG_Z sits above every realistic popup depth, so it
   wins over an open menu/combo/modal too. */
#define GUI_REGION_Z     0x40000000u
#define GUI_REGION_BG_Z  0x00000000u
#define GUI_REGION_FG_Z  0xF0000000u

/*----------------------------------------------------------------------------------------------
    gui_hover_nominate -- keep the front-most (highest z) candidate the cursor is over; promoted
    to hover_win next frame.  Shared by window_begin (gui_window.c) and gui_region_begin, so a
    region and a window compete for hover_win in one contest keyed purely on z.

    The cursor lives in exactly one OS window/surface at a time (s_io.mouse_viewport, resolved
    from the win_id on mouse events).  A candidate on any other surface cannot be under the
    cursor regardless of where its rect sits in its own surface's coordinate space, so it is
    rejected before the rect test -- the "physical window is a parent hover" rule.
----------------------------------------------------------------------------------------------*/

static void
gui_hover_nominate( gui_id_t id, gui_rect_t r, u32 z, u32 viewport )
{
    /* Deaf context: not listening for input this frame, skip hover nomination. */
    if ( !g_ctx->listening )
        return;

    /* Surface gate first: the cursor must be in the OS window hosting this candidate's viewport. */
    if ( viewport != s_io.mouse_viewport )
        return;

    /* Cheap z test gates the rect_hit; ties keep whichever nominates last this frame. */
    if ( z >= s_interaction.next_hover_win_z && rect_hit( r ) )
    {
        s_interaction.next_hover_win   = id;
        s_interaction.next_hover_win_z = z;
    }
}

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
    if ( w <= 0.0f ) w = ( scroll->content_w > 0.0f ) ? scroll->content_w + 2.0f * WIN_BORDER : WIDGET_H * 4.0f;
    if ( h <= 0.0f ) h = ( scroll->content_h > 0.0f ) ? scroll->content_h + WIN_BORDER : WIDGET_H;

    gui_rect_t box = { x, y, w, h };

    /* z tier: GUI_WIN_REGION_BG / _FG override the default mid-band; mutually exclusive, BG
       wins if both are set. */
    u32 z = GUI_REGION_Z;
    if ( flags & GUI_WIN_REGION_BG )      z = GUI_REGION_BG_Z;
    else if ( flags & GUI_WIN_REGION_FG ) z = GUI_REGION_FG_Z;

    /* Stamp the draw state a window would: a stable id for the retained-cache key, this region's
       z tier, and the main surface -- a root region does not yet route to other viewports. */
    draw_set_window( id );
    draw_set_sort_key( z );
    draw_set_viewport( 0 );

    /* s_build.win_id is the id every widget_behavior call in this region compares against
       hover_win to decide hot/active (gui_widget_core.c) -- draw_set_window alone only stamps the
       retained-cache tag, not this.  A window sets it in window_begin_ex; a region is its own
       root-level context so it must set it too, exactly the same way. */
    s_build.win_id = id;

    /* Interactive by default -- enter the same hover_win contest a window does, at this region's
       z tier, so its widgets can go hot/active.  Opt out with GUI_WIN_NO_INPUT for a pure HUD. */
    if ( !( flags & GUI_WIN_NO_INPUT ) )
        gui_hover_nominate( id, box, z, 0 );

    /* layout_push_region intersects its own clip against s_build.clip_rect as "the parent clip" --
       correct for child_begin, genuinely nested inside a window's body clip.  A root region has no
       real parent: s_build.clip_rect here is just whatever the last unrelated window left behind
       (e.g. a menu bar's thin strip), and intersecting against it silently empties this region's
       hit-test clip, so no widget inside it can ever pass rect_hit(s_build.clip_rect) -- hover is
       gone regardless of z / hover_win.  Reset to the full display rect first, exactly like a
       window's own plain (non-intersecting) clip assignment in window_begin_ex, so a region is a
       true root-level context. */
    s_build.clip_rect = ( gui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };

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
