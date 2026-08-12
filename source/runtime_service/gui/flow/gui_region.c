/*==============================================================================================

    runtime_service/gui/flow/gui_region.c -- Root-level region: a caller-positioned rect with
    no window chrome, for HUD-style elements that just need to draw and take input somewhere
    on screen.

    window_begin_ex (gui_window_free.c) and child_begin (gui_layout_child.c) both build on the
    shared scroll-region engine in gui_scroll.c, but neither is minimal: a window carries a
    pool record, drag, and dock/native-surface sync even with every one of those paths flagged
    off; a child_begin box needs an already-active parent frame to carve its rect from.
    gui_region_begin/end is the third, minimal caller -- an explicit rect, persisted scroll +
    content-measure state (autosize on h/w <= 0, exactly like child_begin's AutoResizeY), and
    the same draw-state stamp window_begin_ex applies -- with no pool slot, no drag, no dock,
    no title, no background fill.

    It rents the full gui_region_t (gui_flow.h) from the keyed state pool -- the same record
    child_begin uses. GUI_WIN_CHILD_RESIZE_X/_Y opts a region into the same right/bottom drag
    grip child_begin offers, persisted in user_w/user_h; everything else about the record
    (window-free, no pool slot, no dock) is unchanged by that.

    The rect lives in the target viewport's client space (`vp`; GUI_VP_MAIN = primary,
    GUI_VP_INVALID or a torn-down viewport both resolve to it). It competes for hover_win in
    the same z contest windows and popups use (surface_hover_nominate, core/gui_surface.c), so
    it is interactive by default at the caller's chosen tier (gui_region_tier_t, see gui.h);
    opt out with GUI_WIN_NO_INPUT, the same flag a window honors.

    Included by gui_flow.c after the sub-layout / split files (needs layout_push/pop_region,
    REGION_PAD_DEFAULT, and the resize protocol resize_item/resize_apply_edges pulled in via
    gui_interact.h) -- no dependency of its own.

==============================================================================================*/
// clang-format off

bool
gui_region_begin( const char* id_str, f32 x, f32 y, f32 w, f32 h, gui_region_tier_t tier,
                  i32 vp, gui_win_flags_t flags )
{
    gui_id_t      id = id_hash( id_str );
    gui_region_t* rg = region_get( id );

    DBG_NAME( id, id_str );

    vp = vp_resolve( vp );   /* GUI_VP_INVALID and a torn-down surface both mean the primary */

    /* Land this viewport's DPI bake before any metric below (WIDGET_H, REGION_PAD_DEFAULT)
       reads s_style -- differently-scaled monitors carry different bakes. No-op once landed. */

    gui_dpi_land( vp );

    /* GUI_WIN_CHILD_RESIZE_X/_Y (gui.h) puts a draggable grip on the right / bottom edge. 
       It is the same flag and the same user_w/user_h field that child_begin drags: seeded
       once from the passed w/h (or a default), then owned by the drag from then on.

       Without the flag, the axis autosizes the same way child_begin's h <= 0 does (AutoResizeY):
       hug last frame's measured content if there is any, else open one widget-row tall.

       layout_push_region insets the view from this box by WIN_BORDER (view_w loses it twice,
       left and right; view_h loses it once). On the autosize path that border has to be added
       back here, or the view ends up a border short of the content it was just measured
       against, and region_gutters mistakes that shortfall for overflow and reserves a
       scrollbar gutter nothing needs.

       x and y each choose autosize or resize independently. A caller can autosize one axis
       (w or h <= 0) while dragging the other (its CHILD_RESIZE_X/_Y bit set) in the same
       region -- that is deliberate, not a conflict. */

    bool resize_x = ( flags & GUI_WIN_CHILD_RESIZE_X ) != 0;
    bool resize_y = ( flags & GUI_WIN_CHILD_RESIZE_Y ) != 0;

    if ( resize_x )
    {
        if ( rg->user_w <= 0.0f ) rg->user_w = ( w > 0.0f ) ? w : WIDGET_H * 4.0f;
        w = size_animate( rg->user_w, GUI_ID_NONE, 0.0f );
    }
    else if ( w <= 0.0f )
    {
        // 
        w = ( rg->scroll.content_w > 0.0f ) ? rg->scroll.content_w + 2.0f * WIN_BORDER : WIDGET_H * 4.0f;
    }

    if ( resize_y )
    {
        if ( rg->user_h <= 0.0f ) rg->user_h = ( h > 0.0f ) ? h : WIDGET_H * 8.0f;
        h = size_animate( rg->user_h, GUI_ID_NONE, 0.0f );
    }
    else if ( h <= 0.0f ) 
    {
        h = ( rg->scroll.content_h > 0.0f ) ? rg->scroll.content_h + WIN_BORDER : WIDGET_H;
    }

    gui_rect_t box = { x, y, w, h };

    /* Edge-resize interaction, resolved here -- before pane_tag stamps s_scope.win -- so a press
       on the grip band pre-empts a body widget under it, the same order window_begin_ex and
       child_begin resolve in.  owner_win is this region's own id (it resolves before the scope
       is stamped, the same reason a window passes its own id).  Right/bottom only: the region's
       x,y stays the caller's fixed anchor, only the far edges move.  Gated implicitly by the
       same hover contest as everything else here -- GUI_WIN_NO_INPUT means this region never
       nominates for hover_win, so resize_item's hover gate below always fails and no drag can
       ever start; no separate check needed. */

    u8 resize_hot = 0;
    if ( resize_x || resize_y )
    {
        u8   allow    = (u8)( ( resize_x ? GUI_RESIZE_R : 0u ) | ( resize_y ? GUI_RESIZE_B : 0u ) );
        bool dragging = false;
        resize_hot = resize_item( id, id, box, allow, false, &dragging );

        if ( dragging )
        {
            gui_rect_t rr = box;
            resize_apply_edges( &rr, resize_hot );

            /* CHILD_MIN_W/H (flow/gui_layout_child.c): the same floor a resizeable child won't
               shrink below -- a couple of rows wide, one row plus border tall. */
            if ( resize_hot & GUI_RESIZE_R )
            {
                rg->user_w = ( rr.w < CHILD_MIN_W ) ? CHILD_MIN_W : rr.w;
                box.w = rg->user_w;
            }
            if ( resize_hot & GUI_RESIZE_B )
            {
                rg->user_h = ( rr.h < CHILD_MIN_H ) ? CHILD_MIN_H : rr.h;
                box.h = rg->user_h;
            }
        }
    }

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

    layout_push_region( id, box, REGION_PAD_DEFAULT, flags, &rg->scroll,
                        /* own_clip */ !( flags & GUI_WIN_NO_CLIP ) );

    /* Stamp the resize bookkeeping on the just-pushed frame for region_end's deferred highlight,
       and suppress body-widget hover under a hot/armed edge for the region's duration -- the
       same protocol child_begin runs on the same layout_frame_t fields; region_end restores the
       saved hot. */
    layout_frame_t* f         = lf();
    f->child_resize_edge      = resize_hot;
    f->child_resize_saved_hot = s_scope.resize_hot;
    if ( f->child_resize_edge ) s_scope.resize_hot = f->child_resize_edge;

    return true;
}

void
gui_region_end( void )
{
    /* Capture the box + resize state before layout_pop_region unwinds the frame -- mirrors
       child_end, minus the border: a region paints no chrome of its own, so only the hot/armed
       edge gets a highlight, and only when GUI_WIN_CHILD_RESIZE_X/_Y was set (edges is 0
       otherwise, so this is a no-op for an ordinary, non-resizeable region). */
    layout_frame_t* f     = lf();
    gui_rect_t      box   = f->outer;
    u8              edges = f->child_resize_edge;
    u8              saved = f->child_resize_saved_hot;

    layout_pop_region();

    s_scope.resize_hot = saved;

    if ( edges )
        draw_resize_highlight( box, edges );

    /* Restore the main display's root clip for whatever paints next at root level -- same
       hand-back window_end performs. */
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );
}

// clang-format on
/*============================================================================================*/
