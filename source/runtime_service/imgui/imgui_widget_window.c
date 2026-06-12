/*==============================================================================================

    runtime_service/imgui/imgui_widget_window.c -- The window as a widget (chrome + interaction).

    Everything that presents a window this frame: the title bar, collapse arrow, border,
    edge-resize affordance, the scrollbars, and begin_window / end_window themselves.  The
    persistent window record and the registry live in imgui_window.c; this file only reads
    and mutates that state through window_get and the shared drag/resize state vars -- it
    declares no long-lived state of its own.

    A window is treated as a large compound widget, so this builds on the shared primitives
    in imgui_widget_core.c (widget_behavior, widget_bg_color, the theme + layout macros).

    Included by imgui.c after imgui_widget.c, so the window record (imgui_window.c), the
    shared widget core, and the leaf widgets are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Per-window widget ids -- salted from the window id so a window's chrome controls never
    collide with each other or with a title-hashed widget id inside the same window.
----------------------------------------------------------------------------------------------*/

/* Vertical / horizontal scrollbar tracks: distinct salts so the two never share an id. */
#define IMGUI_SCROLLBAR_SALT  0x5C011B01u
#define IMGUI_HSCROLLBAR_SALT 0x5C011B02u

/* Collapse arrow: a distinct stable per-window widget id. */
#define IMGUI_COLLAPSE_SALT   0xC011A95Eu

/* Edge-resize grab: while a resize is in flight the window owns active_id == (win id ^
   IMGUI_RESIZE_SALT), distinct from its drag (the bare id), scrollbar, and collapse arrow. */
#define IMGUI_RESIZE_SALT     0x5152E001u

/* Edge bits for s_resize_edges -- combined on a corner grab (e.g. R|B). */
#define IMGUI_RESIZE_L  ( 1u << 0 )
#define IMGUI_RESIZE_R  ( 1u << 1 )
#define IMGUI_RESIZE_T  ( 1u << 2 )
#define IMGUI_RESIZE_B  ( 1u << 3 )

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
#define WIN_RESIZE_OUTER  ( WIN_BORDER + 6.0f )     /* and just outside it      */

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

/* Draw one scrollbar track + knob along an axis and fold any knob drag back into *scroll.
   `vertical` picks the axis; `track` is the full track rect, `content`/`view` the measured
   and visible extents along that axis, and `mouse_along` the live cursor coordinate on it.
   The knob length tracks the visible fraction (min-clamped so it stays grabbable) and the
   drag maps the cursor back into scroll, mirroring slider_float.  Shared by both bars. */
static void
window_scrollbar( imgui_id_t id, imgui_rect_t track, bool vertical,
                  f32 content, f32 view, f32 mouse_along, f32* scroll )
{
    f32 max_scroll = content - view;
    if ( max_scroll < 0.0f ) max_scroll = 0.0f;

    f32 track_len = vertical ? track.h : track.w;
    f32 track_org = vertical ? track.y : track.x;

    widget_state_t st = widget_behavior( id, track, WIDGET_KIND_DRAG );

    /* Knob length is the visible fraction of the track, clamped to a grabbable minimum
       and never longer than the track itself (content <= view => full-length knob). */
    f32 knob_len = ( content > 0.0f ) ? track_len * ( view / content ) : track_len;
    f32 min_len  = (f32)s_layout.slider_knob_w;
    if ( knob_len < min_len )   knob_len = min_len;
    if ( knob_len > track_len ) knob_len = track_len;
    f32 travel = track_len - knob_len;

    /* Drag maps the cursor (knob centre) back into the scroll offset. */
    if ( st.active && travel > 0.0f )
    {
        f32 t = ( mouse_along - track_org - knob_len * 0.5f ) / travel;
        t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
        *scroll = t * max_scroll;
    }

    f32 t_cur    = ( max_scroll > 0.0f ) ? *scroll / max_scroll : 0.0f;
    f32 knob_off = track_org + t_cur * travel;

    draw_push_rect_filled( track.x, track.y, track.w, track.h, 0,0,1,1, 0, COL_SLIDER_TRACK );
    if ( vertical )
        draw_push_rect_filled( track.x, knob_off, track.w, knob_len, 0,0,1,1, 0, widget_bg_color( st ) );
    else
        draw_push_rect_filled( knob_off, track.y, knob_len, track.h, 0,0,1,1, 0, widget_bg_color( st ) );
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

    /* Debug overlay: show the outer edge-resize grab band (the catch region just outside the
       border), brightened while an edge is armed.  Only meaningful for a resizeable window. */
    if ( resizeable )
        DBG_RESIZE( ( ( imgui_rect_t ){ win->x - ox, win->y - oy,
                                        win->w + 2.0f * ox, disp_h + 2.0f * oy } ), resize_hot );

    /* All of this window's geometry is stamped with its z so flush can paint
       windows back-to-front regardless of begin_window call order. */
    draw_set_sort_key( win->z );

    /* Scrollbars (expanded only -- a collapsed window shows no content).  content_h/content_w
       were measured last frame (end_window); they drive both the gutter reservation here and
       the bar drawing in end_window.  The layout origin below is biased by -scroll_y/-scroll_x
       so all widgets slide under the clip; the bars themselves are drawn and dragged in
       end_window once geometry is known.

       Policy from the flags (see imgui.h): ALWAYS_* force a static bar on; NOSCROLL hides every
       bar (wheel input still works); otherwise dynamic -- vertical defaults on, horizontal only
       when HSCROLL is requested.  The two bars are mutually dependent (each gutter shrinks the
       cross view, which can tip the other axis into overflow), so resolve in two passes. */
    f32  sb_w = 0.0f, sb_h = 0.0f;   /* reserved gutter sizes (0 = no bar this frame) */
    bool show_v = false, show_h = false;
    if ( !collapsed )
    {
        const f32 scroll_step = WIDGET_H * 3.0f;   /* content advanced per wheel notch (tunable) */
        const f32 knob        = (f32)s_layout.slider_knob_w;

        bool no_bars  = ( flags & IMGUI_WIN_NOSCROLL ) != 0;
        bool v_static = ( flags & IMGUI_WIN_ALWAYS_VSCROLL ) != 0;
        bool h_static = ( flags & IMGUI_WIN_ALWAYS_HSCROLL ) != 0;
        bool v_dyn    = !no_bars && !v_static;                               /* on by default */
        bool h_dyn    = !no_bars && !h_static && ( ( flags & IMGUI_WIN_HSCROLL ) != 0 );

        /* View extents inside the chrome, before reserving any gutter. */
        f32 view_h = win->h - title_h - WIN_BORDER;
        f32 view_w = win->w - 2.0f * WIN_BORDER;

        /* First pass: a forced or already-overflowing bar claims its gutter. */
        show_v = v_static || ( v_dyn && win->content_h > view_h );
        show_h = h_static || ( h_dyn && win->content_w > view_w );
        if ( show_v ) view_w -= knob;
        if ( show_h ) view_h -= knob;

        /* Second pass: a gutter just reserved may have pushed the cross axis into overflow. */
        if ( !show_v && v_dyn && win->content_h > view_h ) { show_v = true; view_w -= knob; }
        if ( !show_h && h_dyn && win->content_w > view_w ) { show_h = true; view_h -= knob; }

        sb_w = show_v ? knob : 0.0f;
        sb_h = show_h ? knob : 0.0f;

        /* Wheel scrolls the hovered window: vertical by default, horizontal with Shift held.
           Disabled by NOMOUSESCROLL, frozen mid-drag (modal), only for the window under cursor. */
        if ( !( flags & IMGUI_WIN_NOMOUSESCROLL )
             && s_ctx.hover_win == id && s_ctx.active_id == IMGUI_ID_NONE
             && s_io.mouse_wheel != 0.0f )
        {
            bool shift = s_io.keys_down[ APP_KEY_LSHIFT ] || s_io.keys_down[ APP_KEY_RSHIFT ];
            if ( shift ) win->scroll_x -= s_io.mouse_wheel * scroll_step;
            else         win->scroll_y -= s_io.mouse_wheel * scroll_step;
        }

        /* Clamp both offsets to their overflow against the gutter-adjusted views. */
        f32 max_y = win->content_h - view_h;  if ( max_y < 0.0f ) max_y = 0.0f;
        f32 max_x = win->content_w - view_w;  if ( max_x < 0.0f ) max_x = 0.0f;
        if ( win->scroll_y < 0.0f )   win->scroll_y = 0.0f;
        if ( win->scroll_y > max_y )  win->scroll_y = max_y;
        if ( win->scroll_x < 0.0f )   win->scroll_x = 0.0f;
        if ( win->scroll_x > max_x )  win->scroll_x = max_x;
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
    s_ctx.win_show_v    = show_v;  /* scrollbar policy resolved above; end_window draws it */
    s_ctx.win_show_h    = show_h;
    s_ctx.win_sb_w      = sb_w;
    s_ctx.win_sb_h      = sb_h;
    s_ctx.content_x     = win->x + WIDGET_PAD - win->scroll_x;     /* biased by horizontal scroll */
    s_ctx.content_w     = win->w - 2.0f * WIDGET_PAD - sb_w;       /* leave room for the gutter */
    s_ctx.content_max_x = s_ctx.content_x;     /* seed extent at the origin -> empty body measures 0 */

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

    /* Start the layout cursor at the content origin, biased by both scroll offsets. */
    s_ctx.cursor_x = win->x + WIDGET_PAD - win->scroll_x;
    s_ctx.cursor_y = win->y + title_h + WIDGET_GAP - win->scroll_y;

    /* false tells the caller to skip its body widgets (they would do nothing anyway). */
    return !collapsed;
}

void
imgui_end_window( void )
{
    imgui_window_t* win = s_ctx.cur_win;

    /* Content + scrollbars are expanded-only.  A collapsed window measures nothing and keeps
       its content extents from the last expanded frame, so scroll state survives a collapse. */
    if ( !s_ctx.win_collapsed )
    {
        /* Content extent = how far the layout pen travelled from the (unscrolled) origin.
           Adding the scroll offset back cancels the bias begin_window applied.  Stored for
           next frame's clamp and gutter decision, and the knob proportions just below. */
        f32 origin_y = s_ctx.win_y + s_ctx.win_title_h + WIDGET_GAP;
        f32 origin_x = s_ctx.win_x + WIDGET_PAD;
        win->content_h = ( s_ctx.cursor_y      + win->scroll_y ) - origin_y;
        win->content_w = ( s_ctx.content_max_x + win->scroll_x ) - origin_x;

        /* Gutter-adjusted views must match begin_window's reservation exactly, so the drawn
           bars line up with the space already stolen from the content this frame. */
        f32 view_h = s_ctx.win_h - s_ctx.win_title_h - WIN_BORDER - s_ctx.win_sb_h;
        f32 view_w = s_ctx.win_w - 2.0f * WIN_BORDER - s_ctx.win_sb_w;

        /* Bars are drawn before the chrome so the border frames them, and before the window
           drag-grab below so a press on a knob claims active_id and the window does not drag.
           Each is inset by the border; the reserved gutters keep them clear of the corner. */
        if ( s_ctx.win_show_v )
        {
            f32 sb_w = s_ctx.win_sb_w;
            imgui_rect_t track = { s_ctx.win_x + s_ctx.win_w - WIN_BORDER - sb_w,
                                   s_ctx.win_y + s_ctx.win_title_h, sb_w, view_h };
            window_scrollbar( s_ctx.win_id ^ IMGUI_SCROLLBAR_SALT, track, true,
                              win->content_h, view_h, s_io.mouse_y, &win->scroll_y );
        }
        if ( s_ctx.win_show_h )
        {
            f32 sb_h = s_ctx.win_sb_h;
            imgui_rect_t track = { s_ctx.win_x + WIN_BORDER,
                                   s_ctx.win_y + s_ctx.win_h - WIN_BORDER - sb_h, view_w, sb_h };
            window_scrollbar( s_ctx.win_id ^ IMGUI_HSCROLLBAR_SALT, track, false,
                              win->content_w, view_w, s_io.mouse_x, &win->scroll_x );
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

            /* Double-click anywhere on the bar (but not the arrow, which hovers, nor a hot
               resize edge) does the same toggle -- the familiar "double-click titlebar to
               collapse" gesture.  hover_id == NONE excludes the arrow; win_resize_hot excludes
               the edges; the toggle lands next frame like the arrow click and the drag grab. */
            imgui_rect_t bar_r = { s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, title_h };
            if ( s_io.mouse_double[ 0 ] && !s_ctx.win_resize_hot
                 && s_ctx.win_id == s_ctx.hover_win && s_ctx.hover_id == IMGUI_ID_NONE
                 && rect_hit( bar_r ) )
                win->collapsed = !win->collapsed;
        }

        /* Title text. */
        draw_push_text( text_x, s_ctx.win_y + ( title_h - font_char_h() ) * 0.5f, COL_TEXT, s_ctx.win_title );
    }

    /* Border frames the whole window, with or without a title bar. */
    imgui_rect_t win_r = { s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, s_ctx.win_h };
    draw_push_rect_outline( win_r.x, win_r.y, win_r.w, win_r.h, WIN_BORDER, 0, COL_BORDER );

    /* Debug overlay: trace the window frame; the front-most (hover) window stands out. */
    DBG_WINDOW( win_r, ( s_ctx.win_id == s_ctx.hover_win ) );

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

    /* Window move grab.  Decided here, after this window's widgets have run, and pinned off
       entirely by NOMOVE (fixed-position widgets).  This window must be the one under the
       cursor (hover_win) and nothing must already own active_id -- an edge press never reaches
       here, since begin_window grabbed the resize before the widgets ran.  Two buttons grab:

       Left button obeys the global drag mode and only on empty window space (hover_id == NONE,
       so a press on a widget drives the widget, not a drag): BODY drags from anywhere empty,
       TITLEBAR only from the bar (a NOTITLEBAR window has title_h 0, so its title_r never hits
       and TITLEBAR mode cannot move it).

       Middle button is a convenience grab: it moves the front window from anywhere over it --
       even atop a widget, since no widget consumes the middle button -- ignoring the drag mode,
       so the window is always easy to pick up and reposition without aiming for the bar. */
    if ( s_ctx.win_id == s_ctx.hover_win && s_ctx.active_id == IMGUI_ID_NONE
         && !( s_ctx.win_flags & IMGUI_WIN_NOMOVE ) )
    {
        imgui_rect_t title_r = { s_ctx.win_x, s_ctx.win_y, s_ctx.win_w, s_ctx.win_title_h };

        bool left_grab = s_io.mouse_pressed[ 0 ] && s_ctx.hover_id == IMGUI_ID_NONE
                      && s_win_drag_mode != IMGUI_WIN_DRAG_NONE
                      && ( s_win_drag_mode == IMGUI_WIN_DRAG_BODY || rect_hit( title_r ) );

        bool mid_grab = s_io.mouse_pressed[ 2 ];

        if ( left_grab || mid_grab )
        {
            s_ctx.active_id     = s_ctx.win_id;
            s_ctx.active_button = mid_grab ? 2 : 0;   /* release tracks the grabbing button */
            s_drag_off_x        = s_io.mouse_x - s_ctx.win_x;
            s_drag_off_y        = s_io.mouse_y - s_ctx.win_y;
        }
    }
}

// clang-format on
/*============================================================================================*/
