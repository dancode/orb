/*==============================================================================================

    runtime_service/imgui/imgui_widget.c -- Immediate-mode widget library.

    All widgets are positioned by the layout cursor (s_ctx.cursor_x / cursor_y) which
    starts at the content area top-left of the active window and advances downward.
    Every widget advances cursor_y by its height + WIDGET_GAP after drawing.

    Widget interaction uses the classic hover/active/focused state machine:
        hover    : mouse is hovering over the widget
        active : primary mouse button is held with this widget as the target
        focused: this widget owns keyboard input (input_text)

    Included by imgui.c after imgui_ctx.c so s_ctx, s_io, s_draw, and draw helpers
    are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Layout accessors  (read from s_layout, computed by layout_compute() in imgui.c)
----------------------------------------------------------------------------------------------*/

#define WIDGET_H      ( (f32)s_layout.line_size     )
#define WIDGET_GAP    ( (f32)s_layout.widget_gap    )
#define WIDGET_PAD    ( (f32)s_layout.widget_pad    )
#define WIN_TITLE_H   ( (f32)s_layout.win_title_h   )
#define WIN_BORDER    ( (f32)s_layout.win_border    )
#define CHECKBOX_SZ   ( (f32)s_layout.checkbox_sz   )
#define SLIDER_KNOB_W ( (f32)s_layout.slider_knob_w )

/*----------------------------------------------------------------------------------------------
    Color palette (IMGUI_COLOR: byte order R,G,B,A in memory = ABGR u32)
----------------------------------------------------------------------------------------------*/

#define COL_WIN_BG       IMGUI_COLOR( 0x24, 0x24, 0x24, 0xE8 )
#define COL_TITLE_BG     IMGUI_COLOR( 0x10, 0x60, 0xA0, 0xFF )
#define COL_BORDER       IMGUI_COLOR( 0x80, 0x80, 0x80, 0xFF )
#define COL_TEXT         IMGUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF )
#define COL_TEXT_DIM     IMGUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF )
#define COL_WIDGET_BG    IMGUI_COLOR( 0x40, 0x40, 0x40, 0xFF )
#define COL_WIDGET_HOT   IMGUI_COLOR( 0x50, 0x80, 0xB0, 0xFF )
#define COL_WIDGET_ACT   IMGUI_COLOR( 0x30, 0x60, 0x90, 0xFF )
#define COL_WIDGET_FG    IMGUI_COLOR( 0x20, 0x90, 0xD0, 0xFF )
#define COL_CHECK_MARK   IMGUI_COLOR( 0x20, 0xC0, 0x60, 0xFF )
#define COL_SLIDER_TRACK IMGUI_COLOR( 0x30, 0x30, 0x30, 0xFF )
#define COL_RESIZE_HOT   IMGUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF )   /* bold edge line when a resize border is hot */
#define COL_INPUT_BG     IMGUI_COLOR( 0x38, 0x38, 0x38, 0xFF )
#define COL_INPUT_FOCUS  IMGUI_COLOR( 0x20, 0x50, 0x70, 0xFF )
#define COL_CURSOR       IMGUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF )

/* Derive the scrollbar's widget id from the window id by XOR -- a stable per-window id
   that never collides with a title-hashed widget id in the same window. */
#define IMGUI_SCROLLBAR_SALT  0x5C011B01u

/* Same trick for the collapse arrow: a distinct stable per-window widget id. */
#define IMGUI_COLLAPSE_SALT   0xC011A95Eu

/* And for the edge-resize grab: while a resize is in flight the window owns
   active_id == (win id ^ IMGUI_RESIZE_SALT), distinct from its drag (the bare id),
   its scrollbar, and its collapse arrow. */
#define IMGUI_RESIZE_SALT     0x5152E001u

/* Edge bits for s_resize_edges -- combined on a corner grab (e.g. R|B). */
#define IMGUI_RESIZE_L  ( 1u << 0 )
#define IMGUI_RESIZE_R  ( 1u << 1 )
#define IMGUI_RESIZE_T  ( 1u << 2 )
#define IMGUI_RESIZE_B  ( 1u << 3 )

/*----------------------------------------------------------------------------------------------
    Helpers
----------------------------------------------------------------------------------------------*/

static f32 widget_right( void ) { return s_ctx.content_x + s_ctx.content_w; }

/* Begin a new widget row; returns the rect for the full widget. */
static imgui_rect_t
widget_next_rect( f32 h )
{
    imgui_rect_t r = {
        .x = s_ctx.content_x,
        .y = s_ctx.cursor_y,
        .w = s_ctx.content_w,
        .h = h,
    };
    s_ctx.cursor_y += h + WIDGET_GAP;
    return r;
}

/* Interaction class for a widget, selected at the call site.  Only the press-time
   behavior differs between widgets; everything else (hover/active/click) is uniform. */
typedef enum
{
    WIDGET_KIND_BUTTON    = 0,   /* press captures active; reports clicked   */
    WIDGET_KIND_DRAG      = 1,   /* press captures active; held for dragging */
    WIDGET_KIND_FOCUSABLE = 2,   /* press also claims keyboard focus         */

} widget_kind_t;

/* Result of one frame of interaction for a widget.  Every widget drives its
   visuals and value changes from these flags instead of touching s_ctx directly. */
typedef struct
{
    bool hover;       /* cursor is over the widget this frame                  */
    bool active;    /* primary button held with this widget as the target    */
    bool pressed;   /* primary button went down on the widget this frame     */
    bool clicked;   /* press + release completed with the cursor still over  */
    bool focused;   /* widget owns keyboard input (focusable widgets)        */

} widget_state_t;

/* Unified hover/active/focus/click state machine.  Call once per widget with the
   hit rect and the desired interaction kind; the returned flags are all a widget
   needs for drawing and value updates. */

static widget_state_t
widget_behavior( imgui_id_t id, imgui_rect_t r, widget_kind_t kind )
{
    widget_state_t st = { 0 };

    /* Hot only when this widget belongs to the window the cursor is over (hover_win,
       resolved last frame).  Widgets in any other window short-circuit before rect_hit,
       so occluded windows do no hit-testing at all -- occlusion is decided once, at the
       window level, not per widget.

       Modal-while-dragging: once any item owns active_id (a slider, scrollbar, or window
       drag is in flight) every other item is frozen -- only the active item may hover.
       The active item keeps interacting through st.active below, which reads active_id
       directly, so a drag stays live while the cursor sweeps over inert neighbours. */
    bool can_hover = ( s_ctx.active_id == IMGUI_ID_NONE || s_ctx.active_id == id );
    if ( can_hover && s_ctx.win_id == s_ctx.hover_win && !s_ctx.win_resize_hot && rect_hit( r ) )
        s_ctx.hover_id = id;

    /* Press: capture active (and focus for focusable widgets) on button-down. */
    if ( s_ctx.hover_id == id && s_io.mouse_pressed[ 0 ] )
    {
        s_ctx.active_id = id;
        st.pressed      = true;
        if ( kind == WIDGET_KIND_FOCUSABLE )
            s_ctx.focused_id = id;
    }

    st.hover   = ( s_ctx.hover_id == id );
    st.active  = ( s_ctx.active_id == id );
    st.focused = ( s_ctx.focused_id == id );
    st.clicked = s_io.mouse_released[ 0 ] && s_ctx.hover_id == id && s_ctx.active_id == id;

    return st;
}

/* Background color for a pushbutton / knob style widget, from its interaction state. */
static u32
widget_bg_color( widget_state_t st )
{
    if ( st.active ) return COL_WIDGET_ACT;
    if ( st.hover    ) return COL_WIDGET_HOT;
    return COL_WIDGET_BG;
}

/*----------------------------------------------------------------------------------------------
    begin_window / end_window     
----------------------------------------------------------------------------------------------*/

/* Keep a dragged window reachable: clamp so its top edge stays on-screen and at
   least one title-bar's worth of the window remains within the display bounds. */
static void
window_clamp( imgui_window_t* win )
{
    const f32 margin = WIN_TITLE_H;
    const f32 max_x  = (f32)s_io.display_w - margin;
    const f32 max_y  = (f32)s_io.display_h - margin;

    if ( win->x > max_x )          win->x = max_x;
    if ( win->y > max_y )          win->y = max_y;
    if ( win->x < margin - win->w ) win->x = margin - win->w;
    if ( win->y < 0.0f )            win->y = 0.0f;
}

/*----------------------------------------------------------------------------------------------
    Edge resize

    A window may be resized by grabbing a band that straddles any edge or corner -- reaching a
    little inside the border and a little outside it, OS-style, so the grip is easy to land on.
    The hover/grab is resolved up front in begin_window (using last frame's hover_win) so it
    takes priority over the scrollbar and collapse arrow underneath; the resize itself is
    applied at the top of the next begin_window, one frame later, mirroring the title-bar drag.

    Reaching outside the border needs an exception to the normal hover rule: a resizeable
    window nominates an outer-expanded rect for hover_win so the cursor still counts as "over"
    it within the outer band -- otherwise the edge would go cold the instant the cursor crossed
    the border.  The expansion is only the few outer pixels, so occlusion is barely affected.
----------------------------------------------------------------------------------------------*/

/* Grab band straddling the border: a few pixels inside and a few outside. */
#define WIN_RESIZE_INNER  ( 4.0f )                  /* reach inside the border  */
#define WIN_RESIZE_OUTER  ( WIN_BORDER + 3.0f )     /* and just outside it      */

/* Smallest width a window may be shrunk to. */
static f32 window_min_w( void ) { return WIN_TITLE_H * 4.0f; }

/* Smallest height: always keeps the title bar fully visible plus one widget row of body, so
   a resize never eats into the title bar vertically.  title_h is 0 for a NOTITLEBAR window. */
static f32 window_min_h( f32 title_h ) { return title_h + WIDGET_H + WIN_BORDER; }

/* Which edges of rect r the cursor is within the grab band of (0 = none).  The band spans
   [edge - OUTER, edge + INNER] on each side, so the cursor catches an edge from just outside
   the border as well as just inside.  Caller gates on hover_win, so no occlusion test here.
   Collapsed windows report horizontal edges only -- their height is pinned to the title bar. */
static u8
window_resize_hit( imgui_rect_t r, bool collapsed )
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
    if ( !collapsed )
    {
        if ( my <= r.y + in )       e |= IMGUI_RESIZE_T;
        if ( my >= r.y + r.h - in ) e |= IMGUI_RESIZE_B;
    }
    return e;
}

/* Apply the in-flight resize to win's geometry, clamped to the minimum size.  Moving edges
   (left/top) shift the origin while pinning the opposite edge recorded at grab time. */
static void
window_apply_resize( imgui_window_t* win, f32 title_h )
{
    const f32 min_w = window_min_w();
    const f32 min_h = window_min_h( title_h );

    if ( s_resize_edges & IMGUI_RESIZE_R )
        win->w = ( s_io.mouse_x - s_resize_off_x ) - win->x;

    if ( s_resize_edges & IMGUI_RESIZE_L )
    {
        win->x = s_io.mouse_x - s_resize_off_x;
        win->w = s_resize_fix_x - win->x;
    }

    if ( s_resize_edges & IMGUI_RESIZE_B )
        win->h = ( s_io.mouse_y - s_resize_off_y ) - win->y;

    if ( s_resize_edges & IMGUI_RESIZE_T )
    {
        win->y = s_io.mouse_y - s_resize_off_y;
        win->h = s_resize_fix_y - win->y;
    }

    /* Clamp to minimum; a moving edge stops against the pinned far edge. */
    if ( win->w < min_w )
    {
        if ( s_resize_edges & IMGUI_RESIZE_L ) win->x = s_resize_fix_x - min_w;
        win->w = min_w;
    }
    if ( win->h < min_h )
    {
        if ( s_resize_edges & IMGUI_RESIZE_T ) win->y = s_resize_fix_y - min_h;
        win->h = min_h;
    }
}

/* Paint a bold line over each hot edge of the window outline so it is obvious that the border
   is grabbable and which side will move.  Drawn just inside the rect, over the thin border. */
static void
window_draw_resize_highlight( imgui_rect_t r, u8 edges )
{
    const f32 t = WIN_BORDER * 2.0f + 1.0f;   /* bold relative to the 1px frame */

    if ( edges & IMGUI_RESIZE_L ) draw_push_rect_filled( r.x,             r.y,             t,   r.h, 0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & IMGUI_RESIZE_R ) draw_push_rect_filled( r.x + r.w - t,   r.y,             t,   r.h, 0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & IMGUI_RESIZE_T ) draw_push_rect_filled( r.x,             r.y,             r.w, t,   0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & IMGUI_RESIZE_B ) draw_push_rect_filled( r.x,             r.y + r.h - t,   r.w, t,   0,0,1,1, 0, COL_RESIZE_HOT );
}

/* On a press inside the resize band, claim active_id and record the grab anchors: an offset
   that keeps the grabbed edge under the cursor and the absolute position of the pinned edge. */
static void
window_resize_grab( imgui_window_t* win, imgui_id_t id, u8 edges )
{
    s_ctx.active_id = id ^ IMGUI_RESIZE_SALT;
    s_resize_edges  = edges;

    s_resize_off_x = ( edges & IMGUI_RESIZE_L ) ? ( s_io.mouse_x - win->x )
                   : ( edges & IMGUI_RESIZE_R ) ? ( s_io.mouse_x - ( win->x + win->w ) )
                   : 0.0f;
    s_resize_off_y = ( edges & IMGUI_RESIZE_T ) ? ( s_io.mouse_y - win->y )
                   : ( edges & IMGUI_RESIZE_B ) ? ( s_io.mouse_y - ( win->y + win->h ) )
                   : 0.0f;

    s_resize_fix_x = win->x + win->w;   /* pinned right edge for a left-edge drag  */
    s_resize_fix_y = win->y + win->h;   /* pinned bottom edge for a top-edge drag  */
}

/* Collapse toggle glyph: a small triangle centered in a title-bar-height square.  Points
   down when the window is expanded, right when it is collapsed (the title follows it). */
static void
draw_collapse_arrow( imgui_rect_t box, bool collapsed, u32 color )
{
    f32 cx = box.x + box.w * 0.5f;
    f32 cy = box.y + box.h * 0.5f;
    f32 s  = floorf( box.h * 0.22f );   /* triangle half-extent */

    if ( collapsed )
        /* pointing right:  |>  */
        draw_push_triangle( cx - s, cy - s, cx - s, cy + s, cx + s, cy, 0, color );
    else
        /* pointing down:   \/  */
        draw_push_triangle( cx - s, cy - s, cx + s, cy - s, cx, cy + s, 0, color );
}

bool
imgui_begin_window( const char* title, f32 x, f32 y, f32 w, f32 h, imgui_win_flags_t flags )
{
    /* x/y/w/h are the initial geometry; the registry owns position after that. */
    imgui_id_t      id  = id_hash( title );
    imgui_window_t* win = window_get( id, x, y, w, h );
    win->flags          = flags;

    /* NOTITLEBAR removes the bar entirely (title_h 0); content then starts at the top edge.
       Collapsing lives on the title bar, so NOTITLEBAR and NOCOLLAPSE both pin the window
       open -- any stale collapsed state is cleared so it cannot resurface if the flag drops. */
    bool has_titlebar = !( flags & IMGUI_WIN_NOTITLEBAR );
    f32  title_h      = has_titlebar ? WIN_TITLE_H : 0.0f;
    bool can_collapse = has_titlebar && !( flags & IMGUI_WIN_NOCOLLAPSE );
    if ( !can_collapse ) win->collapsed = false;
    bool collapsed = win->collapsed;

    /* Apply an in-progress drag: this window holds active_id while the button is down. */
    if ( s_ctx.active_id == id )
    {
        win->x = s_io.mouse_x - s_drag_off_x;
        win->y = s_io.mouse_y - s_drag_off_y;
        window_clamp( win );
    }

    /* Apply an in-progress edge resize (active_id is the resize-salted window id).  Runs after
       the drag apply -- the two are mutually exclusive, only one can own active_id at a time. */
    if ( s_ctx.active_id == ( id ^ IMGUI_RESIZE_SALT ) )
        window_apply_resize( win, title_h );

    /* Collapsed windows shrink to just their title bar, freeing the space below; win->h is
       preserved so reopening restores the previous size.  disp_h is the height actually
       shown this frame and drives the hover rect, clip, and border. */
    f32 disp_h = collapsed ? title_h : win->h;

    /* Edge resize, resolved here so it pre-empts the scrollbar and collapse arrow (resolved in
       end_window) underneath: while the cursor sits on a hot edge, win_resize_hot suppresses
       every widget hover in this window, and a press grabs the resize before any widget can.
       Gated on hover_win (last frame's front-most), so only the top window's edges go hot. */
    imgui_rect_t disp_r    = { win->x, win->y, win->w, disp_h };
    imgui_id_t   resize_id = id ^ IMGUI_RESIZE_SALT;
    u8           resize_hot = 0;
    bool         resizeable = !( flags & IMGUI_WIN_NORESIZE );
    if ( resizeable && s_ctx.hover_win == id
         && ( s_ctx.active_id == IMGUI_ID_NONE || s_ctx.active_id == resize_id ) )
    {
        resize_hot = window_resize_hit( disp_r, collapsed );
        if ( resize_hot && s_ctx.active_id == IMGUI_ID_NONE && s_io.mouse_pressed[ 0 ] )
            window_resize_grab( win, id, resize_hot );
    }
    s_ctx.win_resize_hot = resize_hot;   /* read by widget_behavior + end_window's highlight */

    /* Nominate this window as the one under the cursor (front-most by z wins).  A resizeable
       window expands its nominee rect by the outer grab band (horizontally only when collapsed,
       since its height is pinned) so the cursor still counts as "over" it just outside the
       border -- that is what keeps an edge hot as the cursor crosses to the outside.  The
       winner becomes hover_win next frame; that single fact gates all widget hit-testing. */
    f32 ox = resizeable ? WIN_RESIZE_OUTER : 0.0f;
    f32 oy = ( resizeable && !collapsed ) ? WIN_RESIZE_OUTER : 0.0f;
    window_nominate_hover( id, ( imgui_rect_t ){ win->x - ox, win->y - oy,
                                                 win->w + 2.0f * ox, disp_h + 2.0f * oy }, win->z );

    /* All of this window's geometry is stamped with its z so flush can paint
       windows back-to-front regardless of begin_window call order. */
    draw_set_sort_key( win->z );

    /* Vertical scroll (expanded only -- a collapsed window shows no content).  content_h was
       measured last frame (end_window); use it to clamp and to decide whether the scrollbar
       gutter steals content width this frame.  The layout origin below is biased by
       -scroll_y, so all widgets slide under the clip; the scrollbar itself is drawn and
       dragged in end_window once geometry is known. */
    f32 sb_w = 0.0f;
    if ( !collapsed )
    {
        const f32 scroll_step = WIDGET_H * 3.0f;   /* content advanced per wheel notch (tunable) */

        f32 view_h     = win->h - title_h - WIN_BORDER;
        f32 max_scroll = win->content_h - view_h;
        if ( max_scroll < 0.0f ) max_scroll = 0.0f;

        /* Wheel scrolls only the hovered window, and never mid-drag (modal). */
        if ( s_ctx.hover_win == id && s_ctx.active_id == IMGUI_ID_NONE && s_io.mouse_wheel != 0.0f )
            win->scroll_y -= s_io.mouse_wheel * scroll_step;

        if ( win->scroll_y < 0.0f )       win->scroll_y = 0.0f;
        if ( win->scroll_y > max_scroll ) win->scroll_y = max_scroll;

        sb_w = ( win->content_h > view_h ) ? (f32)s_layout.slider_knob_w : 0.0f;
    }

    /* Commit resolved geometry for the widgets and end_window. */
    s_ctx.win_id        = id;
    s_ctx.win_title     = title;   /* cached for end_window's deferred chrome */
    s_ctx.win_collapsed = collapsed;
    s_ctx.win_flags     = flags;   /* end_window reads these for chrome + resize grab */
    s_ctx.win_title_h   = title_h; /* 0 when NOTITLEBAR */
    s_ctx.cur_win       = win;     /* scroll write-back target for end_window */
    s_ctx.win_x         = win->x;
    s_ctx.win_y         = win->y;
    s_ctx.win_w         = win->w;
    s_ctx.win_h         = disp_h;  /* displayed height (title bar only when collapsed) */
    s_ctx.content_x     = win->x + WIDGET_PAD;
    s_ctx.content_w     = win->w - 2.0f * WIDGET_PAD - sb_w;   /* leave room for the gutter */

    /* A collapsed window emits no body: the caller is expected to skip its widgets on the
       false return, so there is nothing below the title bar to clip.  The fixed-size chrome
       end_window draws is wholly inside the app bounds, so the collapsed path needs no clip
       rect and pushes none.  (A caller that ignores the return and emits widgets anyway gets
       visibly wrong output -- there is deliberately no guard to mask that.) */
    if ( !collapsed )
    {
        /* One clip rect for the whole window: background, content, and the titlebar/border
           chrome deferred to end_window all share it, so the window flushes as a single draw
           command.  Content scrolled into the titlebar or border region is overpainted by
           the chrome end_window draws last; anything past the outer edge is clipped here. */
        draw_push_clip_rect( win->x, win->y, win->w, disp_h );

        /* Window body background. */
        draw_push_rect_filled( win->x, win->y, win->w, win->h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_WIN_BG );
    }

    /* Start the layout cursor at the content origin, biased up by the scroll offset. */
    s_ctx.cursor_x = win->x + WIDGET_PAD;
    s_ctx.cursor_y = win->y + title_h + WIDGET_GAP - win->scroll_y;

    /* false tells the caller to skip its body widgets (they would do nothing anyway). */
    return !collapsed;
}

void
imgui_end_window( void )
{
    imgui_window_t* win = s_ctx.cur_win;

    /* Content + scrollbar are expanded-only.  A collapsed window measures nothing and keeps
       win->content_h from its last expanded frame, so scroll state survives a collapse. */
    if ( !s_ctx.win_collapsed )
    {
        /* Content extent = how far the layout cursor travelled from the (unscrolled) origin.
           Adding scroll_y back cancels the bias begin_window applied.  Stored for next
           frame's clamp, gutter decision, and the knob proportions just below. */
        f32 view_h   = s_ctx.win_h - s_ctx.win_title_h - WIN_BORDER;
        f32 origin_y = s_ctx.win_y + s_ctx.win_title_h + WIDGET_GAP;
        win->content_h = ( s_ctx.cursor_y + win->scroll_y ) - origin_y;

        /* Scrollbar: only when content overflows the view.  Drawn before the chrome so the
           border can frame it; handled before the window drag-grab below so a press on the
           knob claims active_id first and the window itself does not start dragging. */
        if ( win->content_h > view_h )
        {
            f32 max_scroll = win->content_h - view_h;
            f32 sb_w       = (f32)s_layout.slider_knob_w;
            f32 track_x    = s_ctx.win_x + s_ctx.win_w - WIN_BORDER - sb_w;
            f32 track_y    = s_ctx.win_y + s_ctx.win_title_h;
            imgui_rect_t track_r = { track_x, track_y, sb_w, view_h };

            imgui_id_t     sb_id = s_ctx.win_id ^ IMGUI_SCROLLBAR_SALT;
            widget_state_t st    = widget_behavior( sb_id, track_r, WIDGET_KIND_DRAG );

            /* Knob height tracks the visible fraction; min-clamped so it stays grabbable. */
            f32 knob_h = view_h * ( view_h / win->content_h );
            if ( knob_h < sb_w ) knob_h = sb_w;
            f32 travel = view_h - knob_h;

            /* Drag maps the cursor (knob centre) back into scroll_y, mirroring slider_float. */
            if ( st.active )
            {
                f32 t = ( travel > 0.0f ) ? ( s_io.mouse_y - track_y - knob_h * 0.5f ) / travel : 0.0f;
                t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
                win->scroll_y = t * max_scroll;
            }

            f32 t_cur  = ( max_scroll > 0.0f ) ? win->scroll_y / max_scroll : 0.0f;
            f32 knob_y = track_y + t_cur * travel;

            draw_push_rect_filled( track_x, track_y, sb_w, view_h, 0,0,1,1, 0, COL_SLIDER_TRACK );
            draw_push_rect_filled( track_x, knob_y, sb_w, knob_h, 0,0,1,1, 0, widget_bg_color( st ) );
        }
    }

    /* Deferred chrome: titlebar, collapse arrow, title text, and border paint last under the
       window's single clip rect, so they overdraw any content that scrolled beneath them
       while still merging into the one window draw command.  A NOTITLEBAR window (title_h 0)
       skips the bar entirely and keeps only the border. */
    if ( s_ctx.win_title_h > 0.0f )
    {
        f32 title_h = s_ctx.win_title_h;
        draw_push_rect_filled( s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, title_h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_TITLE_BG );

        /* Collapse toggle: a triangle in a title-bar-height square at the bar's left edge.  A
           click flips win->collapsed, taking effect next frame like the drag grab.  Claiming
           hover/active here also keeps the title-bar drag grab below from firing on the same
           press.  Omitted (and the title slides left to the padding) when NOCOLLAPSE is set.
           The icon is drawn from this frame's state so it matches the body shown this frame. */
        f32 text_x = s_ctx.win_x + WIDGET_PAD;
        if ( !( s_ctx.win_flags & IMGUI_WIN_NOCOLLAPSE ) )
        {
            imgui_rect_t   arrow_r  = { s_ctx.win_x, s_ctx.win_y, title_h, title_h };
            imgui_id_t     arrow_id = s_ctx.win_id ^ IMGUI_COLLAPSE_SALT;
            widget_state_t arrow_st = widget_behavior( arrow_id, arrow_r, WIDGET_KIND_BUTTON );
            if ( arrow_st.clicked )
                win->collapsed = !win->collapsed;
            draw_collapse_arrow( arrow_r, s_ctx.win_collapsed, arrow_st.hover ? COL_TEXT : COL_TEXT_DIM );
            text_x = s_ctx.win_x + title_h;   /* title follows the arrow square */
        }

        /* Title text. */
        draw_push_text( text_x, s_ctx.win_y + ( title_h - font_char_h() ) * 0.5f, COL_TEXT, s_ctx.win_title );
    }

    /* Border frames the whole window, with or without a title bar. */
    imgui_rect_t win_r = { s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, s_ctx.win_h };
    draw_push_rect_outline( win_r.x, win_r.y, win_r.w, win_r.h, WIN_BORDER, 0, COL_BORDER );

    /* Resize affordance: bold the outline on any hot edge.  While a resize is in flight, the
       grabbed edges stay lit even if the cursor drifts off them; otherwise use win_resize_hot,
       the hover set computed in begin_window (already NORESIZE- and hover_win-gated). */
    {
        u8 hot_edges = ( s_ctx.active_id == ( s_ctx.win_id ^ IMGUI_RESIZE_SALT ) )
                     ? s_resize_edges
                     : s_ctx.win_resize_hot;
        if ( hot_edges )
            window_draw_resize_highlight( win_r, hot_edges );
    }

    /* Balance the clip push, which begin_window only made for an expanded window. */
    if ( !s_ctx.win_collapsed )
        draw_pop_clip_rect();

    /* Subsequent draws (low-level API, the next window) revert to the background key. */
    draw_set_sort_key( 0 );

    /* Drag grab.  Decided here, after this window's widgets have run, so hover_id tells us
       whether the press landed on a widget: this window is the one under the cursor (hover_win)
       and no widget of it took the hover (hover_id == NONE) means the press is on empty window
       space.  An edge press never reaches here -- begin_window already grabbed the resize (and
       set active_id) before the widgets ran, so the active_id == NONE test below excludes it.
       BODY drags from anywhere empty; TITLEBAR only from the bar (a NOTITLEBAR window has
       title_h 0, so its title_r never hits and TITLEBAR mode cannot move it). */
    if ( s_ctx.win_id == s_ctx.hover_win && s_ctx.hover_id == IMGUI_ID_NONE
         && s_io.mouse_pressed[ 0 ] && s_ctx.active_id == IMGUI_ID_NONE
         && s_win_drag_mode != IMGUI_WIN_DRAG_NONE )
    {
        imgui_rect_t title_r = { s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, s_ctx.win_title_h };
        if ( s_win_drag_mode == IMGUI_WIN_DRAG_BODY || rect_hit( title_r ) )
        {
            s_ctx.active_id = s_ctx.win_id;
            s_drag_off_x    = s_io.mouse_x - s_ctx.win_x;
            s_drag_off_y    = s_io.mouse_y - s_ctx.win_y;
        }
    }
}

/*----------------------------------------------------------------------------------------------
    text
----------------------------------------------------------------------------------------------*/

void
imgui_text( const char* str )
{
    imgui_rect_t r = widget_next_rect( font_char_h() );
    draw_push_text( r.x, r.y, COL_TEXT, str );
}

/*----------------------------------------------------------------------------------------------
    textf -- printf-style text label (no overloading, so distinct from text())
----------------------------------------------------------------------------------------------*/

void
imgui_textf( const char* fmt, ... )
{
    /* Format into a frame-local buffer; oversized output is truncated, not wrapped. */
    char buf[ 1024 ];

    va_list ap;
    va_start( ap, fmt );
    vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );

    imgui_text( buf );
}

/*----------------------------------------------------------------------------------------------
    button -- returns true on the frame the button is released while hovered
----------------------------------------------------------------------------------------------*/

bool
imgui_button( const char* label )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Background. */
    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );

    /* Centered label. */
    f32 lw = font_text_w( label );
    f32 lx = r.x + ( r.w - lw  ) * 0.5f;
    f32 y_shift = ( r.h - font_char_h() ) * 0.5f;
    f32 ly = r.y + y_shift;
    draw_push_text( lx, ly, COL_TEXT, label );

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    checkbox -- returns true when the value toggles
----------------------------------------------------------------------------------------------*/

bool
imgui_checkbox( const char* label, bool* v )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    widget_state_t st = widget_behavior( id, r, WIDGET_KIND_BUTTON );

    /* Box on the left. */
    f32 bx = r.x;
    f32 by = r.y + ( r.h - CHECKBOX_SZ ) * 0.5f;
    draw_push_rect_filled( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, 0,0,1,1, 0, COL_WIDGET_BG );
    draw_push_rect_outline( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, WIN_BORDER, 0, COL_BORDER );

    if ( *v )
    {
        /* Check mark: simple smaller filled square. */
        f32 pad = (f32)s_layout.checkmark_pad;
        draw_push_rect_filled( bx + pad, by + pad,
                               CHECKBOX_SZ - 2.0f * pad, CHECKBOX_SZ - 2.0f * pad,
                               0,0,1,1, 0, COL_CHECK_MARK );
    }

    /* Label to the right of the box. */
    draw_push_text( bx + CHECKBOX_SZ + WIDGET_PAD, r.y + ( r.h - font_char_h() ) * 0.5f,
                    COL_TEXT, label );

    bool changed = false;
    if ( st.clicked )
    {
        *v    = !( *v );
        changed = true;
    }
    return changed;
}

/*----------------------------------------------------------------------------------------------
    slider_float -- draggable horizontal slider; returns true while dragging
----------------------------------------------------------------------------------------------*/

bool
imgui_slider_float( const char* label, f32* v, f32 lo, f32 hi )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    /* Label to the right of the track; track takes the left portion. */
    f32 label_w = font_text_w( label );
    f32 track_w = r.w - label_w - WIDGET_PAD;
    f32 min_w   = (f32)( s_layout.slider_knob_w * 3u );
    if ( track_w < min_w ) track_w = min_w;

    imgui_rect_t track_r = { r.x, r.y, track_w, r.h };
    widget_state_t st = widget_behavior( id, track_r, WIDGET_KIND_DRAG );

    /* Drag: update value when active. */
    bool changed = false;
    if ( st.active )
    {
        f32 t = ( s_io.mouse_x - track_r.x ) / track_r.w;
        t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
        f32 nv = lo + t * ( hi - lo );
        if ( nv != *v )
        {
            *v     = nv;
            changed = true;
        }
    }

    /* Draw track. */
    draw_push_rect_filled( track_r.x, track_r.y, track_r.w, track_r.h,
                           0,0,1,1, 0, COL_SLIDER_TRACK );
    draw_push_rect_outline( track_r.x, track_r.y, track_r.w, track_r.h,
                            WIN_BORDER, 0, COL_BORDER );

    /* Draw fill bar up to the current value. */
    f32 t_cur    = ( hi > lo ) ? ( ( *v - lo ) / ( hi - lo ) ) : 0.0f;
    f32 fill_w   = t_cur * ( track_r.w - SLIDER_KNOB_W );
    if ( fill_w > 0.0f )
        draw_push_rect_filled( track_r.x, track_r.y + 1.0f, fill_w, track_r.h - 2.0f,
                               0,0,1,1, 0, COL_WIDGET_FG );

    /* Draw knob. */
    f32 knob_x = track_r.x + t_cur * ( track_r.w - SLIDER_KNOB_W );
    draw_push_rect_filled( knob_x, track_r.y, SLIDER_KNOB_W, track_r.h,
                           0,0,1,1, 0, widget_bg_color( st ) );

    /* Label. */
    draw_push_text( track_r.x + track_r.w + WIDGET_PAD,
                    r.y + ( r.h - font_char_h() ) * 0.5f,
                    COL_TEXT, label );

    return changed;
}

/*----------------------------------------------------------------------------------------------
    input_text -- single-line text field; returns true when Enter is pressed
----------------------------------------------------------------------------------------------*/

bool
imgui_input_text( const char* label, char* buf, u32 bufsz )
{
    imgui_id_t   id = id_hash( label );
    imgui_rect_t r  = widget_next_rect( WIDGET_H );

    /* Box takes the left portion; label on the right. */
    f32 label_w = font_text_w( label );
    f32 box_w    = r.w - label_w - WIDGET_PAD;
    f32 min_box  = s_font->char_h * 3.0f;
    if ( box_w < min_box ) box_w = min_box;
    imgui_rect_t box_r = { r.x, r.y, box_w, r.h };

    /* Click focuses this widget (focus claim handled by the behavior helper). */
    widget_state_t st = widget_behavior( id, box_r, WIDGET_KIND_FOCUSABLE );

    bool focused = st.focused;
    bool enter   = false;

    /* Text input when focused. */
    if ( focused )
    {
        /* Printable chars from s_io.text. */
        for ( const char* ch = s_io.text; *ch; ++ch )
        {
            u32 len = 0;
            while ( len < bufsz - 1 && buf[ len ] ) ++len;
            if ( len + 1 < bufsz )
            {
                buf[ len     ] = *ch;
                buf[ len + 1 ] = '\0';
            }
        }

        /* Backspace. */
        if ( s_io.keys_pressed[ APP_KEY_BACKSPACE ] )
        {
            u32 len = 0;
            while ( len < bufsz - 1 && buf[ len ] ) ++len;
            if ( len > 0 )
                buf[ len - 1 ] = '\0';
        }

        /* Enter submits. */
        if ( s_io.keys_pressed[ APP_KEY_ENTER ] )
        {
            s_ctx.focused_id = IMGUI_ID_NONE;
            enter = true;
        }

        /* Escape cancels focus. */
        if ( s_io.keys_pressed[ APP_KEY_ESCAPE ] )
            s_ctx.focused_id = IMGUI_ID_NONE;
    }

    /* Background. */
    draw_push_rect_filled( box_r.x, box_r.y, box_r.w, box_r.h,
                           0,0,1,1, 0,
                           focused ? COL_INPUT_FOCUS : COL_INPUT_BG );
    draw_push_rect_outline( box_r.x, box_r.y, box_r.w, box_r.h,
                            WIN_BORDER, 0,
                            focused ? COL_WIDGET_HOT : COL_BORDER );

    /* Buffer contents. */
    draw_push_text( box_r.x + WIDGET_PAD,
                    box_r.y + ( box_r.h - font_char_h() ) * 0.5f,
                    COL_TEXT, buf );

    /* Blinking cursor (always visible when focused for simplicity). */
    if ( focused )
    {
        f32 tw      = font_text_w( buf );
        f32 cx      = box_r.x + WIDGET_PAD + tw;
        f32 inset   = (f32)s_layout.cursor_inset;
        f32 cur_w   = (f32)s_layout.cursor_w;
        if ( cx + cur_w < box_r.x + box_r.w )
            draw_push_rect_filled( cx, box_r.y + inset,
                                   cur_w, box_r.h - inset * 2.0f,
                                   0,0,1,1, 0, COL_CURSOR );
    }

    /* Label. */
    draw_push_text( box_r.x + box_r.w + WIDGET_PAD,
                    r.y + ( r.h - font_char_h() ) * 0.5f,
                    COL_TEXT_DIM, label );

    return enter;
}

/*----------------------------------------------------------------------------------------------
    Low-level draw_rect / draw_text
----------------------------------------------------------------------------------------------*/

void
imgui_draw_rect( f32 x, f32 y, f32 w, f32 h, u32 abgr )
{
    draw_push_rect_filled( x, y, w, h, 0,0,1,1, 0, abgr );
}

void
imgui_draw_text( f32 x, f32 y, u32 abgr, const char* str )
{
    draw_push_text( x, y, abgr, str );
}

// clang-format on
/*============================================================================================*/
