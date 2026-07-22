/*==============================================================================================

    runtime_service/gui/chrome/window/gui_select.c -- Window text selection (GUI_WIN_TEXT_SELECT).

    CHROME, not a gesture mechanism: the controller reads the
    render server's run capture (select_capture_* / select_run, render/gui_select_capture.c)
    and measures with the draw unit's font metrics -- server crossings the interact unit must
    never make.  Its true mechanisms are the generic core verbs it rides (interact_claim /
    interact_held / interact_idle) plus the highlight paint, which is chrome's to draw.  So
    the whole controller lives with its only callers: the window begins (select_paint_under)
    and gui_window_end (select_window_end), all in this unit.

    The UI half of selectable display text; the backend half (the run capture) is
    render/gui_select_capture.c.  A window flagged GUI_WIN_TEXT_SELECT gets two selection
    gestures over every text run it draws -- including text drawn BY widgets (a button's
    label is an ordinary text command in the window's segments, so it captures like any run):

      SWEEP -- press ON a text run and drag: the linear web selection, whole lines between
               the endpoints, clipped at the endpoints' characters on the first / last line.
      RECT  -- press on window-body dead space and drag: a marquee box; every character
               whose glyph the box covers is selected, line by line (column select).

    Either way Ctrl+C copies the covered runs to the OS clipboard (newline between lines,
    space between runs sharing a line), and Escape, a click that starts nothing, or a press
    in another window clears.

    Display text stays interaction-less -- nothing here touches the widgets.  The selection
    is a fallback consumer of presses no widget claimed (interact_idle), so buttons,
    selectables and scrollbars always win arbitration; while a gesture is in flight it holds
    active_id (the standard drag modality) under a salted window id, exactly like an edge
    resize.  All interaction is gated to the window BODY (below the title bar), so the
    title's own text run can never swallow a titlebar drag.

    The controller runs from gui_window_end while the flagged window's content clip is still
    active (before layout_pop_region), so the highlight rects clip and batch with the text
    they cover.  It works against LAST frame's captured runs -- the standard one-frame
    self-measurement lag; content is static while the user sweeps, so the lag is invisible.

    ONE selection exists at a time, in one window (mirroring the single hover/active/focus).
    Sweep endpoints are (run index, byte offset) pairs; the covered range of each run is
    resolved GEOMETRICALLY from the endpoints' pixel positions ordered by (y, x), so emit
    order never matters.  Rect endpoints are window-relative pixels, so the box rides a
    window move; content scrolling under either gesture changes what is covered -- select
    what you see.  A capture rebuild (every dirty frame) revalidates sweep endpoints by
    range; runs that shift under them clear the selection.

==============================================================================================*/
// clang-format off

/* Salted id for the gesture's active_id claim, distinct from the window drag (bare id), its
   resize (GUI_RESIZE_SALT), and every widget id. */
#define GUI_SELECT_SALT 0x5E1EC7EDu

#define SELECT_ABS( v ) ( ( v ) < 0.0f ? -( v ) : ( v ) )

static struct
{
    gui_id_t win;         /* selection owner window; GUI_ID_NONE = no selection */
    bool     dragging;    /* press-drag gesture in flight (active_id held) */
    bool     rect_mode;   /* RECT marquee vs SWEEP linear */
    u32      serial;      /* capture serial the sweep indices below are valid against */

    u32 a_run, a_chr;     /* SWEEP anchor endpoint: (run index, byte offset) -- fixed at press */
    u32 c_run, c_chr;     /* SWEEP cursor endpoint -- follows the mouse during the drag */

    f32 rx0, ry0;         /* RECT anchor corner, window-relative -- fixed at press */
    f32 rx1, ry1;         /* RECT cursor corner, window-relative -- follows the mouse */

} s_select;

/* The covered region for the active mode, resolved to ordered pixels once per use.  SWEEP:
   (y0,x0) is the visually-first endpoint, (y1,x1) the last -- lines between are covered
   whole.  RECT: the marquee box -- every line clips to [x0, x1]. */
typedef struct
{
    bool rect;
    f32  y0, x0, y1, x1;

} select_span_t;

/*==============================================================================================
    Geometry helpers
==============================================================================================*/

/* The window's content view: the body region's gutter-adjusted rect (the window root region is
   still open when select_window_end runs, so lf() is that frame).  The interaction gate for
   both gestures -- it excludes the title bar (whose text is also a captured run, and whose
   presses belong to the window drag) AND the scrollbar gutters (whose presses belong to the
   bars arbitrating in layout_pop_region, after this hook). */
static gui_rect_t
select_body_rect( void )
{
    return lf()->view;
}

/*==============================================================================================
    Font-correct measurement.  Runs carry their segment's font id; every metric walk below
    activates it first so a push_font'd run measures true.  select_window_end restores the
    font it entered with before returning, so later chrome (titlebar text) is unaffected.
==============================================================================================*/

static void
select_font_for( const gui_select_run_t* r )
{
    if ( font_active_id() != r->font )
        font_use( r->font );
}

/* Pixel x of byte boundary `chr` within the run (its font already active). */
static f32
select_x_at( const gui_select_run_t* r, u32 chr )
{
    const char* s = select_run_text( r );
    f32         x = r->x;
    for ( u32 i = 0; i < chr && i < r->len; ++i )
        x += font_char_advance( (u8)s[ i ] );
    return x;
}

/* Byte boundary nearest to pixel `px` within the run (its font already active): a hit past a
   glyph's midpoint lands after it, so the caret feels like every text editor's. */
static u32
select_chr_from_x( const gui_select_run_t* r, f32 px )
{
    const char* s = select_run_text( r );
    f32         x = r->x;
    for ( u32 i = 0; i < r->len; ++i )
    {
        f32 adv = font_char_advance( (u8)s[ i ] );
        if ( px < x + adv * 0.5f )
            return i;
        x += adv;
    }
    return r->len;
}

/*==============================================================================================
    Endpoint / coverage resolution
==============================================================================================*/

/* True when the run's glyph band lies inside the window body (its font already active) --
   excludes the title text and any chrome run from every walk below. */
static bool
select_run_in_body( const gui_select_run_t* r, gui_rect_t body )
{
    f32 ch = font_char_h();
    return r->y + ch > body.y && r->y < body.y + body.h;
}

/* Resolve the mouse to the nearest (run, chr) among `win`'s body runs.  require_hit demands
   the cursor actually be on a run's rect (the press gate); otherwise the nearest run wins
   (the sweep gate -- dragging past the ends selects to start / end, like every editor).
   Vertical distance to the run's glyph band decides the line; horizontal breaks ties. */
static bool
select_pos_from_mouse( gui_id_t win, gui_rect_t body, bool require_hit,
                       u32* out_run, u32* out_chr )
{
    u32  n      = select_run_count();
    f32  best   = 1e30f;
    u32  bi     = 0;
    bool found  = false;

    for ( u32 i = 0; i < n; ++i )
    {
        const gui_select_run_t* r = select_run( i );
        if ( r->win != win )
            continue;

        select_font_for( r );
        if ( !select_run_in_body( r, body ) )
            continue;

        f32 ch = font_char_h();
        f32 w  = select_x_at( r, r->len ) - r->x;

        f32 dy = 0.0f;
        if ( s_io.mouse_y < r->y )           dy = r->y - s_io.mouse_y;
        else if ( s_io.mouse_y > r->y + ch ) dy = s_io.mouse_y - ( r->y + ch );
        f32 dx = 0.0f;
        if ( s_io.mouse_x < r->x )           dx = r->x - s_io.mouse_x;
        else if ( s_io.mouse_x > r->x + w )  dx = s_io.mouse_x - ( r->x + w );

        if ( require_hit && ( dy > 0.0f || dx > 2.0f ) )
            continue;   /* press must land on the run (2px of horizontal grace) */

        /* The line owns the pick: any vertical miss outweighs the largest horizontal one. */
        f32 d = dy * 4096.0f + dx;
        if ( d < best )
        {
            best  = d;
            bi    = i;
            found = true;
        }
    }

    if ( !found )
        return false;

    const gui_select_run_t* r = select_run( bi );
    select_font_for( r );
    *out_run = bi;
    *out_chr = select_chr_from_x( r, s_io.mouse_x );
    return true;
}

/* The active gesture's covered region as ordered pixels.  SWEEP orders the two endpoints by
   (y, x) with half-a-line tolerance so runs sharing a baseline count as one line; RECT
   normalizes the marquee corners and clamps the box to the window body (so a drag that
   strays over the title bar never covers its text). */
static select_span_t
select_span( gui_rect_t body )
{
    select_span_t s;
    s.rect = s_select.rect_mode;

    if ( s.rect )
    {
        f32 ax = s_build.win.x + s_select.rx0, ay = s_build.win.y + s_select.ry0;
        f32 cx = s_build.win.x + s_select.rx1, cy = s_build.win.y + s_select.ry1;
        s.x0 = ax < cx ? ax : cx;   s.x1 = ax < cx ? cx : ax;
        s.y0 = ay < cy ? ay : cy;   s.y1 = ay < cy ? cy : ay;

        if ( s.x0 < body.x )          s.x0 = body.x;
        if ( s.y0 < body.y )          s.y0 = body.y;
        if ( s.x1 > body.x + body.w ) s.x1 = body.x + body.w;
        if ( s.y1 > body.y + body.h ) s.y1 = body.y + body.h;
        return s;
    }

    const gui_select_run_t* ra = select_run( s_select.a_run );
    const gui_select_run_t* rc = select_run( s_select.c_run );

    select_font_for( ra );
    f32 ay = ra->y, ax = select_x_at( ra, s_select.a_chr ), ah = font_char_h();
    select_font_for( rc );
    f32 cy = rc->y, cx = select_x_at( rc, s_select.c_chr );

    bool a_first = ( SELECT_ABS( ay - cy ) > ah * 0.5f ) ? ( ay < cy ) : ( ax <= cx );
    if ( a_first ) { s.y0 = ay; s.x0 = ax; s.y1 = cy; s.x1 = cx; }
    else           { s.y0 = cy; s.x0 = cx; s.y1 = ay; s.x1 = ax; }
    return s;
}

/* Covered byte range [lo, hi) of one run against the span; false when the run lies outside.
   SWEEP: a run on the start line clips at x0, on the end line at x1, between lines it is
   covered whole -- the web model.  RECT: the run's glyph band must overlap the box
   vertically, and EVERY line clips to [x0, x1] -- the marquee model. */
static bool
select_run_covered( const gui_select_run_t* r, const select_span_t* s, u32* lo, u32* hi )
{
    select_font_for( r );
    f32 ch = font_char_h();

    if ( s->rect )
    {
        if ( r->y >= s->y1 || r->y + ch <= s->y0 )
            return false;
        *lo = select_chr_from_x( r, s->x0 );
        *hi = select_chr_from_x( r, s->x1 );
        return *lo < *hi;
    }

    f32 tol = ch * 0.5f;
    if ( r->y < s->y0 - tol || r->y > s->y1 + tol )
        return false;

    bool on_first = SELECT_ABS( r->y - s->y0 ) <= tol;
    bool on_last  = SELECT_ABS( r->y - s->y1 ) <= tol;

    *lo = on_first ? select_chr_from_x( r, s->x0 ) : 0;
    *hi = on_last  ? select_chr_from_x( r, s->x1 ) : r->len;
    return *lo < *hi;
}

/*==============================================================================================
    Actions
==============================================================================================*/

static bool
select_exists( void )
{
    if ( s_select.win == GUI_ID_NONE )
        return false;
    if ( s_select.rect_mode )
        return SELECT_ABS( s_select.rx1 - s_select.rx0 ) > 2.0f
            || SELECT_ABS( s_select.ry1 - s_select.ry0 ) > 2.0f;
    return !( s_select.a_run == s_select.c_run && s_select.a_chr == s_select.c_chr );
}

static void
select_clear( void )
{
    if ( s_select.win != GUI_ID_NONE )
        g_ctx->retained.wants_redraw = true;   /* the highlight must visibly drop */
    s_select.win      = GUI_ID_NONE;
    s_select.dragging = false;
}

/* Copy the covered runs to the OS clipboard, in visual (y, x) order: newline between lines,
   a space between runs sharing a line (same_line columns).  Bounded by the capture pools, so
   the scratch below covers the worst case; app()->clipboard_set takes it unbounded. */
static void
select_copy( gui_id_t win, gui_rect_t body )
{
    static char buf[ GUI_SELECT_TEXT_POOL + GUI_SELECT_MAX_RUNS + 1 ];

    select_span_t span = select_span( body );

    /* Visual order: index the window's covered runs, insertion-sorted by (y, x). */
    u32 order[ GUI_SELECT_MAX_RUNS ];
    u32 count = 0;
    u32 n     = select_run_count();
    for ( u32 i = 0; i < n; ++i )
    {
        const gui_select_run_t* r = select_run( i );
        u32 lo, hi;
        if ( r->win != win || !select_run_covered( r, &span, &lo, &hi ) )
            continue;
        u32 j = count++;
        while ( j > 0 )
        {
            const gui_select_run_t* p = select_run( order[ j - 1 ] );
            bool after = ( SELECT_ABS( p->y - r->y ) > 1.0f ) ? ( p->y < r->y ) : ( p->x <= r->x );
            if ( after )
                break;
            order[ j ] = order[ j - 1 ];
            --j;
        }
        order[ j ] = i;
    }

    u32 used   = 0;
    f32 prev_y = 0.0f;
    for ( u32 k = 0; k < count; ++k )
    {
        const gui_select_run_t* r = select_run( order[ k ] );
        u32 lo, hi;
        select_run_covered( r, &span, &lo, &hi );

        if ( k > 0 && used + 1 < sizeof( buf ) )
            buf[ used++ ] = ( SELECT_ABS( r->y - prev_y ) > 1.0f ) ? '\n' : ' ';
        prev_y = r->y;

        u32 len = hi - lo;
        if ( used + len >= sizeof( buf ) )
            len = (u32)sizeof( buf ) - 1 - used;
        memcpy( buf + used, select_run_text( r ) + lo, len );
        used += len;
    }
    buf[ used ] = '\0';

    if ( used )
        app()->clipboard_set( buf );
}

/* Paint the selection UNDER the window's content -- called from the window begins (free +
   docked) right after the body region opens, before any widget emits, so the opaque bands
   sit BEHIND the text exactly like an editor's selection.  The original glyphs then render
   over them untouched: fully solid highlight, perfectly crisp text, and no extra glyph
   geometry (an earlier over-the-text version redrew every covered glyph and doubled the
   text load -- big selections blew the tess caps).  Bands come from LAST frame's runs (the
   capture lag): during a drag the highlight trails the cursor by one frame, which the
   mouse-move-dirty frames make imperceptible.  The live marquee's box fill draws here too
   (under content, so widgets read through it); its outline paints at window_end, on top. */
void
select_paint_under( void )
{
    gui_id_t win = s_build.win.id;
    if ( s_select.win != win )
        return;

    bool live_box = s_select.rect_mode && s_select.dragging;
    if ( !select_exists() && !live_box )
        return;

    u32           saved = font_active_id();
    gui_rect_t    body  = select_body_rect();
    select_span_t span  = select_span( body );

    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );

    if ( live_box && span.x1 > span.x0 && span.y1 > span.y0 )
        draw_push_rect_filled( span.x0, span.y0, span.x1 - span.x0, span.y1 - span.y0,
                               0, 0, 1, 1, 0,
                               ( COL_WIDGET_ACT & 0x00FFFFFFu ) | 0x30000000u );

    if ( select_exists() )
    {
        u32 n = select_run_count();
        for ( u32 i = 0; i < n; ++i )
        {
            const gui_select_run_t* r = select_run( i );
            u32 lo, hi;
            if ( r->win != win || !select_run_covered( r, &span, &lo, &hi ) )
                continue;

            f32 rx0 = select_x_at( r, lo );
            f32 rx1 = select_x_at( r, hi );
            gui_rect_t band = rect_intersect(
                ( gui_rect_t ){ rx0, r->y, rx1 - rx0, font_char_h() }, r->clip );
            if ( band.w > 0.0f && band.h > 0.0f )
                draw_push_rect_filled( band.x, band.y, band.w, band.h,
                                       0, 0, 1, 1, 0, COL_WIDGET_ACT );
        }
    }

    draw_set_rounding( save_round );
    if ( font_active_id() != saved )
        font_use( saved );   /* the covered-range walks measure with each run's font */
}

/* The over-content half of the highlight, painted at window_end: a LIGHT translucent band per
   covered run, plus the live marquee outline.  The translucent bands exist for text drawn BY
   widgets -- a button's background fill paints after select_paint_under and hides the opaque
   under-band beneath its label, so this pass is the only highlight that reads there.  Over
   plain text (where the solid under-band already shows) the same hue at low alpha just deepens
   the band slightly instead of washing the glyphs out. */
static void
select_paint_overlay( gui_id_t win, gui_rect_t body )
{
    bool live_box = s_select.rect_mode && s_select.dragging;
    if ( !select_exists() && !live_box )
        return;

    select_span_t span = select_span( body );

    f32 save_round = draw_rounding();
    draw_set_rounding( 0.0f );

    if ( select_exists() )
    {
        u32 tint = ( COL_CHECK_MARK & 0x00FFFFFFu ) | 0x50000000u;   // 0x50000000u;   /* ~30% alpha */
        // u32 tint = ( COL_WIDGET_ACT & 0x00FFFFFFu ) | 0x48000000u;   /* ~28% alpha */
        u32 n    = select_run_count();
        for ( u32 i = 0; i < n; ++i )
        {
            const gui_select_run_t* r = select_run( i );
            u32 lo, hi;
            if ( r->win != win || !select_run_covered( r, &span, &lo, &hi ) )
                continue;

            f32 rx0 = select_x_at( r, lo );
            f32 rx1 = select_x_at( r, hi );
            gui_rect_t band = rect_intersect(
                ( gui_rect_t ){ rx0, r->y, rx1 - rx0, font_char_h() }, r->clip );
            if ( band.w > 0.0f && band.h > 0.0f )
                draw_push_rect_filled( band.x, band.y, band.w, band.h, 0, 0, 1, 1, 0, tint );
        }
    }

    /* Marquee outline over everything (its fill sits under the content, from paint_under). */
    if ( live_box && span.x1 > span.x0 && span.y1 > span.y0 )
        draw_push_rect_outline( span.x0, span.y0, span.x1 - span.x0, span.y1 - span.y0,
                                1.0f, 0, ( COL_WIDGET_ACT & 0x00FFFFFFu ) | 0xC0000000u );

    draw_set_rounding( save_round );
}

/*==============================================================================================
    select_window_end -- one frame of the selection protocol for the current window.

    Called by gui_window_end for an expanded GUI_WIN_TEXT_SELECT window, inside its content
    clip (before layout_pop_region).  Owns marking, revalidation, the press/drag protocol,
    keys, and the marquee outline; the selection bands themselves paint at window BEGIN
    (select_paint_under) so they sit beneath the content.  Order within a frame: this
    window's widgets have already arbitrated (a press any of them claimed shows in
    active_id); the scrollbars and the window move-grab run after, and both respect an
    active_id this function claims.
==============================================================================================*/

void
select_window_end( void )
{
    gui_id_t   win    = s_build.win.id;
    gui_id_t   sel_id = id_combine( win, GUI_SELECT_SALT );
    gui_rect_t body   = select_body_rect();
    u32        saved  = font_active_id();

    select_capture_mark( win );   /* capture this window's runs at this frame's build seam */

    /* Revalidate held sweep endpoints against a rebuilt capture (every dirty frame rebuilds).
       By range only -- content that merely re-emitted identically keeps the selection; runs
       that shifted or vanished under the endpoints clear it (select what you see).  A rect
       selection holds no run indices, so it rides any rebuild. */
    if ( s_select.win == win && select_capture_serial() != s_select.serial )
    {
        s_select.serial = select_capture_serial();
        if ( !s_select.rect_mode )
        {
            u32 n = select_run_count();
            const gui_select_run_t* ra = select_run( s_select.a_run );
            const gui_select_run_t* rc = select_run( s_select.c_run );
            if ( s_select.a_run >= n || s_select.c_run >= n
              || ra->win != win || rc->win != win
              || s_select.a_chr > ra->len || s_select.c_chr > rc->len )
                select_clear();
        }
    }

    /* Gesture in flight: follow the mouse with the cursor endpoint / marquee corner, and
       finish on release.  A gesture that never opened a range (a plain click) clears on
       release -- web behavior. */
    if ( interact_held( sel_id ) )
    {
        if ( s_select.rect_mode )
        {
            f32 mx = s_io.mouse_x - s_build.win.x;
            f32 my = s_io.mouse_y - s_build.win.y;
            if ( mx != s_select.rx1 || my != s_select.ry1 )
                g_ctx->retained.wants_redraw = true;
            s_select.rx1 = mx;
            s_select.ry1 = my;
        }
        else
        {
            u32 run, chr;
            if ( select_pos_from_mouse( win, body, false, &run, &chr ) )
            {
                if ( run != s_select.c_run || chr != s_select.c_chr )
                    g_ctx->retained.wants_redraw = true;
                s_select.c_run = run;
                s_select.c_chr = chr;
            }
            cursor_set( APP_CURSOR_TEXT );   /* hold the I-beam through the sweep */
        }

        if ( s_io.mouse_released[ 0 ] )
        {
            s_select.dragging = false;
            if ( !select_exists() )
                select_clear();
        }
    }
    else if ( s_select.win == win && s_select.dragging )
        s_select.dragging = false;   /* grab lost elsewhere (modal steal): settle as-is */

    /* Press arbitration -- the fallback consumer.  A press nothing claimed, in this window's
       body: on a text run it starts a SWEEP; on dead space it starts a RECT marquee (this is
       what makes dead-space presses draggable at all -- without the flag they fall through to
       nothing).  Any press while another window is front-most drops the selection. */
    if ( s_io.mouse_pressed[ 0 ] )
    {
        if ( s_interaction.hover_win == win && s_interaction.hover_id == GUI_ID_NONE
          && interact_idle() && rect_hit( body ) )
        {
            u32 run, chr;
            if ( select_pos_from_mouse( win, body, true, &run, &chr ) )
            {
                interact_claim( sel_id, 0 );
                s_select.win       = win;
                s_select.dragging  = true;
                s_select.rect_mode = false;
                s_select.serial    = select_capture_serial();
                s_select.a_run     = s_select.c_run = run;
                s_select.a_chr     = s_select.c_chr = chr;
                g_ctx->retained.wants_redraw = true;
            }
            else
            {
                interact_claim( sel_id, 0 );
                s_select.win       = win;
                s_select.dragging  = true;
                s_select.rect_mode = true;
                s_select.serial    = select_capture_serial();
                s_select.rx0       = s_select.rx1 = s_io.mouse_x - s_build.win.x;
                s_select.ry0       = s_select.ry1 = s_io.mouse_y - s_build.win.y;
                g_ctx->retained.wants_redraw = true;
            }
        }
        else if ( s_select.win == win && s_interaction.hover_win != win )
            select_clear();
    }

    /* Hover feedback: the I-beam over selectable text when nothing else owns the cursor. */
    if ( !interact_held( sel_id ) && interact_idle()
      && s_interaction.hover_win == win && s_interaction.hover_id == GUI_ID_NONE )
    {
        u32 run, chr;
        if ( select_pos_from_mouse( win, body, true, &run, &chr ) )
            cursor_set( APP_CURSOR_TEXT );
    }

    /* Keys act when no text field owns the keyboard, OR one does but has no selection of its own
       (a focused field with a live selection runs its own copy/cut for it, so the window selection
       yields to it -- but an exclusive-mode input like the console keeps focus with an empty caret
       while the scrollback is swept, and there the window selection rightly owns Ctrl+C).  Neither
       key is claimed -- Escape may also close a popup, and should. */
    bool keys_for_window_sel = ( s_interaction.focused_id == GUI_ID_NONE )
                            || !s_interaction.focus_has_selection;
    if ( s_select.win == win && select_exists() && keys_for_window_sel )
    {
        if ( io_ctrl() && s_io.keys_pressed[ APP_KEY_C ] )
            select_copy( win, body );
        if ( s_io.keys_pressed[ APP_KEY_ESCAPE ] )
            select_clear();
    }

    /* The opaque bands painted UNDER the content at window_begin (select_paint_under) carry
       the highlight for plain text; this over-pass tints widget-covered runs and draws the
       live marquee outline. */
    if ( s_select.win == win )
        select_paint_overlay( win, body );

    if ( font_active_id() != saved )
        font_use( saved );   /* metric walks may have switched fonts; later chrome must not see it */
}

// clang-format on
/*============================================================================================*/
