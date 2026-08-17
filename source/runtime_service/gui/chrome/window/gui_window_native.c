/*==============================================================================================

    runtime_service/gui/chrome/window/gui_window_native.c -- Native-borderless windows (GUI_WIN_NATIVE).

    A native window IS its host OS window: its titlebar stands in for the Win32 caption and its
    border for the sizing frame.  gui does NOT hit-test or forward gestures -- it just publishes
    the frame layout (caption band + resize border) to the app each frame; the OS then drives
    move, resize, Aero Snap, double-click-maximize and the system menu itself through
    WM_NCHITTEST.  So the gui side only pins geometry, draws chrome, and calls
    window_set_native_frame.  This file is everything of that story:

        window_is_native / window_native_id  -- the identity tests both begin and end gate on
        native_caption_buttons               -- the one layout for holes AND drawn buttons
        window_sync_native                   -- geometry pin + frame publish + restore tracking
        native_popin_request                 -- button-triggered tear-off / merge-back request
        native_caption_chrome                -- window_end's caption strip (draw + interact)

    Included by gui_chrome.c after gui_window.c (gesture policy state) and before
    gui_window_free.c (window_begin_ex / window_end, which call everything here).

==============================================================================================*/
// clang-format off

/* Native caption buttons (min / max / close / pop-in): one base salt offset by the button kind so
   each gets a distinct, stable per-window widget id (see native_caption_buttons below). */
#define GUI_NATIVE_BTN_SALT 0xCA9710B0u

/* OS window hosting this window's viewport surface (-1 / APP_WIN_INVALID if unassociated). */
static win_id_t window_native_id( const gui_window_t* win )
{
    return ( win_id_t )s_vp_pool[ win->viewport ].win_id;
}

/* A window is native when it solely occupies an gui-owned OS window: flagged explicitly (a
   borderless main window) or living on an owned floater (a detached panel -- "detach = native").
   A panel on the main viewport (0, never owned) is not native unless flagged.  window_begin and
   window_end both gate on this, so it must be derived the same way in both. */
static bool window_is_native( const gui_window_t* win, gui_win_flags_t flags )
{
    /* A popup / tooltip overlay is never the OS frame, even when it inherits an owned floater's
       viewport: it is an anchored, auto-sized overlay on that surface, not the surface itself.
       win->overlay is stamped before window_begin_ex runs, so the test is live here -- without
       it a menu opened from a detached floater would be pinned to (0,0) at full surface size by
       the native branch below instead of dropping under its button. */
    if ( win && win->overlay )
        return false;

    return ( flags & GUI_WIN_NATIVE ) != 0
        || ( win && win->viewport != 0 && s_vp_pool[ win->viewport ].owned );
}

/*==============================================================================================
    Native caption buttons

    The OS owns a native window's caption band (HTCAPTION), so its titlebar buttons cannot be
    ordinary gui widgets unless their rects are punched out as HTCLIENT "holes".  This one
    layout function feeds both halves of that: window_begin publishes the rects as holes (via
    window_set_native_frame) so the OS lets clicks through, and window_end draws the glyphs and
    runs item_state on the same rects.  Computing the layout in one place keeps the holes
    exactly aligned with the drawn buttons.

    Buttons are title-bar-height squares laid out right-to-left from the bar's right edge:
    close, maximize/restore, minimize, then pop-in (merge back into the main surface) innermost
    for a detached floater.  That is the SAME slot order the docked window paints in window_end
    (close, max, min, detach), so tearing a window off or merging it back never makes a button
    hop across its neighbours -- detach and pop-in are one control that stays put through the
    transition.  GUI_WIN_NO_MINIMIZE / NO_MAXIMIZE drop the matching button per-window (close on
    the main window and a floater's pop-in are never dropped); the layout closes up around the gap.
==============================================================================================*/

typedef enum
{
    NATIVE_BTN_MINIMIZE = 0,
    NATIVE_BTN_MAXIMIZE,        /* maximize, or restore when already maximized */
    NATIVE_BTN_CLOSE,           /* main window: request graceful close (quit)  */
    NATIVE_BTN_POPIN,           /* floater: merge back into the main surface    */

} native_btn_kind_t;

#define NATIVE_BTN_MAX 4

typedef struct
{
    gui_rect_t      r;
    native_btn_kind_t kind;

} native_btn_t;

/* Fill `out` with this native window's caption buttons; returns the count (0 when no title bar).
   Close is outermost (a floater's only when CLOSEABLE; the main window always has it), then
   maximize and minimize -- each suppressed by the matching NO_* flag -- then a floater's pop-in
   innermost, holding the same slot the docked detach button holds.  The set the OS hit-tests
   against (the holes window_begin publishes) and the set window_end draws stay identical -- both
   call here with the same flags.  out[count-1] is the leftmost button -- its x bounds the title
   text. */
static i32
native_caption_buttons( const gui_window_t* win, gui_win_flags_t flags,
                        f32 win_x, f32 win_y, f32 win_w, f32 title_h,
                        native_btn_t out[ NATIVE_BTN_MAX ] )
{
    if ( title_h <= 0.0f )
        return 0;

    bool floater = win && win->viewport != 0;   /* detached: pops back in, and may also close */

    f32 x = win_x + win_w;   /* march leftward from the right edge */
    i32 n = 0;

    /* Close (X), outermost.  The main window's close IS its graceful quit, so it is unconditional
       there; a floater only offers one when CLOSEABLE -- closing hides the window (and frees its
       OS surface) where pop-in merges it back into the main surface. */
    if ( !floater || ( flags & GUI_WIN_CLOSEABLE ) )
    {
        x -= title_h; out[ n++ ] = ( native_btn_t ){ { x, win_y, title_h, title_h }, NATIVE_BTN_CLOSE };
    }
    if ( !( flags & GUI_WIN_NO_MAXIMIZE ) )
    {
        x -= title_h; out[ n++ ] = ( native_btn_t ){ { x, win_y, title_h, title_h }, NATIVE_BTN_MAXIMIZE };
    }
    if ( !( flags & GUI_WIN_NO_MINIMIZE ) )
    {
        x -= title_h; out[ n++ ] = ( native_btn_t ){ { x, win_y, title_h, title_h }, NATIVE_BTN_MINIMIZE };
    }

    /* Pop-in, innermost -- the slot the docked window's detach button occupies, so the control
       does not jump over min / max when the window tears off or merges back.  Never dropped: it
       is a floater's only way home. */
    if ( floater )
    {
        x -= title_h; out[ n++ ] = ( native_btn_t ){ { x, win_y, title_h, title_h }, NATIVE_BTN_POPIN };
    }
    return n;
}

/* Detach / pop-in mark: a vertical arrow over a dock tray -- lifting up and out when the window is
   attached (click = tear it off into its own OS window), dropping back down into the tray when it
   is floating (click = merge it back into the main surface).  The tray is the surface; the arrow
   is the panel leaving it or returning to it.

   Deliberately not a box: maximize and restore own the plain and doubled square outlines, and an
   outlined-vs-filled box reads as the same glyph twice.  The two states here share one shaft and
   differ only in which end carries the head, so the pair reads as one control with a direction.

   Sized to the maximize box (half-extent 0.20 * button, against its 0.18) so the mark carries the
   same weight as its neighbours rather than looming over them -- which is also why the tray is
   three strokes and not a closed frame.  The tray's turned-up end walls are what keep it from
   reading as the minimize bar sitting right beside it.  Every stroke is axis-aligned or exactly
   45 degrees, so the mark stays crisp at title-bar size and scales with DPI.

   One painter serves both the docked window's detach button (window_end) and the floater's caption
   pop-in, so the pair can never drift apart. */
static void
native_draw_dock_glyph( gui_rect_t r, bool attached, u32 col )
{
    f32 cx = r.x + r.w * 0.5f;
    f32 cy = r.y + r.h * 0.5f;
    f32 e  = floorf( r.h * 0.20f );      /* half-extent of the whole mark */
    if ( e < 3.0f ) e = 3.0f;
    /* Stroke from the glyph's own size, 1 px floor -- ink, not a border, so a border-0 theme
       keeps its caption glyphs.  Same derivation as draw_close_x, so the row strokes evenly. */
    f32 t  = floorf( r.h * 0.06f );  if ( t < 1.0f ) t = 1.0f;

    /* Dock tray along the bottom: a baseline with a short wall turned up at each end. */
    f32 yb = cy + e;
    gui_draw_line( cx - e, yb, cx + e, yb, t, col );
    gui_draw_line( cx - e, yb, cx - e, yb - e * 0.5f, t, col );
    gui_draw_line( cx + e, yb, cx + e, yb - e * 0.5f, t, col );

    /* The arrow: one shaft standing clear of the tray, head on the travelling end. */
    f32 y_top = cy - e;
    f32 y_bot = cy + e * 0.35f;
    f32 hw    = e * 0.55f;               /* head half-width == its leg drop, so the legs sit at 45 */

    gui_draw_line( cx, y_top, cx, y_bot, t, col );
    if ( attached )
    {
        gui_draw_line( cx, y_top, cx - hw, y_top + hw, t, col );   /* head up: out of the tray */
        gui_draw_line( cx, y_top, cx + hw, y_top + hw, t, col );
    }
    else
    {
        gui_draw_line( cx, y_bot, cx - hw, y_bot - hw, t, col );   /* head down: into the tray */
        gui_draw_line( cx, y_bot, cx + hw, y_bot - hw, t, col );
    }
}

/* Draw the glyph for one caption button, centered in its square: a minimize bar, a maximize box
   (two offset boxes when already maximized = restore), a close X, or the pop-in mark. */
static void
native_btn_draw_glyph( native_btn_kind_t kind, gui_rect_t r, bool maximized, u32 col )
{
    f32 cx = r.x + r.w * 0.5f;
    f32 cy = r.y + r.h * 0.5f;
    f32 s  = floorf( r.h * 0.18f );   /* glyph half-extent */
    f32 t  = floorf( r.h * 0.06f );  if ( t < 1.0f ) t = 1.0f;   /* ink weight -- see dock glyph */

    /* The glyph boxes are small line art -- draw them square so the frame radius cannot bend a
       maximize/restore box into a circle. */
    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );

    switch ( kind )
    {
        case NATIVE_BTN_MINIMIZE:
            gui_draw_line( cx - s, cy, cx + s, cy, t, col );
            break;

        case NATIVE_BTN_MAXIMIZE:
            if ( maximized )
            {
                /* Restore: two overlapping boxes, a back one up-right and a front one down-left. */
                f32 o = floorf( s * 0.5f );
                draw_push_rect_outline( cx - s + o, cy - s - o, 2.0f * s, 2.0f * s, t, col );
                draw_push_rect_outline( cx - s - o, cy - s + o, 2.0f * s, 2.0f * s, t, col );
            }
            else
            {
                draw_push_rect_outline( cx - s, cy - s, 2.0f * s, 2.0f * s, t, col );
            }
            break;

        case NATIVE_BTN_CLOSE:
            draw_close_x( r, col );
            break;

        case NATIVE_BTN_POPIN:
            /* The floating half of the detach pair: the arrow dropping back into the dock tray.
               Same painter the docked detach button uses, so the control keeps its identity
               across the tear-off. */
            native_draw_dock_glyph( r, false, col );
            break;
    }

    draw_set_rounding( save_round );
}

/*==============================================================================================
    Per-frame sync + the button-request seam
==============================================================================================*/

/* Sync a native window's geometry to its OS-owned surface and publish the frame layout (caption
   band + resize border) to app() -- the OS drives move/resize/Aero-snap through WM_NCHITTEST, gui
   just pins geometry and tells it where the grab bands are.  Also tracks RESTORE geometry for a
   CLOSEABLE floater so closing it captures the right state: win->w/h are the maximized size while
   maximized, so sample the home position and restore size only on restored frames; record the
   maximized state either way so re-open can re-maximize while still holding the previous normal
   size to restore back to. */
static void
window_sync_native( gui_window_t* win, gui_win_flags_t flags )
{
    const gui_viewport_t* vp = &s_vp_pool[ win->viewport ];
    win->x = 0.0f;
    win->y = 0.0f;
    if ( vp->disp_w > 0 ) win->w = ( f32 )vp->disp_w;
    if ( vp->disp_h > 0 ) win->h = ( f32 )vp->disp_h;

    i32 border = ( flags & GUI_WIN_NORESIZE ) ? 0 : ( i32 )RESIZE_BAND_OUTER;
    app()->window_set_native_frame( window_native_id( win ), true, border );

    /* Snap the OS client area to the same lattice the gui windows inside it snap to, so a live
       edge-drag of this surface always leaves a whole number of grid cells and docked/snapped
       windows stay flush with its edges.  0 when the grid is off (grid_quantum <= 1 or the
       GUI_GRID_LATTICE compile switch is off) -> free-pixel resize, the pre-grid behavior. */
#if GUI_GRID_LATTICE
    i32 step = ( GRID_Q > 1 ) ? ( i32 )GRID_Q : 0;
#else
    i32 step = 0;
#endif
    app()->window_set_size_step( window_native_id( win ), step, step );

    i32 caption = ( flags & GUI_WIN_NOTITLEBAR ) ? 0 : ( i32 )WIN_TITLE_H;
    s_vp_pool[ win->viewport ].caption_inset      = ( f32 )caption;
    s_vp_pool[ win->viewport ].caption_seen_frame = gui_frame_index();

    if ( win->viewport != 0 && ( flags & GUI_WIN_CLOSEABLE ) )
    {
        win_id_t os = window_native_id( win );
        win->reopen.maximized = app()->window_state( os ).maximized != 0;
        if ( !win->reopen.maximized )
        {
            app()->window_get_pos( os, &win->reopen.home_x, &win->reopen.home_y );
            win->reopen.w = win->w;
            win->reopen.h = win->h;
        }
    }
}

/* Enqueue a button-triggered tear-off or merge-back into the one-shot s_vp_request slot
   (core/gui_surface.c) -- shared by the pop-in caption button here and the plain detach
   button in window_end.  Idempotent: a single slot covers the one dragged window at a time, so
   the first caller wins. */
static void
native_popin_request( gui_window_t* win )
{
    if ( s_vp_request.active ) return;
    s_vp_request.active  = true;
    s_vp_request.by_drag = false;
    s_vp_request.win_id  = s_build.win.id;
    s_vp_request.from_vp = win ? win->viewport : 0u;
    s_vp_request.title   = s_build.win.title;
    s_vp_request.owner   = g_ctx;   /* this window's context, for the reconcile's lookup */
}

/*==============================================================================================
    native_caption_chrome -- the caption strip of window_end's deferred chrome.

    A native window's titlebar IS the OS caption, so its collapse arrow, detach button and gui
    drag-grab are all suppressed in window_end -- instead it gets these OS-window controls.
    window_begin published these exact rects as HTCLIENT holes, so a click here reaches gui
    rather than starting an OS move.  The buttons run right-to-left from the bar's right edge;
    returns the x that bounds the title text (the leftmost button's left edge minus a pad), or
    `right_limit` unchanged when there are no buttons.
==============================================================================================*/

static f32
native_caption_chrome( gui_window_t* win, f32 title_h, f32 right_limit )
{
    win_id_t     os   = window_native_id( win );
    bool         zoom = app()->window_state( os ).maximized != 0;
    native_btn_t btns[ NATIVE_BTN_MAX ];
    i32          nb   = native_caption_buttons( win, s_build.win.flags, s_build.win.x, s_build.win.y,
                                                s_build.win.w, title_h, btns );

    for ( i32 i = 0; i < nb; ++i )
    {
        gui_rect_t       br  = btns[ i ].r;
        gui_id_t         bid = id_combine( s_build.win.id,
                                           GUI_NATIVE_BTN_SALT + ( u32 )btns[ i ].kind );
        gui_item_state_t bs  = item_state( bid, br, ITEM_BUTTON );

        /* Hover/press background so the control reads as clickable -- a control frame, so it
           takes the widget radius (the glyph itself squares off in native_btn_draw_glyph). */
        if ( bs.hover || bs.active )
        {
            draw_set_rounding( ROUND_WIDGET );
            draw_push_rect_filled( br.x, br.y, br.w, br.h, 0, 0, 1, 1, 0, bs.active ? COL_BG_ACTIVE : COL_BG_HOT );
        }

        native_btn_draw_glyph( btns[ i ].kind, br, zoom, col_btn_glyph( bs ) );

        if ( bs.clicked )
        {
            switch ( btns[ i ].kind )
            {
                case NATIVE_BTN_MINIMIZE: app()->window_minimize( os );         break;
                case NATIVE_BTN_MAXIMIZE: app()->window_toggle_maximize( os );  break;
                case NATIVE_BTN_CLOSE:
                    /* A floater's close hides the CLOSEABLE window and sets reopen.floater;
                       the abandoned-teardown frees this OS surface next frame, and the next
                       begin re-spawns it from the restore geometry tracked in
                       window_sync_native (so a maximized floater re-opens maximized).  The
                       main window's close is the graceful application quit. */
                    if ( win && win->viewport != 0 )
                    {
                        win->reopen.floater = true;
                        win->closed         = true;
                    }
                    else
                        app()->window_request_close( os );
                    break;
                case NATIVE_BTN_POPIN: native_popin_request( win ); break;
            }
        }
    }

    if ( nb > 0 )
        right_limit = btns[ nb - 1 ].r.x - WIDGET_PAD;   /* leftmost button bounds the title */
    return right_limit;
}

// clang-format on
/*============================================================================================*/
