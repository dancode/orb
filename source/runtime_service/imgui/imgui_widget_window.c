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

/* Scrollbar salts (IMGUI_SCROLLBAR_SALT / IMGUI_HSCROLLBAR_SALT) and the scrollbar widget
   itself now live in imgui_layout.c -- the window body is just a region that uses them. */

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

/*----------------------------------------------------------------------------------------------
    Auto-resize

    window_fit_size computes the window geometry that hugs its measured content.  The content
    extent (content_w / content_h) is what the body region measured last frame -- the width
    reached by the widgets and the total stacked height of the pen travel -- so this is purely a
    function of that measurement plus the fixed chrome.

    Height hugs tightly: content_h is the pen travel, independent of the window's own height, so
    sizing to it never feeds back.  Width hugs the *natural* content -- text and bullet runs report
    their glyph width, so a text window shrinks to its longest line -- but a flex widget (button,
    slider, input) fills its cell and reports the full column width, so a window of those keeps the
    width it already has.  That is stable (no oscillation), just not shrink-to-button.

    Used by ALWAYS_AUTOSIZE every frame (from last frame's content) and by the CAN_AUTOSIZE grip on
    a double-click (from this frame's content).  Never narrower than the title bar or the resize
    minimum so the chrome stays legible.
----------------------------------------------------------------------------------------------*/

static void
window_fit_size( const char* title, f32 title_h, bool collapsible,
                 f32 content_w, f32 content_h, f32* out_w, f32* out_h )
{
    /* Width: content + the symmetric left/right region padding.  Height: title bar + the content
       stack + one gap of bottom breathing + the bottom border (the top pad lives inside the body
       region, the trailing gap inside content_h). */
    f32 want_w = content_w + 2.0f * WIDGET_PAD;
    f32 want_h = title_h + content_h + WIDGET_GAP + WIN_BORDER;

    /* Stay wide enough for the title bar: the collapse-arrow lead (or the left pad) + the title
       text + a trailing pad.  Keeps the title from being clipped when the body is narrow. */
    if ( title && title_h > 0.0f )
    {
        f32 lead    = collapsible ? title_h : WIDGET_PAD;
        f32 title_w = lead + font_text_w( title ) + WIDGET_PAD;
        if ( want_w < title_w ) want_w = title_w;
    }

    f32 min_w = window_min_w();
    if ( want_w < min_w ) want_w = min_w;

    *out_w = want_w;
    *out_h = want_h;
}

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
    s_ctx.active_id = id_combine( id, IMGUI_RESIZE_SALT );
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

/* draw_collapse_arrow lives in imgui_widget_core.c (shared with collapsing_header). */

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

    /* ALWAYS_AUTOSIZE owns its own geometry: it cannot be user-resized and shows no scrollbars
       (the body always fits), and its size is recomputed below from the measured content.  The
       body region is opened with NOSCROLL so it never reserves a gutter -- a clean content_w. */
    bool autosize = ( flags & IMGUI_WIN_ALWAYS_AUTOSIZE ) != 0;
    imgui_win_flags_t body_flags = autosize ? ( flags | IMGUI_WIN_NOSCROLL ) : flags;

    /* Apply an in-progress drag: this window holds active_id while the button is down. */
    if ( s_ctx.active_id == id )
    {
        win->x = s_io.mouse_x - s_drag_off_x;
        win->y = s_io.mouse_y - s_drag_off_y;
        window_clamp( win );
    }

    /* Apply an in-progress edge resize (active_id is the resize-salted window id).  Runs after
       the drag apply -- the two are mutually exclusive, only one can own active_id at a time. */
    if ( s_ctx.active_id == id_combine( id, IMGUI_RESIZE_SALT ) )
        window_apply_resize( win, title_h );

    /* ALWAYS_AUTOSIZE: hug the content measured last frame (held in win->content_*).  Skipped while
       collapsed (the title-bar-only height is preserved) and on the very first appearance, before
       any content has been measured -- then the caller's initial w/h stands for one frame. */
    if ( autosize && !collapsed && win->content_h > 0.0f )
        window_fit_size( title, title_h, can_collapse, win->content_w, win->content_h,
                         &win->w, &win->h );

    /* Collapsed windows shrink to just their title bar, freeing the space below; win->h is
       preserved so reopening restores the previous size.  disp_h is the height actually
       shown this frame and drives the hover rect, clip, and border. */
    f32 disp_h = collapsed ? title_h : win->h;

    /* Edge resize, resolved here so it pre-empts the scrollbar and collapse arrow (resolved in
       end_window) underneath: while the cursor sits on a hot edge, win_resize_hot suppresses
       every widget hover in this window, and a press grabs the resize before any widget can.
       Gated on hover_win (last frame's front-most), so only the top window's edges go hot. */
    imgui_rect_t disp_r    = { win->x, win->y, win->w, disp_h };
    imgui_id_t   resize_id = id_combine( id, IMGUI_RESIZE_SALT );
    u8           resize_hot = 0;
    bool         resizeable = !( flags & IMGUI_WIN_NORESIZE ) && !autosize;
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

    /* Commit window chrome state for the widgets and end_window.  The layout pen, content
       column, scroll, and scrollbars are all owned by the body region opened below -- the
       window no longer resolves any of that itself; it is just the root region plus chrome. */
    s_ctx.win_id        = id;
    s_ctx.win_title     = title;   /* cached for end_window's deferred chrome */
    s_ctx.win_collapsed = collapsed;
    s_ctx.win_flags     = flags;   /* end_window reads these for chrome + resize grab */
    s_ctx.win_title_h   = title_h; /* 0 when NOTITLEBAR */
    s_ctx.cur_win       = win;     /* collapse write-back target for end_window */
    s_ctx.win_x         = win->x;
    s_ctx.win_y         = win->y;
    s_ctx.win_w         = win->w;
    s_ctx.win_h         = disp_h;  /* displayed height (title bar only when collapsed) */

    /* A collapsed window emits no body: the caller skips its widgets on the false return, so
       no body region is opened and no clip is pushed (end_window mirrors this on win_collapsed).
       The fixed-size chrome end_window draws is wholly inside the app bounds.  A caller that
       ignores the return and emits widgets anyway draws into the empty root layout frame --
       harmless zero-size rects -- rather than into the window. */
    if ( !collapsed )
    {
        /* One clip rect for the whole window: the background, the scrolled content, and the
           titlebar/border chrome (deferred to end_window) all share it, so the window flushes
           as a single draw command.  Content scrolled under the title bar or border is
           overpainted by the chrome end_window draws last; anything past the outer edge is
           clipped here.  The body region reuses this clip (own_clip false) -- it does not push
           a second one; only a begin_child inside the window adds another. */
        draw_push_clip_rect( win->x, win->y, win->w, disp_h );
        s_ctx.clip_rect = ( imgui_rect_t ){ win->x, win->y, win->w, disp_h };

        /* Window body background. */
        draw_push_rect_filled( win->x, win->y, win->w, win->h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_WIN_BG );

        /* Open the body as a scroll region.  Its region id is the window id, so the body
           scrollbar ids stay exactly what the window used before unification.  The region owns
           the pen, content column, wheel, and bars until layout_pop_region in end_window, but
           reuses the window's single clip.  Bias-from-scroll, gutter reservation, and clamping
           all live there now. */
        imgui_rect_t body = { win->x, win->y + title_h, win->w, win->h - title_h };
        layout_push_region( id, body, REGION_PAD_DEFAULT, body_flags,
                            &win->scroll_x, &win->scroll_y, &win->content_w, &win->content_h,
                            /* own_clip */ false );
    }

    /* false tells the caller to skip its body widgets (they would do nothing anyway). */
    return !collapsed;
}

void
imgui_end_window( void )
{
    imgui_window_t* win = s_ctx.cur_win;

    /* Close the body scroll region (expanded only -- a collapsed window opened none).  This
       measures the content extent, pops the inner content clip, draws the scrollbars, and
       handles the wheel, all via the shared layout engine.  A collapsed window measures
       nothing and keeps its extents from the last expanded frame, so scroll survives collapse.
       Bars are drawn here, before the chrome (so the border frames them) and before the window
       drag-grab below (so a press on a knob claims active_id and the window does not drag). */
    if ( !s_ctx.win_collapsed )
        layout_pop_region();

    /* Deferred chrome: titlebar, collapse arrow, title text, and border paint last under the
       outer window clip, so they overdraw any content that scrolled beneath them while still
       merging into the one window draw command.  A NOTITLEBAR window (title_h 0) skips the bar
       entirely and keeps only the border. */
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
            imgui_id_t     arrow_id = id_combine( s_ctx.win_id, IMGUI_COLLAPSE_SALT );
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
        draw_push_text( text_x, text_center_y( s_ctx.win_y, title_h ), COL_TEXT, s_ctx.win_title );
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
        u8 hot_edges = ( s_ctx.active_id == id_combine( s_ctx.win_id, IMGUI_RESIZE_SALT ) )
                     ? s_resize_edges
                     : s_ctx.win_resize_hot;
        if ( hot_edges )
            window_draw_resize_highlight( win_r, hot_edges );
    }

    /* CAN_AUTOSIZE: a size-grip triangle hugging the bottom-right corner -- both a resize handle
       and an auto-fit button.  Drag it to resize the window (it grabs the same right+bottom edge
       resize the border band uses, so begin_window applies it next frame); double-click it to snap
       the window to this frame's measured content (win->content_* was just written back by
       layout_pop_region).  Only one of the two fires per press, and the double-click never starts a
       drag.  The grip resizes regardless of NORESIZE -- it is the window's own explicit handle. */
    if ( ( s_ctx.win_flags & IMGUI_WIN_CAN_AUTOSIZE ) && !s_ctx.win_collapsed && win )
    {
        f32          g         = WIDGET_H;           /* grip leg length */
        imgui_rect_t gr        = { s_ctx.win_x + s_ctx.win_w - g, s_ctx.win_y + s_ctx.win_h - g, g, g };
        imgui_id_t   resize_id = id_combine( s_ctx.win_id, IMGUI_RESIZE_SALT );
        bool         resizing  = ( s_ctx.active_id == resize_id );
        bool         hot       = ( s_ctx.win_id == s_ctx.hover_win ) && rect_hit( gr );

        if ( hot && s_ctx.active_id == IMGUI_ID_NONE )
        {
            if ( s_io.mouse_double[ 0 ] )
            {
                bool collapsible = ( s_ctx.win_title_h > 0.0f ) && !( s_ctx.win_flags & IMGUI_WIN_NOCOLLAPSE );
                window_fit_size( s_ctx.win_title, s_ctx.win_title_h, collapsible,
                                 win->content_w, win->content_h, &win->w, &win->h );
            }
            else if ( s_io.mouse_pressed[ 0 ] )
            {
                window_resize_grab( win, s_ctx.win_id, IMGUI_RESIZE_R | IMGUI_RESIZE_B );
                resizing = true;
            }
        }

        /* Filled right-angle triangle, lit while hovered or actively resizing. */
        draw_push_triangle( gr.x + g, gr.y, gr.x + g, gr.y + g, gr.x, gr.y + g,
                            0, ( hot || resizing ) ? COL_RESIZE_HOT : COL_TEXT_DIM );
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
