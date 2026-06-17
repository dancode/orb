/*==============================================================================================

    runtime_service/imgui/imgui_resize.c -- Shared edge-resize geometry.

    The record-agnostic mechanism behind every draggable edge in the UI: the grab-band hit test,
    the hot-edge highlight, the press-time anchor record, and the raw cursor-to-edge apply.  Each
    touches only a rect and the cursor, so a window (imgui_widget_window.c), a resizeable
    begin_child (imgui_layout.c), and a future dock splitter all share one resize feel from one
    place.  The split is mechanism vs policy: this records the anchors and maps the cursor onto the
    edges; the caller layers its own size policy on the result (a window pins + clamps to its min,
    a child clamps to its constraints and persists the size).

    The in-flight s_resize_* state (edges, offsets, far-edge pins) lives in imgui_window.c; the
    style macros (WIN_BORDER, COL_RESIZE_HOT) come from imgui_widget_core.c.

    Included by imgui.c after imgui_widget_core.c (for the macros) and before imgui_layout.c /
    imgui_widget_window.c (the consumers).  Lifted out of imgui_widget_core.c so the resize
    mechanism is one named unit.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Shared edge-resize geometry

    The salt, edge bits, grab-band constants, and the record-agnostic helpers -- hit-test, highlight,
    grab, and the raw edge-drag apply.  They sit here (they need the style macros above, and this file
    is still ahead of imgui_layout.c) so the window chrome (imgui_widget_window.c), a resizeable
    begin_child (imgui_layout.c), and a future dock splitter all share one resize feel from one
    mechanism.  Each touches only a rect and the cursor; the s_resize_* in-flight state lives in
    imgui_window.c (included earlier).

    The split is mechanism vs policy: resize_grab records the anchors, resize_apply_edges maps the
    cursor onto the edges, and the *caller* layers its own size policy on the result -- a window pins
    the far edge and clamps to its min (window_apply_resize), a child clamps to its constraints and
    persists the size (begin_child).  So the geometry is shared while the bounding stays where it
    belongs, and a new consumer reuses the mechanism rather than copying it.
----------------------------------------------------------------------------------------------*/

/* Edge-resize grab: while a resize is in flight the owner holds active_id == (id ^
   IMGUI_RESIZE_SALT), distinct from a window drag (the bare id), scrollbar, and collapse arrow. */
#define IMGUI_RESIZE_SALT     0x5152E001u

/* Edge bits for s_resize_edges -- combined on a corner grab (e.g. R|B). */
#define IMGUI_RESIZE_L  ( 1u << 0 )
#define IMGUI_RESIZE_R  ( 1u << 1 )
#define IMGUI_RESIZE_T  ( 1u << 2 )
#define IMGUI_RESIZE_B  ( 1u << 3 )

/* Grab band straddling the border: a few pixels inside and a few outside. */
#define WIN_RESIZE_INNER  ( 4.0f )                  /* reach inside the border  */
#define WIN_RESIZE_OUTER  ( WIN_BORDER + 6.0f )     /* and just outside it      */

/* Which edges of rect r the cursor is within the grab band of (0 = none).  The band spans
   [edge - OUTER, edge + INNER] on each side, so the cursor catches an edge from just outside
   the border as well as just inside.  Caller gates on hover_win, so no occlusion test here.
   `pin_v` reports horizontal edges only -- a collapsed window (height pinned to the title bar). */
static u8
window_resize_hit( imgui_rect_t r, bool pin_v )
{
    const f32 in  = WIN_RESIZE_INNER;
    const f32 out = WIN_RESIZE_OUTER;
    const f32 mx  = s_io.mouse_x;
    const f32 my  = s_io.mouse_y;

    /* Outside the outer-expanded rect entirely -> no edge. */
    if ( mx < r.x - out || mx > r.x + r.w + out ) return 0;
    if ( my < r.y - out || my > r.y + r.h + out ) return 0;

    u8 e = 0;
    if ( mx <= r.x + in )           e |= IMGUI_RESIZE_L;
    if ( mx >= r.x + r.w - in )     e |= IMGUI_RESIZE_R;
    if ( !pin_v )
    {
        if ( my <= r.y + in )       e |= IMGUI_RESIZE_T;
        if ( my >= r.y + r.h - in ) e |= IMGUI_RESIZE_B;
    }
    return e;
}

/* Paint a bold line over each hot edge of an outline so it is obvious that the border is
   grabbable and which side will move.  Drawn just inside the rect, over the thin border. */
static void
window_draw_resize_highlight( imgui_rect_t r, u8 edges )
{
    const f32 t = WIN_BORDER * 2.0f + 1.0f;   /* bold relative to the 1px frame */

    if ( edges & IMGUI_RESIZE_L ) draw_push_rect_filled( r.x,             r.y,             t,   r.h, 0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & IMGUI_RESIZE_R ) draw_push_rect_filled( r.x + r.w - t,   r.y,             t,   r.h, 0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & IMGUI_RESIZE_T ) draw_push_rect_filled( r.x,             r.y,             r.w, t,   0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & IMGUI_RESIZE_B ) draw_push_rect_filled( r.x,             r.y + r.h - t,   r.w, t,   0,0,1,1, 0, COL_RESIZE_HOT );
}

/* Record the grab anchors for an edge-resize of `box`, keyed by `id` (resize-salted into active_id).
   Stores the cursor offset that keeps each grabbed edge under the cursor, and the absolute position
   of the far edges -- pinned when a left / top edge moves (a right / bottom-only drag never reads
   them).  Record-agnostic: a window, a child, or a splitter grabs identically from its rect. */
static void
resize_grab( imgui_id_t id, imgui_rect_t box, u8 edges )
{
    s_ctx.active_id = id_combine( id, IMGUI_RESIZE_SALT );
    s_resize_edges  = edges;

    s_resize_off_x = ( edges & IMGUI_RESIZE_L ) ? ( s_io.mouse_x - box.x )
                   : ( edges & IMGUI_RESIZE_R ) ? ( s_io.mouse_x - ( box.x + box.w ) )
                   : 0.0f;
    s_resize_off_y = ( edges & IMGUI_RESIZE_T ) ? ( s_io.mouse_y - box.y )
                   : ( edges & IMGUI_RESIZE_B ) ? ( s_io.mouse_y - ( box.y + box.h ) )
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
resize_apply_edges( imgui_rect_t* r, u8 edges )
{
    if ( edges & IMGUI_RESIZE_R ) r->w = ( s_io.mouse_x - s_resize_off_x ) - r->x;
    if ( edges & IMGUI_RESIZE_L ) { r->x = s_io.mouse_x - s_resize_off_x; r->w = s_resize_fix_x - r->x; }
    if ( edges & IMGUI_RESIZE_B ) r->h = ( s_io.mouse_y - s_resize_off_y ) - r->y;
    if ( edges & IMGUI_RESIZE_T ) { r->y = s_io.mouse_y - s_resize_off_y; r->h = s_resize_fix_y - r->y; }
}

// clang-format on
/*============================================================================================*/
