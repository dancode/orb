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

/* Detach / reattach button (title-bar right edge): a distinct stable per-window widget id. */
#define IMGUI_DETACH_SALT     0xDE7AC405u

/* Native caption buttons (min / max / close / pop-in): one base salt offset by the button kind so
   each gets a distinct, stable per-window widget id (see native_caption_buttons below). */
#define IMGUI_NATIVE_BTN_SALT 0xCA9710B0u

/* IMGUI_RESIZE_SALT, the IMGUI_RESIZE_* edge bits, the WIN_RESIZE_* grab-band constants, and the
   record-agnostic resize helpers (window_resize_hit, window_draw_resize_highlight, resize_grab,
   resize_apply_edges) live in imgui_widget_core.c -- alongside the style macros they need and ahead
   of imgui_layout.c -- so begin_child and a future dock splitter reuse the same mechanism.  Only the
   window's size policy stays below: the min clamp with far-edge pinning (window_apply_resize) and
   the content auto-fit (window_fit_size). */

/*----------------------------------------------------------------------------------------------
    begin_window / end_window
----------------------------------------------------------------------------------------------*/

/* Keep a dragged window reachable: clamp so its top edge stays on-screen and at
   least one title-bar's worth of the window remains within the display bounds.
   Uses the window's own viewport dimensions so dragging on a secondary surface
   clamps against that surface, not the primary.  The top bound is the host's
   native caption band (caption_inset): a window cannot slide its titlebar under
   the OS-owned caption, where the grab would be lost -- mirroring how a child
   stays below a normal OS title bar.  Inset is 0 with no native shell, so the
   default-chrome path keeps the old top-of-surface behavior.
   IMGUI_WIN_NO_BOUNDARY_CLAMP bypasses this entirely. */
static void
window_clamp( imgui_window_t* win )
{
    if ( win->flags & IMGUI_WIN_NO_BOUNDARY_CLAMP )
        return;

    const imgui_viewport_t* vp = &g_ctx->viewports[ win->viewport ];
    f32 dw = vp->disp_w > 0 ? (f32)vp->disp_w : (f32)s_io.display_w;
    f32 dh = vp->disp_h > 0 ? (f32)vp->disp_h : (f32)s_io.display_h;
    const f32 margin = WIN_TITLE_H;
    const f32 top    = vp->caption_inset;
    const f32 max_x  = dw - margin;
    const f32 max_y  = dh - margin;

    if ( win->x > max_x )           win->x = max_x;
    if ( win->y > max_y )           win->y = max_y;
    if ( win->x < margin - win->w ) win->x = margin - win->w;
    if ( win->y < top )             win->y = top;
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

/* Smallest width a window may be shrunk to. */
static f32 window_min_w( void ) { return WIN_TITLE_H * 4.0f; }

/* Smallest height: always keeps the title bar fully visible plus one widget row of body, so
   a resize never eats into the title bar vertically.  title_h is 0 for a NOTITLEBAR window. */
static f32 window_min_h( f32 title_h ) { return title_h + WIDGET_H + WIN_BORDER; }

/*----------------------------------------------------------------------------------------------
    Native-borderless windows (IMGUI_WIN_NATIVE)

    A native window IS its host OS window: its titlebar stands in for the Win32 caption and its
    border for the sizing frame.  imgui does NOT hit-test or forward gestures -- it just publishes
    the frame layout (caption band + resize border) to the app each frame; the OS then drives move,
    resize, Aero Snap, double-click-maximize and the system menu itself through WM_NCHITTEST.  So
    the imgui side only pins geometry, draws chrome, and calls window_set_native_frame.
----------------------------------------------------------------------------------------------*/

/* OS window hosting this window's viewport surface (-1 / APP_WIN_INVALID if unassociated). */
static win_id_t window_native_id( const imgui_window_t* win )
{
    return ( win_id_t )g_ctx->viewports[ win->viewport ].win_id;
}

/* A window is native when it solely occupies an imgui-owned OS window: flagged explicitly (a
   borderless main window) or living on an owned floater (a detached panel -- "detach = native").
   A panel on the main viewport (0, never owned) is not native unless flagged.  begin_window and
   end_window both gate on this, so it must be derived the same way in both. */
static bool window_is_native( const imgui_window_t* win, imgui_win_flags_t flags )
{
    return ( flags & IMGUI_WIN_NATIVE ) != 0
        || ( win && win->viewport != 0 && g_ctx->viewports[ win->viewport ].owned );
}

/*----------------------------------------------------------------------------------------------
    Native caption buttons

    The OS owns a native window's caption band (HTCAPTION), so its titlebar buttons cannot be
    ordinary imgui widgets unless their rects are punched out as HTCLIENT "holes".  This one
    layout function feeds both halves of that: begin_window publishes the rects as holes (via
    window_set_native_frame) so the OS lets clicks through, and end_window draws the glyphs and
    runs widget_behavior on the same rects.  Computing the layout in one place keeps the holes
    exactly aligned with the drawn buttons.

    Buttons are title-bar-height squares laid out right-to-left from the bar's right edge:
    minimize, maximize/restore, then a primary action -- close for the main window, pop-in (merge
    back into the main surface) for a detached floater.  IMGUI_WIN_NO_MINIMIZE / NO_MAXIMIZE drop the
    matching button per-window (the primary is never dropped); the layout closes up around the gap.
----------------------------------------------------------------------------------------------*/

typedef enum
{
    NATIVE_BTN_MINIMIZE = 0,
    NATIVE_BTN_MAXIMIZE,        /* maximize, or restore when already maximized */
    NATIVE_BTN_CLOSE,           /* main window: request graceful close (quit)  */
    NATIVE_BTN_POPIN,           /* floater: merge back into the main surface    */

} native_btn_kind_t;

#define NATIVE_BTN_MAX 3

typedef struct
{
    imgui_rect_t      r;
    native_btn_kind_t kind;

} native_btn_t;

/* Fill `out` with this native window's caption buttons; returns the count (0 when no title bar).
   The primary (rightmost) button is pop-in for a floater (owned viewport) or close for the main
   window, and is always present.  Minimize / maximize are each suppressed by the matching NO_* flag,
   so the set the OS hit-tests against (the holes begin_window publishes) and the set end_window draws
   stay identical -- both call here with the same flags.  out[count-1] is the leftmost button -- its x
   bounds the title text. */
static i32
native_caption_buttons( const imgui_window_t* win, imgui_win_flags_t flags,
                        f32 win_x, f32 win_y, f32 win_w, f32 title_h,
                        native_btn_t out[ NATIVE_BTN_MAX ] )
{
    if ( title_h <= 0.0f )
        return 0;

    bool              floater = win && win->viewport != 0;   /* detached: pop back in, not close */
    native_btn_kind_t primary = floater ? NATIVE_BTN_POPIN : NATIVE_BTN_CLOSE;

    f32 x = win_x + win_w;   /* march leftward from the right edge */
    i32 n = 0;
    x -= title_h; out[ n++ ] = ( native_btn_t ){ { x, win_y, title_h, title_h }, primary };
    if ( !( flags & IMGUI_WIN_NO_MAXIMIZE ) )
    {
        x -= title_h; out[ n++ ] = ( native_btn_t ){ { x, win_y, title_h, title_h }, NATIVE_BTN_MAXIMIZE };
    }
    if ( !( flags & IMGUI_WIN_NO_MINIMIZE ) )
    {
        x -= title_h; out[ n++ ] = ( native_btn_t ){ { x, win_y, title_h, title_h }, NATIVE_BTN_MINIMIZE };
    }
    return n;
}

/* Draw the glyph for one caption button, centered in its square: a minimize bar, a maximize box
   (two offset boxes when already maximized = restore), a close X, or a filled pop-in box. */
static void
native_btn_draw_glyph( native_btn_kind_t kind, imgui_rect_t r, bool maximized, u32 col )
{
    f32 cx = r.x + r.w * 0.5f;
    f32 cy = r.y + r.h * 0.5f;
    f32 s  = floorf( r.h * 0.18f );   /* glyph half-extent */
    f32 t  = WIN_BORDER;

    switch ( kind )
    {
        case NATIVE_BTN_MINIMIZE:
            imgui_draw_line( cx - s, cy, cx + s, cy, t, col );
            break;

        case NATIVE_BTN_MAXIMIZE:
            if ( maximized )
            {
                /* Restore: two overlapping boxes, a back one up-right and a front one down-left. */
                f32 o = floorf( s * 0.5f );
                draw_push_rect_outline( cx - s + o, cy - s - o, 2.0f * s, 2.0f * s, t, 0, col );
                draw_push_rect_outline( cx - s - o, cy - s + o, 2.0f * s, 2.0f * s, t, 0, col );
            }
            else
            {
                draw_push_rect_outline( cx - s, cy - s, 2.0f * s, 2.0f * s, t, 0, col );
            }
            break;

        case NATIVE_BTN_CLOSE:
            imgui_draw_line( cx - s, cy - s, cx + s, cy + s, t, col );
            imgui_draw_line( cx - s, cy + s, cx + s, cy - s, t, col );
            break;

        case NATIVE_BTN_POPIN:
            /* Filled box: a docked target the floating panel snaps back into (mirrors the old
               detach glyph, where a filled box meant "floating -- click to dock back in"). */
            draw_push_rect_filled( cx - s, cy - s, 2.0f * s, 2.0f * s, 0, 0, 1, 1, 0, col );
            break;
    }
}

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

/* Apply the in-flight resize to win's geometry, clamped to the minimum size.  The raw edge-drag is
   the shared resize_apply_edges (origin / pin math); the window then layers its own policy -- the
   min clamp with a moving edge stopping against the pinned far edge. */
static void
window_apply_resize( imgui_window_t* win, f32 title_h )
{
    const f32 min_w = window_min_w();
    const f32 min_h = window_min_h( title_h );

    imgui_rect_t r = { win->x, win->y, win->w, win->h };
    resize_apply_edges( &r, s_resize_edges );
    win->x = r.x;  win->y = r.y;  win->w = r.w;  win->h = r.h;

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

/* resize_grab (the press-time anchor record) and draw_collapse_arrow live in imgui_widget_core.c,
   shared with begin_child and collapsing_header respectively. */

/* window_begin_ex -- the shared body of begin_window, with the window id supplied explicitly and
   the title used only for display + chrome (NULL = no title text).  imgui_begin_window hashes the
   title for its id; the popup layer (imgui_popup.c) passes a salted popup id and its own title (or
   NULL), so a popup reuses the entire window path -- record, geometry, region, clip, scroll,
   auto-resize, chrome -- with nothing duplicated.  The caller is responsible for any overlay
   save/restore needed when this is begun inside another window (popups detach via imgui_overlay_*). */
static bool
window_begin_ex( imgui_id_t id, const char* title, f32 x, f32 y, f32 w, f32 h, imgui_win_flags_t flags )
{
    /* x/y/w/h are the initial geometry; the registry owns position after that. */
    imgui_window_t* win = window_get( id, x, y, w, h );
    win->flags          = flags;

    /* Closed-viewport fallback: if this window's surface was destroyed, revert to primary. */
    if ( win->viewport > 0 && !rhi_handle_valid( g_ctx->viewports[ win->viewport ].vb ) )
        win->viewport = 0;

    /* Next-window channel: apply any queued set_next_window_pos / _size before this frame's drag,
       resize, and autosize act on the geometry, so a ONCE / APPEARING seed becomes the incoming
       state the user then interacts with.  `appearing` is the first begin (last_frame 0) or the
       first begin after a frame of absence -- it renews the one-shot APPEARING permission. */
    bool appearing = ( win->last_frame == 0u ) || ( win->last_frame != s_retained.frame - 1u );
    window_apply_next( win, appearing );
    win->last_frame = s_retained.frame;

    /* NOTITLEBAR removes the bar entirely (title_h 0); content then starts at the top edge.
       Collapsing lives on the title bar, so NOTITLEBAR and NOCOLLAPSE both pin the window
       open -- any stale collapsed state is cleared so it cannot resurface if the flag drops. */
    bool has_titlebar = !( flags & IMGUI_WIN_NOTITLEBAR );
    f32  title_h      = has_titlebar ? WIN_TITLE_H : 0.0f;

    /* A native window IS its OS window: it cannot collapse (the OS window has no such state) and
       its geometry is owned by the OS surface, not imgui -- pin it to the viewport so the imgui
       window always exactly covers its host window.  Border/titlebar gestures route to the OS
       (see the resize and move-grab sections below). */
    bool native = window_is_native( win, flags );

    /* Frame-only shell: an explicitly IMGUI_WIN_NATIVE window stands in for a borderless OS window's
       frame -- it draws the titlebar + border but leaves its body empty and click-through (no
       background fill, hover nominated only over the titlebar, never raised).  That way the windows
       living inside the borderless viewport stay visible and selectable above it.  A detached floater
       is native too (via its owned viewport) but is real content, so it keeps a normal solid body. */
    bool frame_only = ( flags & IMGUI_WIN_NATIVE ) != 0;

    if ( native )
    {
        const imgui_viewport_t* vp = &g_ctx->viewports[ win->viewport ];
        win->x = 0.0f;
        win->y = 0.0f;
        if ( vp->disp_w > 0 ) win->w = ( f32 )vp->disp_w;
        if ( vp->disp_h > 0 ) win->h = ( f32 )vp->disp_h;

        /* Publish the edge-resize grab thickness (the only metric the WndProc still needs; imgui now
           owns the full caption band via HTCLIENT and dispatches move / title / system-menu itself).
           NORESIZE disables edge dragging (border = 0). */
        i32 border = ( flags & IMGUI_WIN_NORESIZE ) ? 0 : ( i32 )WIN_RESIZE_OUTER;
        app()->window_set_native_frame( window_native_id( win ), true, border );

        /* Publish the caption-inset so panels on this surface clamp their title bars below the
           drawn chrome band.  frame_only shells emit before the panels they frame, so the inset is
           live by the time begin_window runs for each panel this frame. */
        i32 caption = ( flags & IMGUI_WIN_NOTITLEBAR ) ? 0 : ( i32 )WIN_TITLE_H;
        g_ctx->viewports[ win->viewport ].caption_inset = ( f32 )caption;
    }

    bool can_collapse = has_titlebar && !( flags & IMGUI_WIN_NOCOLLAPSE ) && !native;
    if ( !can_collapse ) win->collapsed = false;
    bool collapsed = win->collapsed;

    /* ALWAYS_AUTOSIZE owns its own geometry: it cannot be user-resized and shows no scrollbars
       (the body always fits), and its size is recomputed below from the measured content.  The
       body region is opened with NOSCROLL so it never reserves a gutter -- a clean content_w. */
    bool autosize = ( flags & IMGUI_WIN_ALWAYS_AUTOSIZE ) != 0;
    imgui_win_flags_t body_flags = autosize ? ( flags | IMGUI_WIN_NOSCROLL ) : flags;

    /* Apply an in-progress drag: this window holds active_id while the button is down.

       On the main surface the panel slides within it (win->x/y).  On a floater the panel fills the
       surface, so the drag instead moves the whole OS window in SCREEN space to follow the cursor --
       the floater client origin tracks (screen cursor - grab offset), so the grabbed title point
       stays pinned under the cursor as it crosses the desktop.  Screen cursor reads stay valid
       because the origin window keeps OS mouse capture for the whole gesture (see the tear-off in
       imgui_update_platform_windows). */
    if ( s_interaction.active_id == id )
    {
        if ( win->viewport == 0 )
        {
            win->x = s_io.mouse_x - s_drag_off_x;
            win->y = s_io.mouse_y - s_drag_off_y;
            window_clamp( win );
        }
        else
        {
            i32 cx = 0, cy = 0;
            app()->mouse_position_screen( &cx, &cy );
            app()->window_set_pos( g_ctx->viewports[ win->viewport ].win_id,
                                   cx - (i32)s_drag_off_x, cy - (i32)s_drag_off_y );
            win->x = 0.0f;
            win->y = 0.0f;
        }
    }

    /* Seamless tear-off / merge-back gesture (Dear-ImGui style: no release required).  While a
       title-bar drag is live (button still down, this window owns active_id), crossing a surface
       boundary reassigns which surface hosts the window -- and the drag continues uninterrupted in
       the new home.  The request is enqueued for imgui_update_platform_windows, the safe point to
       create/destroy a surface; one request at a time, and NOMOVE windows never tear off.

       Attached (viewport 0): the OS mouse is captured by the main window, so s_io.mouse_x/y stay in
       its client space and read out-of-bounds exactly when the cursor leaves it -- tear off into a
       floater the moment that happens.

       Floating: the floater follows the cursor, so the cursor never leaves IT; instead test the
       SCREEN cursor against the main window's client rect and merge back when it re-enters.  Capture
       remains on the main window throughout, so the screen-cursor read is valid here too. */
    if ( s_interaction.active_id == id && s_io.mouse_down[ 0 ]
         && !( flags & IMGUI_WIN_NOMOVE ) && !( flags & IMGUI_WIN_NO_DETACH ) && !s_vp_request.active )
    {
        bool crossed = false;
        if ( win->viewport == 0 )
        {
            f32 dw = s_io.display_w > 0 ? (f32)s_io.display_w : 1.0f;
            f32 dh = s_io.display_h > 0 ? (f32)s_io.display_h : 1.0f;
            const imgui_viewport_t* hv = &g_ctx->viewports[ 0 ];
            if ( hv->disp_w > 0 ) dw = (f32)hv->disp_w;
            if ( hv->disp_h > 0 ) dh = (f32)hv->disp_h;
            crossed = s_io.mouse_x < 0.0f || s_io.mouse_y < 0.0f
                   || s_io.mouse_x >= dw || s_io.mouse_y >= dh;
        }
        else
        {
            /* Merge back when the screen cursor re-enters the main window's client rect, inset by a
               title-bar margin.  The inset is hysteresis: tear-off fires at the exact main edge, so
               without a dead-band a cursor hovering on the boundary would spawn and destroy a floater
               every frame.  Re-entering only past the inset breaks that oscillation. */
            i32 cx = 0, cy = 0, mx = 0, my = 0;
            app()->mouse_position_screen( &cx, &cy );
            app()->window_get_pos( g_ctx->viewports[ 0 ].win_id, &mx, &my );
            const imgui_viewport_t* mv = &g_ctx->viewports[ 0 ];
            i32 mw = mv->disp_w > 0 ? (i32)mv->disp_w : (i32)s_io.display_w;
            i32 mh = mv->disp_h > 0 ? (i32)mv->disp_h : (i32)s_io.display_h;
            i32 inset = (i32)WIN_TITLE_H;
            crossed = cx >= mx + inset && cy >= my + inset
                   && cx < mx + mw - inset && cy < my + mh - inset;
        }

        if ( crossed )
        {
            s_vp_request.active  = true;
            s_vp_request.by_drag = true;   /* place the floater under the cursor, keep it tracking */
            s_vp_request.win_id  = id;
            s_vp_request.from_vp = win->viewport;
            s_vp_request.title   = title;
        }
    }

    /* Apply an in-progress edge resize (active_id is the resize-salted window id).  Runs after
       the drag apply -- the two are mutually exclusive, only one can own active_id at a time. */
    if ( s_interaction.active_id == id_combine( id, IMGUI_RESIZE_SALT ) )
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
    bool         resizeable = !( flags & IMGUI_WIN_NORESIZE ) && !autosize && !native;
    if ( resizeable && s_interaction.hover_win == id
         && ( s_interaction.active_id == IMGUI_ID_NONE || s_interaction.active_id == resize_id ) )
    {
        resize_hot = window_resize_hit( disp_r, collapsed );
        if ( resize_hot && s_interaction.active_id == IMGUI_ID_NONE && s_io.mouse_pressed[ 0 ] )
            resize_grab( id, ( imgui_rect_t ){ win->x, win->y, win->w, win->h }, resize_hot );
    }
    s_build.win_resize_hot = resize_hot;   /* read by widget_behavior + end_window's highlight */

    /* CAN_AUTOSIZE size-grip: reserve the bottom-right corner ahead of the body's scrollbars the
       same way the edge band reserves the borders.  The grip square overlaps the scroll gutter,
       but the scrollbar runs first (in layout_pop_region), so without this it would claim the
       press and the grip -- drawn and grabbed later in end_window -- would sit dead behind it.
       Suppressing widget hover over the grip rect leaves active_id free for end_window to grab.
       Gated on hover_win and a free/own active_id, mirroring the edge resize above. */
    bool grip_hot = false;
    if ( ( flags & IMGUI_WIN_CAN_AUTOSIZE ) && !collapsed && s_interaction.hover_win == id
         && ( s_interaction.active_id == IMGUI_ID_NONE || s_interaction.active_id == resize_id ) )
    {
        f32          g  = WIDGET_H;   /* grip leg length -- matches end_window's grip rect */
        imgui_rect_t gr = { win->x + win->w - g, win->y + disp_h - g, g, g };
        grip_hot = rect_hit( gr );
    }
    s_build.win_grip_hot = grip_hot;   /* read by widget_behavior to defer the corner to the grip */

    /* Nominate this window as the one under the cursor (front-most by z wins).  A resizeable
       window expands its nominee rect by the outer grab band (horizontally only when collapsed,
       since its height is pinned) so the cursor still counts as "over" it just outside the
       border -- that is what keeps an edge hot as the cursor crosses to the outside.  The
       winner becomes hover_win next frame; that single fact gates all widget hit-testing. */
    f32 ox = resizeable ? WIN_RESIZE_OUTER : 0.0f;
    f32 oy = ( resizeable && !collapsed ) ? WIN_RESIZE_OUTER : 0.0f;
    if ( frame_only )
        /* Frame-only shell: only the titlebar (caption band + its buttons) is interactive; the body
           is click-through so windows inside the viewport keep their own hover and selection. */
        window_nominate_hover( id, ( imgui_rect_t ){ win->x, win->y, win->w, title_h },
                               win->z, win->viewport );
    else
        window_nominate_hover( id, ( imgui_rect_t ){ win->x - ox, win->y - oy,
                                                     win->w + 2.0f * ox, disp_h + 2.0f * oy }, win->z,
                               win->viewport );

    /* All of this window's geometry is stamped with its z so flush can paint
       windows back-to-front regardless of begin_window call order, and with its
       viewport so flush dispatches it to the surface hosting this window.
       cur_viewport is updated first so DBG_RESIZE below captures to the correct per-viewport list. */
    draw_set_sort_key( win->z );
    draw_set_viewport( win->viewport );
    s_build.cur_viewport = win->viewport;   /* update ambient so new windows created after this inherit it */

    /* Debug overlay: show the outer edge-resize grab band (the catch region just outside the
       border), brightened while an edge is armed.  Only meaningful for a resizeable window. */
    if ( resizeable )
        DBG_RESIZE( ( ( imgui_rect_t ){ win->x - ox, win->y - oy,
                                        win->w + 2.0f * ox, disp_h + 2.0f * oy } ), resize_hot );

    /* Clip this window against ITS surface's extent, not the main window's: the window clip pushed
       below intersects the base clip (clip_stack[0]), so seed that base with this viewport's drawable
       size.  Falls back to the main display when the surface size is unset (single-window default).
       end_window restores the main display for subsequent background / low-level draws. */
    {
        const imgui_viewport_t* vp = &g_ctx->viewports[ win->viewport ];
        f32 rw = vp->disp_w > 0 ? (f32)vp->disp_w : (f32)s_io.display_w;
        f32 rh = vp->disp_h > 0 ? (f32)vp->disp_h : (f32)s_io.display_h;
        draw_set_root_clip( rw, rh );
    }

    /* Window chrome (background, titlebar, border) is not an item: clear any disabled latch a prior
       window's trailing widget left, so this window paints opaque and its chrome interacts. */
    item_flags_chrome_reset();

    /* Commit window chrome state for the widgets and end_window.  The layout pen, content
       column, scroll, and scrollbars are all owned by the body region opened below -- the
       window no longer resolves any of that itself; it is just the root region plus chrome. */
    s_build.win_id        = id;
    s_build.win_title     = title;   /* cached for end_window's deferred chrome */
    s_build.win_collapsed = collapsed;
    s_build.win_flags     = flags;   /* end_window reads these for chrome + resize grab */
    s_build.win_title_h   = title_h; /* 0 when NOTITLEBAR */
    s_build.cur_win       = win;     /* collapse write-back target for end_window */
    s_build.win_x         = win->x;
    s_build.win_y         = win->y;
    s_build.win_w         = win->w;
    s_build.win_h         = disp_h;  /* displayed height (title bar only when collapsed) */

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
        s_build.clip_rect = ( imgui_rect_t ){ win->x, win->y, win->w, disp_h };

        /* Window body background.  Skipped for a frame-only shell: its body stays empty so the
           borderless viewport shows the cleared surface (and the windows inside it) through it. */
        if ( !frame_only )
            draw_push_rect_filled( win->x, win->y, win->w, win->h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_WIN_BG );

        /* Menu-bar strip: when WIN_MENUBAR is set, reserve one row below the title bar for
           begin_menu_bar to fill.  Carved off the top of the body here -- before the scroll
           region opens -- so it sits above the scrolling content and never moves.  The rect is
           stashed in s_build for begin_menu_bar; mb_h is 0 (no reservation) otherwise. */
        f32 mb_h = ( flags & IMGUI_WIN_MENUBAR ) ? ( WIDGET_H + WIDGET_GAP ) : 0.0f;
        s_build.menubar_rect = ( imgui_rect_t ){ win->x, win->y + title_h, win->w, mb_h };

        /* Open the body as a scroll region.  Its region id is the window id, so the body
           scrollbar ids stay exactly what the window used before unification.  The region owns
           the pen, content column, wheel, and bars until layout_pop_region in end_window, but
           reuses the window's single clip.  Bias-from-scroll, gutter reservation, and clamping
           all live there now. */
        imgui_rect_t body = { win->x, win->y + title_h + mb_h, win->w, win->h - title_h - mb_h };
        layout_push_region( id, body, REGION_PAD_DEFAULT, body_flags,
                            &win->scroll_x, &win->scroll_y, &win->content_w, &win->content_h,
                            /* own_clip */ false );
    }
    else
    {
        /* Collapsed: no body region opens and no draw clip is pushed, but end_window still
           hit-tests the collapse arrow through s_build.clip_rect.  Left unset it would inherit
           whatever clip the previously drawn window left behind -- which need not cover this
           title bar, so the arrow goes intermittently dead (it only "works" when the stale clip
           happens to contain it).  Point it at the shown title-bar rect so the arrow is always
           hittable; the deferred chrome in end_window draws within these bounds without a clip. */
        s_build.clip_rect = ( imgui_rect_t ){ win->x, win->y, win->w, disp_h };
    }

    /* false tells the caller to skip its body widgets (they would do nothing anyway). */
    return !collapsed;
}

bool
imgui_begin_window( const char* title, imgui_win_flags_t flags )
{
    return window_begin_ex( id_hash( title ), title, 60.0f, 60.0f, 240.0f, 320.0f, flags );
}

void
imgui_end_window( void )
{
    imgui_window_t* win = s_build.cur_win;

    /* Same native test begin_window used (flag or owned floater): a native window's titlebar/border
       is the OS frame, so its collapse arrow, detach button and imgui drag-grab are all suppressed. */
    bool native     = window_is_native( win, s_build.win_flags );
    bool frame_only = ( s_build.win_flags & IMGUI_WIN_NATIVE ) != 0;

    /* Chrome below (scrollbars via layout_pop_region, collapse arrow, border, size grip) is not an
       item.  layout_pop_region resets too, but a collapsed window opens no region and skips it, so
       reset here to cover that case and the deferred chrome either way. */
    item_flags_chrome_reset();

    /* Close the body scroll region (expanded only -- a collapsed window opened none).  This
       measures the content extent, pops the inner content clip, draws the scrollbars, and
       handles the wheel, all via the shared layout engine.  A collapsed window measures
       nothing and keeps its extents from the last expanded frame, so scroll survives collapse.
       Bars are drawn here, before the chrome (so the border frames them) and before the window
       drag-grab below (so a press on a knob claims active_id and the window does not drag). */
    if ( !s_build.win_collapsed )
        layout_pop_region();

    /* Deferred chrome: titlebar, collapse arrow, title text, and border paint last under the
       outer window clip, so they overdraw any content that scrolled beneath them while still
       merging into the one window draw command.  A NOTITLEBAR window (title_h 0) skips the bar
       entirely and keeps only the border. */
    if ( s_build.win_title_h > 0.0f )
    {
        f32 title_h = s_build.win_title_h;
        draw_push_rect_filled( s_build.win_x, s_build.win_y, s_build.win_w, title_h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_TITLE_BG );

        /* Collapse toggle: a triangle in a title-bar-height square at the bar's left edge.  A
           click flips win->collapsed, taking effect next frame like the drag grab.  Claiming
           hover/active here also keeps the title-bar drag grab below from firing on the same
           press.  Omitted (and the title slides left to the padding) when NOCOLLAPSE is set.
           The icon is drawn from this frame's state so it matches the body shown this frame. */
        f32 text_x = s_build.win_x + WIDGET_PAD;
        if ( !( s_build.win_flags & IMGUI_WIN_NOCOLLAPSE ) && !native )
        {
            imgui_rect_t   arrow_r  = { s_build.win_x, s_build.win_y, title_h, title_h };
            imgui_id_t     arrow_id = id_combine( s_build.win_id, IMGUI_COLLAPSE_SALT );
            widget_state_t arrow_st = widget_behavior( arrow_id, arrow_r, WIDGET_KIND_BUTTON );
            if ( arrow_st.clicked )
                win->collapsed = !win->collapsed;
            draw_collapse_arrow( arrow_r, s_build.win_collapsed, arrow_st.hover ? COL_TEXT : COL_TEXT_DIM );
            text_x = s_build.win_x + title_h;   /* title follows the arrow square */

            /* Double-click anywhere on the bar (but not the arrow, which hovers, nor a hot
               resize edge) does the same toggle -- the familiar "double-click titlebar to
               collapse" gesture.  hover_id == NONE excludes the arrow; win_resize_hot excludes
               the edges; the toggle lands next frame like the arrow click and the drag grab. */
            imgui_rect_t bar_r = { s_build.win_x, s_build.win_y, s_build.win_w, title_h };
            if ( s_io.mouse_double[ 0 ] && !s_build.win_resize_hot
                 && s_build.win_id == s_interaction.hover_win && s_interaction.hover_id == IMGUI_ID_NONE
                 && rect_hit( bar_r ) )
                win->collapsed = !win->collapsed;
        }

        /* Detach / reattach button: a square at the title bar's right edge that pops the window out
           into its own OS window (when it sits on the main surface) or back into the main surface
           (when it is floating).  Movable windows only -- NOMOVE (popups, modals, fixed panels)
           never show it.  Mirrors the collapse arrow on the left: claiming hover/active here keeps
           the title-bar drag and double-click-collapse from also firing on the same press. */
        f32 right_limit = s_build.win_x + s_build.win_w - WIDGET_PAD;
        if ( !( s_build.win_flags & IMGUI_WIN_NOMOVE ) && !( s_build.win_flags & IMGUI_WIN_NO_DETACH )
             && !native )
        {
            imgui_rect_t   det_r  = { s_build.win_x + s_build.win_w - title_h, s_build.win_y, title_h, title_h };
            imgui_id_t     det_id = id_combine( s_build.win_id, IMGUI_DETACH_SALT );
            widget_state_t det_st = widget_behavior( det_id, det_r, WIDGET_KIND_BUTTON );
            if ( det_st.clicked && !s_vp_request.active )
            {
                /* Same channel the drag gesture uses: 0 = on the main surface -> tear off,
                   else on a floater -> merge back.  Serviced by update_platform_windows. */
                s_vp_request.active  = true;
                s_vp_request.by_drag = false;   /* button click: keep the panel at its exact screen spot */
                s_vp_request.win_id  = s_build.win_id;
                s_vp_request.from_vp = win ? win->viewport : 0u;
                s_vp_request.title   = s_build.win_title;
            }

            /* Icon: an outlined box when docked (click to pop out), a filled box when floating
               (click to dock back in). */
            bool attached = !win || win->viewport == 0;
            u32  icol     = det_st.hover ? COL_TEXT : COL_TEXT_DIM;
            f32  isz      = title_h * 0.42f;
            f32  ix       = det_r.x + ( det_r.w - isz ) * 0.5f;
            f32  iy       = det_r.y + ( det_r.h - isz ) * 0.5f;
            if ( attached )
                draw_push_rect_outline( ix, iy, isz, isz, 1.0f, 0, icol );
            else
                draw_push_rect_filled( ix, iy, isz, isz, 0.0f, 0.0f, 1.0f, 1.0f, 0, icol );

            right_limit = det_r.x - WIDGET_PAD;   /* keep the title text clear of the button */
        }

        /* Native caption buttons (min / max / close / pop-in): a native window's titlebar IS the OS
           caption, so its collapse arrow, detach button and imgui drag-grab are all suppressed above
           -- instead it gets OS-window controls.  begin_window published these exact rects as
           HTCLIENT holes, so a click here reaches imgui rather than starting an OS move.  The buttons
           run right-to-left from the bar's right edge; the title text stops at the leftmost one. */
        if ( native )
        {
            win_id_t     os   = window_native_id( win );
            bool         zoom = app()->window_state( os ).maximized != 0;
            native_btn_t btns[ NATIVE_BTN_MAX ];
            i32          nb   = native_caption_buttons( win, s_build.win_flags, s_build.win_x, s_build.win_y,
                                                        s_build.win_w, title_h, btns );

            for ( i32 i = 0; i < nb; ++i )
            {
                imgui_rect_t   br  = btns[ i ].r;
                imgui_id_t     bid = id_combine( s_build.win_id,
                                                 IMGUI_NATIVE_BTN_SALT + ( u32 )btns[ i ].kind );
                widget_state_t bs  = widget_behavior( bid, br, WIDGET_KIND_BUTTON );

                /* Hover/press background so the control reads as clickable. */
                if ( bs.hover || bs.active )
                    draw_push_rect_filled( br.x, br.y, br.w, br.h, 0, 0, 1, 1, 0, COL_WIDGET_HOT );

                native_btn_draw_glyph( btns[ i ].kind, br, zoom, bs.hover ? COL_TEXT : COL_TEXT_DIM );

                if ( bs.clicked )
                {
                    switch ( btns[ i ].kind )
                    {
                        case NATIVE_BTN_MINIMIZE: app()->window_minimize( os );        break;
                        case NATIVE_BTN_MAXIMIZE: app()->window_toggle_maximize( os );  break;
                        case NATIVE_BTN_CLOSE:    app()->window_request_close( os );    break;
                        case NATIVE_BTN_POPIN:
                            /* Same merge-back channel the drag / old detach button use; serviced by
                               update_platform_windows.  by_drag false -> dock in place, no jump. */
                            if ( !s_vp_request.active )
                            {
                                s_vp_request.active  = true;
                                s_vp_request.by_drag = false;
                                s_vp_request.win_id  = s_build.win_id;
                                s_vp_request.from_vp = win ? win->viewport : 0u;
                                s_vp_request.title   = s_build.win_title;
                            }
                            break;
                    }
                }
            }

            if ( nb > 0 )
                right_limit = btns[ nb - 1 ].r.x - WIDGET_PAD;   /* leftmost button bounds the title */
        }

        /* Title text, fitted to the room between the arrow square and the detach button (or the
           bar's right edge) so a narrow (shrunk) window ellipsizes the title instead of bleeding
           it under the button / border. */
        draw_text_fit_n( text_x, text_center_y( s_build.win_y, title_h ), COL_TEXT, s_build.win_title,
                         0xFFFFFFFFu, right_limit - text_x );
    }

    /* Border frames the whole window, with or without a title bar. */
    imgui_rect_t win_r = { s_build.win_x, s_build.win_y, s_build.win_w, s_build.win_h };
    draw_push_rect_outline( win_r.x, win_r.y, win_r.w, win_r.h, WIN_BORDER, 0, COL_BORDER );

    /* Debug overlay: trace the window frame; the front-most (hover) window stands out. */
    DBG_WINDOW( win_r, ( s_build.win_id == s_interaction.hover_win ) );

    /* Resize affordance: bold the outline on any hot edge.  While a resize is in flight, the
       grabbed edges stay lit even if the cursor drifts off them; otherwise use win_resize_hot,
       the hover set computed in begin_window (already NORESIZE- and hover_win-gated). */
    {
        u8 hot_edges = ( s_interaction.active_id == id_combine( s_build.win_id, IMGUI_RESIZE_SALT ) )
                     ? s_resize_edges
                     : s_build.win_resize_hot;
        if ( hot_edges )
            window_draw_resize_highlight( win_r, hot_edges );
    }

    /* CAN_AUTOSIZE: a size-grip triangle hugging the bottom-right corner -- both a resize handle
       and an auto-fit button.  Drag it to resize the window (it grabs the same right+bottom edge
       resize the border band uses, so begin_window applies it next frame); double-click it to snap
       the window to this frame's measured content (win->content_* was just written back by
       layout_pop_region).  Only one of the two fires per press, and the double-click never starts a
       drag.  The grip resizes regardless of NORESIZE -- it is the window's own explicit handle. */
    if ( ( s_build.win_flags & IMGUI_WIN_CAN_AUTOSIZE ) && !s_build.win_collapsed && win )
    {
        f32          g         = WIDGET_H;           /* grip leg length */
        imgui_rect_t gr        = { s_build.win_x + s_build.win_w - g, s_build.win_y + s_build.win_h - g, g, g };
        imgui_id_t   resize_id = id_combine( s_build.win_id, IMGUI_RESIZE_SALT );
        bool         resizing  = ( s_interaction.active_id == resize_id );
        bool         hot       = ( s_build.win_id == s_interaction.hover_win ) && rect_hit( gr );

        if ( hot && s_interaction.active_id == IMGUI_ID_NONE )
        {
            if ( s_io.mouse_double[ 0 ] )
            {
                bool collapsible = ( s_build.win_title_h > 0.0f ) && !( s_build.win_flags & IMGUI_WIN_NOCOLLAPSE );
                window_fit_size( s_build.win_title, s_build.win_title_h, collapsible,
                                 win->content_w, win->content_h, &win->w, &win->h );
            }
            else if ( s_io.mouse_pressed[ 0 ] )
            {
                resize_grab( s_build.win_id, ( imgui_rect_t ){ win->x, win->y, win->w, win->h },
                             IMGUI_RESIZE_R | IMGUI_RESIZE_B );
                resizing = true;
            }
        }

        /* Filled right-angle triangle, lit while hovered or actively resizing. */
        draw_push_triangle( gr.x + g, gr.y, gr.x + g, gr.y + g, gr.x, gr.y + g,
                            0, ( hot || resizing ) ? COL_RESIZE_HOT : COL_TEXT_DIM );
    }

    /* Balance the clip push, which begin_window only made for an expanded window. */
    if ( !s_build.win_collapsed )
        draw_pop_clip_rect();

    /* Subsequent draws (low-level API, the next window) revert to the background key and the
       main surface; the next begin_window re-routes them to its own viewport.  Restore the base
       clip to the main display so background / low-level draws are not bounded by this window's
       (possibly larger or smaller) surface. */
    draw_set_sort_key( 0 );
    draw_set_viewport( 0 );
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );

    /* Window move grab.  Decided here, after this window's widgets have run, and pinned off
       entirely by NOMOVE (fixed-position widgets).  This window must be the one under the
       cursor (hover_win) and nothing must already own active_id -- an edge press never reaches
       here, since begin_window grabbed the resize before the widgets ran.

       frame_only (IMGUI_WIN_NATIVE shell): imgui owns the full client surface via HTCLIENT, so
       title-bar clicks land here instead of going to the OS.  Dispatch them: single press starts
       an OS move (Aero Snap follows for free); double-click sends the maximize toggle; right-click
       shows the system menu.  No active_id is set -- the OS modal loop runs in place of imgui drag.

       All other windows (panels on main surface, floaters on owned viewports): imgui drag grab.
       Left button obeys the global drag mode and only fires on empty space (hover_id == NONE, so a
       widget press drives the widget).  Middle button grabs from anywhere -- no widget consumes it.
       Floaters on owned viewports move their whole OS window each frame (see begin_window drag apply). */
    if ( s_build.win_id == s_interaction.hover_win && s_interaction.active_id == IMGUI_ID_NONE
         && !( s_build.win_flags & IMGUI_WIN_NOMOVE ) )
    {
        imgui_rect_t title_r = { s_build.win_x, s_build.win_y, s_build.win_w, s_build.win_title_h };
        win_id_t     os      = window_native_id( win );

        if ( frame_only || ( native && win && win->viewport != 0 ) )
        {
            /* Native title bar (frame-shell or floater): defer dispatch behind a drag threshold
               so a fast double-click never triggers window_start_move or sets active_id on
               click-1 -- either would absorb click-2 before mouse_double can be tested.
               Right-click on the frame-shell opens the system menu (no double-click concern). */
            if ( s_interaction.hover_id == IMGUI_ID_NONE && rect_hit( title_r ) )
            {
                if ( s_io.mouse_double[ 0 ] )
                {
                    s_titlebar_drag_pending = false;
                    app()->window_title_event( os );
                }
                else if ( s_io.mouse_pressed[ 0 ] )
                {
                    s_titlebar_drag_pending = true;
                    s_titlebar_drag_os      = frame_only;
                    s_titlebar_drag_os_id   = os;
                    s_titlebar_drag_imgui   = s_build.win_id;
                    s_titlebar_drag_px      = s_io.mouse_x;
                    s_titlebar_drag_py      = s_io.mouse_y;
                }
            }
            if ( frame_only && s_io.mouse_pressed[ 1 ]
                 && s_interaction.hover_id == IMGUI_ID_NONE && rect_hit( title_r ) )
                app()->window_system_menu( os, ( i32 )s_io.mouse_x, ( i32 )s_io.mouse_y );

            /* Middle button: immediate grab for floaters (middle has no double-click concern). */
            if ( !frame_only && s_io.mouse_pressed[ 2 ] )
            {
                s_interaction.active_id     = s_build.win_id;
                s_interaction.active_button = 2;
                s_drag_off_x = s_io.mouse_x - s_build.win_x;
                s_drag_off_y = s_io.mouse_y - s_build.win_y;
            }
        }
        else
        {
            /* Regular panel: immediate drag grab. */
            bool left_grab = s_io.mouse_pressed[ 0 ] && s_interaction.hover_id == IMGUI_ID_NONE
                          && s_win_drag_mode != IMGUI_WIN_DRAG_NONE
                          && ( s_win_drag_mode == IMGUI_WIN_DRAG_BODY || rect_hit( title_r ) );
            bool mid_grab  = s_io.mouse_pressed[ 2 ];

            if ( left_grab || mid_grab )
            {
                s_interaction.active_id     = s_build.win_id;
                s_interaction.active_button = mid_grab ? 2 : 0;
                s_drag_off_x        = s_io.mouse_x - s_build.win_x;
                s_drag_off_y        = s_io.mouse_y - s_build.win_y;
            }
        }
    }

    /* Native title-bar threshold: advance or commit outside the hover / active_id gate so
       dragging off the title bar does not stall an in-flight drag.  Only acts on the window
       that armed the pending state. */
    if ( s_titlebar_drag_pending && s_titlebar_drag_imgui == s_build.win_id )
    {
        if ( !s_io.mouse_down[ 0 ] )
        {
            s_titlebar_drag_pending = false;   /* button released without dragging -- was a click */
        }
        else
        {
            f32 dx = s_io.mouse_x - s_titlebar_drag_px;
            f32 dy = s_io.mouse_y - s_titlebar_drag_py;
            if ( dx * dx + dy * dy >= TITLEBAR_DRAG_THRESH * TITLEBAR_DRAG_THRESH )
            {
                s_titlebar_drag_pending = false;
                if ( s_titlebar_drag_os )
                {
                    app()->window_start_move( s_titlebar_drag_os_id );
                }
                else
                {
                    /* Floater: commit to imgui drag; begin_window applies via window_set_pos.
                       If the floater is maximized, restore it first -- dragging a maximized OS
                       window while calling window_set_pos leaves it in a bad state and produces
                       a stale-chrome white bar across the titlebar.  Re-anchor the grab offset
                       so the cursor lands at a natural title-bar position on the restored window
                       (the maximized-viewport-local offsets would snap it to screen origin). */
                    if ( app()->window_state( s_titlebar_drag_os_id ).maximized )
                    {
                        app()->window_restore( s_titlebar_drag_os_id );
                        s_drag_off_x = s_build.win_title_h;
                        s_drag_off_y = s_build.win_title_h * 0.5f;
                    }
                    else
                    {
                        s_drag_off_x = s_io.mouse_x - s_build.win_x;
                        s_drag_off_y = s_io.mouse_y - s_build.win_y;
                    }
                    s_interaction.active_id     = s_build.win_id;
                    s_interaction.active_button = 0;
                }
            }
        }
    }
}

// clang-format on
/*============================================================================================*/
