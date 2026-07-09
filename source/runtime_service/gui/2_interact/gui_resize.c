/*==============================================================================================

    runtime_service/gui/2_interact/gui_resize.c -- Shared edge-resize geometry.

    The record-agnostic mechanism behind every draggable edge in the UI: the grab-band hit test,
    the press-time anchor record, and the raw cursor-to-edge apply.  Each touches only a rect and
    the cursor, so a window (gui_widget_window.c), a resizeable child_begin (gui_layout_child.c),
    and a floating dock group (gui_dock_float.c) share one resize feel from here (the dock
    splitter is a gutter grab in 4_dock/, not this mechanism).

    Split is mechanism vs policy: this records the anchors and maps the cursor onto the edges; the
    caller layers its own size policy on the result (a window pins + clamps to its min, a child
    clamps to its constraints and persists the size).  The in-flight s_resize_* state lives here
    because both consumers read/write it, so it belongs to neither.  The hot-edge highlight is
    paint policy and lives with the skin (2_present/gui_widget_core.c: draw_resize_highlight);
    the GUI_RESIZE_* edge bits both sides speak are in gui_internal.h.  WIN_BORDER is still read
    here so the grab band straddles the visible border (a hit-test metric, not paint).

    Included by gui.c before the consumers (gui_layout_child.c / gui_widget_window.c);
    WIN_BORDER resolves from the 0_foundation/gui_style.c vocabulary.

==============================================================================================*/
// clang-format off

/* Edge-resize grab: while a resize is in flight the owner holds active_id == (id ^
   GUI_RESIZE_SALT), distinct from a window drag (the bare id), scrollbar, and collapse arrow. */
#define GUI_RESIZE_SALT     0x5152E001u

/* In-flight edge resize.  s_resize_edges names which edges follow the cursor (GUI_RESIZE_* bits).
   s_resize_off keeps the grabbed edge under the cursor without a jump; s_resize_fix pins the
   opposite edge so a left/top drag grows from the far side. */
static u8   s_resize_edges;
static f32  s_resize_off_x, s_resize_off_y;
static f32  s_resize_fix_x, s_resize_fix_y;

/* Grab band straddling the border: a few pixels inside and a few outside. */
#define RESIZE_BAND_INNER  ( 4.0f )                  /* reach inside the border  */
#define RESIZE_BAND_OUTER  ( WIN_BORDER + 6.0f )     /* and just outside it      */

/* Which edges of rect r the cursor is within the grab band of (0 = none).  The band spans
   [edge - OUTER, edge + INNER] on each side, so the cursor catches an edge from just outside
   the border as well as just inside.  Caller gates on hover_win, so no occlusion test here.
   `pin_v` reports horizontal edges only -- a collapsed window (height pinned to the title bar). */
static u8
edge_resize_hit( gui_rect_t r, bool pin_v )
{
    const f32 in  = RESIZE_BAND_INNER;
    const f32 out = RESIZE_BAND_OUTER;
    const f32 mx  = s_io.mouse_x;
    const f32 my  = s_io.mouse_y;

    /* Outside the outer-expanded rect entirely -> no edge. */
    if ( mx < r.x - out || mx > r.x + r.w + out ) return 0;
    if ( my < r.y - out || my > r.y + r.h + out ) return 0;

    u8 e = 0;
    if ( mx <= r.x + in )           e |= GUI_RESIZE_L;
    if ( mx >= r.x + r.w - in )     e |= GUI_RESIZE_R;
    if ( !pin_v )
    {
        if ( my <= r.y + in )       e |= GUI_RESIZE_T;
        if ( my >= r.y + r.h - in ) e |= GUI_RESIZE_B;
    }
    return e;
}

/* Map a set of grabbed edges to the directional hardware cursor that signals which way the border
   moves: a corner is a diagonal (NWSE for TL/BR, NESW for TR/BL), a single L/R edge is horizontal,
   a single T/B edge is vertical.  Shared by every edge-resize consumer (window, child, splitter). */
static app_cursor_t
resize_cursor_for_edges( u8 e )
{
    bool l = ( e & GUI_RESIZE_L ) != 0, r = ( e & GUI_RESIZE_R ) != 0;
    bool t = ( e & GUI_RESIZE_T ) != 0, b = ( e & GUI_RESIZE_B ) != 0;

    bool corner_tl_or_br = ( t && l ) || ( b && r );   /* top-left or bottom-right corner grabbed */
    bool corner_tr_or_bl = ( t && r ) || ( b && l );   /* top-right or bottom-left corner grabbed */

    if ( corner_tl_or_br ) return APP_CURSOR_RESIZE_NWSE;
    if ( corner_tr_or_bl ) return APP_CURSOR_RESIZE_NESW;
    if ( l || r )          return APP_CURSOR_RESIZE_EW;
    return APP_CURSOR_RESIZE_NS;
}

/* Record the grab anchors for an edge-resize of `box`, keyed by `id` (resize-salted into active_id).
   Stores the cursor offset that keeps each grabbed edge under the cursor, and the absolute position
   of the far edges -- pinned when a left / top edge moves (a right / bottom-only drag never reads
   them).  Record-agnostic: a window, a child, or a splitter grabs identically from its rect. */
static void
resize_grab( gui_id_t id, gui_rect_t box, u8 edges )
{
    s_interaction.active_id = id_combine( id, GUI_RESIZE_SALT );
    s_resize_edges  = edges;

    s_resize_off_x = ( edges & GUI_RESIZE_L ) ? ( s_io.mouse_x - box.x )
                   : ( edges & GUI_RESIZE_R ) ? ( s_io.mouse_x - ( box.x + box.w ) )
                   : 0.0f;
    s_resize_off_y = ( edges & GUI_RESIZE_T ) ? ( s_io.mouse_y - box.y )
                   : ( edges & GUI_RESIZE_B ) ? ( s_io.mouse_y - ( box.y + box.h ) )
                   : 0.0f;

    s_resize_fix_x = box.x + box.w;   /* pinned right edge for a left-edge drag  */
    s_resize_fix_y = box.y + box.h;   /* pinned bottom edge for a top-edge drag  */
}

/* Map the in-flight cursor onto rect *r along the grabbed `edges`, using the offsets / pins recorded
   at grab.  A right / bottom edge moves only the size out from the fixed origin; a left / top edge
   shifts the origin and recovers the size against the pinned far edge.  No min / max clamp -- the
   caller layers its own size policy on the result, so the raw geometry is shared and only the
   bounding differs.  `edges` is a parameter (not read from s_resize_edges) so a caller can apply a
   subset -- a child passes only its R / B. */
static void
resize_apply_edges( gui_rect_t* r, u8 edges )
{
    if ( edges & GUI_RESIZE_R ) r->w = ( s_io.mouse_x - s_resize_off_x ) - r->x;
    if ( edges & GUI_RESIZE_L ) { r->x = s_io.mouse_x - s_resize_off_x; r->w = s_resize_fix_x - r->x; }
    if ( edges & GUI_RESIZE_B ) r->h = ( s_io.mouse_y - s_resize_off_y ) - r->y;
    if ( edges & GUI_RESIZE_T ) { r->y = s_io.mouse_y - s_resize_off_y; r->h = s_resize_fix_y - r->y; }
}

/* One frame of the edge-resize item protocol over (id, box): the behavior seam a resizeable
   rect owner calls once per frame, before its body widgets.  While this id's drag is in flight
   it reports the dragged edges (the caller maps them through resize_apply_edges and layers its
   own clamp / persist policy); otherwise it hit-tests the grab band, arms the grab on press,
   and shows the directional cursor.  `owner_win` is the hover-domain gate: the window whose
   hover makes these edges reachable (a child passes its enclosing scope owner, s_scope.win; a
   window passes its own id -- it resolves before the scope is stamped; a floating dock group
   passes its active tab, the group's hover nominee).  `allow` masks the edges this caller
   exposes; `pin_v` drops the vertical pair (a collapsed window).  Returns the edges live this
   frame -- dragged when *dragging, else hot -- and 0 while another widget owns the interaction
   or the owning window is not hovered. */
static u8
resize_item( gui_id_t id, gui_id_t owner_win, gui_rect_t box, u8 allow, bool pin_v, bool* dragging )
{
    gui_id_t resize_id = id_combine( id, GUI_RESIZE_SALT );

    *dragging = false;
    if ( owner_win != s_interaction.hover_win )
        return 0;
    if ( s_interaction.active_id != GUI_ID_NONE && s_interaction.active_id != resize_id )
        return 0;

    if ( s_interaction.active_id == resize_id )
    {
        u8 ce = (u8)( s_resize_edges & allow );
        if ( ce ) set_mouse_cursor( resize_cursor_for_edges( ce ) );
        *dragging = true;
        return ce;
    }

    u8 hot = (u8)( edge_resize_hit( box, pin_v ) & allow );
    if ( hot )
    {
        if ( s_io.mouse_pressed[ 0 ] )
            resize_grab( id, box, hot );
        set_mouse_cursor( resize_cursor_for_edges( hot ) );
    }
    return hot;
}

// clang-format on
/*============================================================================================*/
