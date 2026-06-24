/*==============================================================================================

    runtime_service/imgui/imgui_widget_core.c -- Shared widget primitives + theme.

    The foundation the rest of the widget layer is built on: the layout-derived size
    macros, the color palette, the label grammar + content placement (rect_align / arrows),
    and the per-widget interaction state machine.  Both the leaf widgets (imgui_widget.c) and
    the window chrome (imgui_widget_window.c) draw and interact through these, so they live
    here, ahead of both in the unity build.  The shared edge-resize geometry is imgui_resize.c
    and the layout engine (track resolver + cell emitters) is imgui_layout_core.c, both next.

    Interaction uses the classic hover/active/focused model:
        hover   : the cursor is over the widget this frame
        active  : the primary button is held with this widget as the target
        focused : this widget owns keyboard input (input_text)

    Included by imgui.c after imgui_window.c so s_interaction, s_build, s_io, s_layout, rect_hit,
    and the draw helpers are all in scope.

==============================================================================================*/
#include "runtime_service/imgui/imgui_internal.h"   /* widget_kind_t, widget_state_t */
// clang-format off

/*----------------------------------------------------------------------------------------------
    Layout accessors  (read from s_layout, computed by layout_compute() in imgui.c)
----------------------------------------------------------------------------------------------*/

/* Each resolves through style_var (imgui_style.c): the font-derived base with any push_style_var /
   next_style_var override applied, so every read here honors the style stacks with no call-site
   change.  See imgui_style_var_t for the slots. */
#define WIDGET_H      style_var( IMGUI_VAR_LINE_SIZE     )
#define WIDGET_GAP    style_var( IMGUI_VAR_WIDGET_GAP    )
#define WIDGET_PAD    style_var( IMGUI_VAR_WIDGET_PAD    )
#define WIN_TITLE_H   style_var( IMGUI_VAR_WIN_TITLE_H   )
#define WIN_BORDER    style_var( IMGUI_VAR_WIN_BORDER    )
#define CHECKBOX_SZ   style_var( IMGUI_VAR_CHECKBOX_SZ   )
#define SLIDER_KNOB_W style_var( IMGUI_VAR_SLIDER_KNOB_W )
#define WIDGET_MIN_W  style_var( IMGUI_VAR_MIN_CELL_W    )

/* Corner-radius categories, fed to draw_set_rounding (imgui_draw.c) so a draw site can pick the
   right rounding before emitting.  The item seam defaults to ROUND_WIDGET and the chrome seam to
   ROUND_WIN; grabs and squared-off marks override locally.  See imgui_style_var_t. */
#define ROUND_WIN     style_var( IMGUI_VAR_WIN_ROUNDING    )
#define ROUND_WIDGET  style_var( IMGUI_VAR_WIDGET_ROUNDING )
#define ROUND_GRAB    style_var( IMGUI_VAR_GRAB_ROUNDING   )

/* Default region padding (the inset every window body / child opens with): pad columns by
   WIDGET_PAD left and right, offset the first row by WIDGET_GAP, no bottom reserve. */
#define REGION_PAD_DEFAULT ( ( imgui_pad_t ){ WIDGET_PAD, WIDGET_PAD, WIDGET_GAP, 0.0f } )

/*----------------------------------------------------------------------------------------------
    Color palette (IMGUI_COLOR: byte order R,G,B,A in memory = ABGR u32)
----------------------------------------------------------------------------------------------*/

/* Each resolves through style_col (imgui_style.c): the theme default with any push_style_color /
   next_style_color override applied.  The defaults themselves live in k_col_default there; these
   names stay so every existing draw site keeps reading COL_* while gaining override support.
   See imgui_col_t for the slots. */
#define COL_WIN_BG       style_col( IMGUI_COL_WINDOW_BG    )
#define COL_CHILD_BG     style_col( IMGUI_COL_CHILD_BG     )
#define COL_TITLE_BG     style_col( IMGUI_COL_TITLE_BG     )
#define COL_BORDER       style_col( IMGUI_COL_BORDER       )
#define COL_TEXT         style_col( IMGUI_COL_TEXT         )
#define COL_TEXT_DIM     style_col( IMGUI_COL_TEXT_DIM     )
#define COL_WIDGET_BG    style_col( IMGUI_COL_WIDGET_BG    )
#define COL_WIDGET_HOT   style_col( IMGUI_COL_WIDGET_HOT   )
#define COL_WIDGET_ACT   style_col( IMGUI_COL_WIDGET_ACT   )
#define COL_WIDGET_FG    style_col( IMGUI_COL_WIDGET_FG    )
#define COL_CHECK_MARK   style_col( IMGUI_COL_CHECK_MARK   )
#define COL_SLIDER_TRACK style_col( IMGUI_COL_SLIDER_TRACK )
#define COL_RESIZE_HOT   style_col( IMGUI_COL_RESIZE_HOT   )
#define COL_INPUT_BG     style_col( IMGUI_COL_INPUT_BG     )
#define COL_INPUT_FOCUS  style_col( IMGUI_COL_INPUT_FOCUS  )
#define COL_CURSOR       style_col( IMGUI_COL_CURSOR       )
#define COL_NAV          style_col( IMGUI_COL_NAV_HIGHLIGHT )

/* Baseline y to vertically center one line of glyphs in a row of height h starting at y.
   Used by every labeled widget and the window title so the centering math lives in one place.
   (The text_center_y( y, h ) form is the VCENTER case of rect_align below, kept as a scalar
   convenience because most labeled widgets only need the y and already own their x.) */
static f32 text_center_y( f32 y, f32 h ) { return y + ( h - font_char_h() ) * 0.5f; }

/* Linear blend between two ABGR colors at t in [0,1] (0 = ca, 1 = cb).  Used by animated
   widgets that blend between palette entries rather than switching them discretely. */
static u32
col_lerp( u32 ca, u32 cb, f32 t )
{
    if ( t <= 0.0f ) return ca;
    if ( t >= 1.0f ) return cb;
    f32 r0 = (f32)( ( ca       ) & 0xFF );  f32 r1 = (f32)( ( cb       ) & 0xFF );
    f32 g0 = (f32)( ( ca >>  8 ) & 0xFF );  f32 g1 = (f32)( ( cb >>  8 ) & 0xFF );
    f32 b0 = (f32)( ( ca >> 16 ) & 0xFF );  f32 b1 = (f32)( ( cb >> 16 ) & 0xFF );
    f32 a0 = (f32)( ( ca >> 24 ) & 0xFF );  f32 a1 = (f32)( ( cb >> 24 ) & 0xFF );
    u32 r  = (u32)( r0 + ( r1 - r0 ) * t );
    u32 g  = (u32)( g0 + ( g1 - g0 ) * t );
    u32 b  = (u32)( b0 + ( b1 - b0 ) * t );
    u32 a  = (u32)( a0 + ( a1 - a0 ) * t );
    return r | ( g << 8 ) | ( b << 16 ) | ( a << 24 );
}

/* Place an extent `len` within the span [org, org+avail) along one axis: centered, against the far
   edge, or (default) the near edge.  The one axis primitive every aligned placement resolves
   through -- rect_align below for a box, and draw_text_in (imgui_widget.c) per line of a text
   block -- so a centered label, a right-flushed caption, and a bottom-anchored run share one rule. */
static f32
align_span( f32 org, f32 avail, f32 len, bool center, bool far )
{
    if ( center ) return org + ( avail - len ) * 0.5f;
    if ( far )    return org +   avail - len;
    return org;                                                   /* near edge -- LEFT / TOP default */
}

/* Horizontal / vertical placement within a cell span, reading the matching imgui_align_t bits. */
static f32 align_x( f32 x, f32 w, f32 len, u32 a ) { return align_span( x, w, len, a & IMGUI_ALIGN_HCENTER, a & IMGUI_ALIGN_RIGHT  ); }
static f32 align_y( f32 y, f32 h, f32 len, u32 a ) { return align_span( y, h, len, a & IMGUI_ALIGN_VCENTER, a & IMGUI_ALIGN_BOTTOM ); }

/* Place a natural nat_w x nat_h box inside `cell` per the alignment flags (imgui_align_t).  The
   single seam for positioning sub-cell content -- a button's label, a checkbox box, an aligned
   text run, a separator line -- so every widget edges / centers content the same way and a
   region's align setting flows through one place.  Returns the placed rect (w/h are nat_*). */
static imgui_rect_t
rect_align( imgui_rect_t cell, f32 nat_w, f32 nat_h, u32 align )
{
    return ( imgui_rect_t ){ align_x( cell.x, cell.w, nat_w, align ),
                             align_y( cell.y, cell.h, nat_h, align ), nat_w, nat_h };
}

/* The symbol render primitives -- the glyph marks (arrow / collapse arrow / check / bullet / close /
   pointing arrow) and the broader shape palette (frames, rounded rects, polygons, arcs, curves,
   dashes, gradients, shadows, text effects, grips, spinners) -- live in imgui_draw_symbol.c,
   included immediately after this file so they may use the COL_* / ROUND_* / WIN_BORDER macros and
   col_lerp defined here, and so every widget file below resolves them by name.  The public
   imgui_render_* surface over them is centralized there too. */

/*----------------------------------------------------------------------------------------------
    Widget label grammar  (Dear ImGui style)

        "Text"        -> display "Text",  id = hash("Text")
        "Text##key"   -> display "Text",  id = hash("Text##key")   distinct ids, same visible text
        "pre###key"   -> display "pre",   id = hash("###key")      id ignores a dynamic prefix

    The visible span ends at the first "##".  A "###" additionally re-roots the id hash at that
    "###", so a label whose visible part changes every frame (a counter, a name) keeps one stable
    id.  Every labeled widget routes its display through label_width / draw_label and its id
    through widget_id, so the grammar is honored uniformly in one place.
----------------------------------------------------------------------------------------------*/

/* Visible byte count: up to the first "##" marker, or the whole string. */
static u32
label_vis_len( const char* s )
{
    u32 i = 0;
    while ( s[ i ] )
    {
        if ( s[ i ] == '#' && s[ i + 1 ] == '#' )    /* s[i+1] is at worst the NUL: safe */
            break;
        ++i;
    }
    return i;
}

/* The substring hashed for the id: the whole label, unless a "###" tail re-roots it there. */
static const char*
label_id_str( const char* s )
{
    for ( u32 i = 0; s[ i ]; ++i )
        if ( s[ i ] == '#' && s[ i + 1 ] == '#' && s[ i + 2 ] == '#' )    /* reads stop at NUL */
            return s + i;
    return s;
}

/* The id for a labeled widget: the active scope seed combined with the label's id key. */
static imgui_id_t widget_id( const char* label ) { return id_combine( id_seed(), id_hash( label_id_str( label ) ) ); }

/* Width / draw of a label's visible span (markers stripped). */
static f32  label_width( const char* s )                         { return font_text_w_n( s, label_vis_len( s ) ); }
static void draw_label ( f32 x, f32 y, u32 c, const char* s )    { draw_push_text_n( x, y, c, s, label_vis_len( s ) ); }

/* Compact truncation ellipsis -- three baseline dots packed into ~1.2 glyph advances instead of
   three full '.' glyph cells.  A literal "..." spends three whole character advances on the cut
   marker, stealing space from the text and forcing the truncation earlier than necessary; these
   filled discs read the same but reserve far less, so more of the string survives.  Sized and
   seated from the active font's glyph box so they track font/scale changes, and rounded like a
   '.' (filled discs, not squares) to blend with the text.  ellipsis_w reports the advance the
   caller must reserve; draw_ellipsis paints it -- kept adjacent so the two never drift. */
static f32
ellipsis_dot_r( void )
{
    f32 r = font_char_h() * 0.065f;             /* dot radius scales with glyph height */
    return r < 0.75f ? 0.75f : r;               /* never sub-pixel -- stay visible at tiny fonts */
}

/* Reserved advance: a leading gap (2r) + three dots on a 3.5r center pitch (7r) = 9r total. */
static f32 ellipsis_w( void ) { return ellipsis_dot_r() * 9.0f; }

static void
draw_ellipsis( f32 x, f32 y, u32 c )
{
    f32 r  = ellipsis_dot_r();
    /* Seat the dot bottom on the baseline (~0.8 of the glyph box; the lower 0.2 is descent space)
       so it rests where a font '.' does, not down in the descender region. */
    f32 cy = y + font_char_h() * 0.8f - r;

    /* Per-dot alpha fade: each subsequent dot is dimmer, so the run trails off rather than
       stopping flat -- it reads as "text continues" the way a fading tail suggests. */
    static const f32 fade[ 3 ] = { 1.0f, 0.7f, 0.45f };
    u32              a0         = ( c >> 24 ) & 0xFFu;     /* source alpha (ABGR high byte) */
    u32              rgb        = c & 0x00FFFFFFu;

    /* Leading gap of 2r separates the dots from the truncated glyph; centers then step by 3.5r
       (a dot diameter plus a gap a touch over its width) so the run breathes like real periods. */
    for ( u32 i = 0; i < 3; ++i )
    {
        u32 a   = (u32)( (f32)a0 * fade[ i ] + 0.5f );
        u32 col = rgb | ( a << 24 );
        draw_push_circle_filled( x + r * 2.0f + (f32)i * r * 3.5f, cy, r, 10u, col );
    }
}

/* Draw at most `len` bytes of s left-anchored at x, fitted into max_w: when the run is wider than
   max_w, truncate on a glyph boundary and mark the cut with a compact ellipsis so a compressed
   widget reads as deliberately clipped rather than bleeding mid-glyph under the region clip.  When
   not even the ellipsis fits, the leading glyphs that do are drawn and the rest dropped -- never
   worse than a hard clip.  max_w <= 0 draws nothing.  Cheap: one width walk, no extra clip command
   (so draw batching is untouched).  draw_label_fit is the label-grammar wrapper; callers with a
   raw string (the window title) pass the whole length through here directly. */
static void
draw_text_fit_n( f32 x, f32 y, u32 c, const char* s, u32 len, f32 max_w )
{
    if ( max_w <= 0.0f ) return;

    /* Fits whole -- the common path: draw the span as-is. */
    if ( font_text_w_n( s, len ) <= max_w )
    {
        draw_push_text_n( x, y, c, s, len );
        return;
    }

    /* Too wide: reserve the compact ellipsis (dropped if even it will not fit), then take the
       longest glyph prefix that fits in the remaining budget. */
    f32  ell    = ellipsis_w();
    bool dots   = ( ell <= max_w );
    f32  budget = dots ? max_w - ell : max_w;

    f32 w = 0.0f;
    u32 n = 0;
    while ( n < len && s[ n ] )
    {
        f32 adv = font_char_advance( (u8)s[ n ] );
        if ( w + adv > budget ) break;
        w += adv;
        ++n;
    }

    draw_push_text_n( x, y, c, s, n );
    if ( dots )
        draw_ellipsis( x + w, y, c );
}

/* Clean-shrink companion to draw_label: fit a label's visible span (markers stripped) into max_w,
   ellipsizing when a cell squeezes it below its natural width.  Used by the labeled widgets. */
static void
draw_label_fit( f32 x, f32 y, u32 c, const char* s, f32 max_w )
{
    draw_text_fit_n( x, y, c, s, label_vis_len( s ), max_w );
}

/*----------------------------------------------------------------------------------------------
    Interaction state machine
----------------------------------------------------------------------------------------------*/

/* widget_kind_t (interaction class) and widget_state_t (per-frame interaction result) are
   defined in imgui_internal.h. */

/* Auto-repeat cadence for a held button (IMGUI_ITEM_BUTTON_REPEAT): the pause before the first
   repeat, then the interval between repeats.  Seconds; matches the familiar key-repeat feel. */
#define REPEAT_DELAY 0.30f
#define REPEAT_RATE  0.05f

/* One tick of the held-button repeat clock (state in s_interaction, since only one widget is active at a
   time).  Fires immediately on the press frame, then again once repeat_t crosses the initial delay
   and thereafter each rate interval.  Returns true on a fire frame; the caller routes it to
   st.clicked.  Subtracting the threshold (vs zeroing) keeps the cadence steady across uneven dt. */
static bool
widget_repeat_tick( bool pressed )
{
    if ( pressed )
    {
        s_interaction.repeat_t  = 0.0f;
        s_interaction.repeat_on = false;   /* the next fire waits the longer initial delay */
        return true;               /* press itself is the first fire */
    }

    s_interaction.repeat_t += s_io.dt;
    f32 thresh = s_interaction.repeat_on ? REPEAT_RATE : REPEAT_DELAY;
    if ( s_interaction.repeat_t >= thresh )
    {
        s_interaction.repeat_t -= thresh;
        s_interaction.repeat_on = true;    /* past the delay -- switch to the faster rate */
        return true;
    }
    return false;
}

/* Keyboard-nav per-item seam.  Called from widget_behavior for every item that belongs to the nav
   window (s_build.win_id == s_nav.win), the keyboard mirror of the hover hit-test above.  It does
   four things, all reading/writing the nav accumulator in s_nav (committed at the next nav_new_frame,
   imgui_nav.c): records this frame's rect for the current nav item, tracks emission order for Tab,
   scores the item as a directional-move candidate, and -- for the current nav item -- lights the
   focus ring and synthesizes a click from an Enter/Space activation so every widget activates from
   the keyboard with no per-widget code, exactly as a mouse click flows through st.clicked. */

#define NAV_RING 2.0f    /* focus-ring inset outside the item rect so the item's own fill spares it */

static void
nav_item_register( imgui_id_t id, imgui_rect_t r, widget_state_t* st, widget_kind_t kind )
{
    bool is_cur = ( id == s_nav.id );
    if ( is_cur )
    {
        s_nav.id_seen   = true;
        s_nav.self_rect = r;
    }

    /* Tab walks emission order (reading order here): first item, the predecessor of the current
       item, and the item right after it. */
    if ( s_nav.tab_first == IMGUI_ID_NONE ) s_nav.tab_first = id;
    if ( s_nav.tab_take ) { s_nav.tab_next = id; s_nav.tab_take = false; }
    if ( is_cur )                    s_nav.tab_take = true;       /* next item becomes tab_next */
    else if ( !s_nav.id_seen )   s_nav.tab_prev = id;        /* last one before the current */

    /* Directional move: score against last frame's nav_ref_rect (the deferred resolve). */
    if ( s_nav.move_dir >= 0 && !is_cur )
    {
        f32 sc = nav_score_dir( s_nav.ref_rect, r, (imgui_dir_t)s_nav.move_dir );
        if ( sc < s_nav.move_score )
        {
            s_nav.move_score = sc;
            s_nav.move_best  = id;
            s_nav.move_rect  = r;
        }
    }

    /* Current item: draw the outline ring whenever a nav cursor exists (even in mouse mode, so it
       keeps its location), and -- only while the keyboard is the active instrument (nav_highlight)
       -- give it the fill (st->nav, read by widget_bg_color / frame_bg_color) and apply a pending
       activation.  The ring is drawn before the widget's own background (widget_behavior runs
       first), inset outward by NAV_RING so the fill leaves the border visible. */
    if ( is_cur && s_nav.active )
    {
        draw_push_rect_outline( r.x - NAV_RING, r.y - NAV_RING,
                                r.w + 2.0f * NAV_RING, r.h + 2.0f * NAV_RING,
                                WIN_BORDER, 0, COL_NAV );

        if ( s_nav.highlight )
        {
            st->nav = true;
            if ( s_nav.activate )
            {
                st->pressed = st->clicked = true;
                if ( kind == WIDGET_KIND_FOCUSABLE )
                    s_interaction.focused_id = id;      /* Enter on an input box -> enter text capture */

                /* Consume the activating keys + any text so the item just focused does not also see
                   this frame's Enter (instant blur) or type the activating Space. */
                s_nav.activate = false;
                s_io.keys_pressed[ APP_KEY_ENTER ] = false;
                s_io.keys_pressed[ APP_KEY_SPACE ] = false;
                s_io.text[ 0 ] = '\0';
            }
        }
    }
}

/* Unified hover/active/focus/click state machine.  Call once per widget with the
   hit rect and the desired interaction kind; the returned flags are all a widget
   needs for drawing and value updates. */

static widget_state_t
widget_behavior( imgui_id_t id, imgui_rect_t r, widget_kind_t kind )
{
    widget_state_t st = { 0 };

    /* Latch the most recent item id for context menus / tooltips (popup_context_item_begin,
       set_item_tooltip).  Done before the disabled early-out so a disabled widget still counts
       as the last item -- the anchor is "what was just emitted", regardless of its state. */
    s_build.last_item_id   = id;
    s_build.last_item_rect = r;   /* item-query getters read this for "the widget just emitted" */

    /* Disabled item: inert this frame -- no hover, active, focus, or click.  Returning the zeroed
       state here is the one place that suppresses interaction for every widget, the behavioral half
       of IMGUI_ITEM_DISABLED (the visual dim is the draw list's global alpha, set at resolve).  The
       flags were latched by widget_next_rect_w just before this call. */
    if ( s_build.cur_item_flags & IMGUI_ITEM_DISABLED )
    {
        s_build.last_item_status = st;   /* a disabled item is still the last item, reported inert */
        return st;
    }

    /* Deaf context: not listening this frame -- render but return inert state.
       last_item_id/rect are latched above so item-query calls still work. */
    if ( !g_ctx->listening )
    {
        s_build.last_item_status = st;
        return st;
    }

    /* Hot only when this widget belongs to the window the cursor is over (hover_win,
       resolved last frame).  Widgets in any other window short-circuit before rect_hit,
       so occluded windows do no hit-testing at all -- occlusion is decided once, at the
       window level, not per widget.

       Modal-while-dragging: once any item owns active_id (a slider, scrollbar, or window
       drag is in flight) every other item is frozen -- only the active item may hover.
       The active item keeps interacting through st.active below, which reads active_id
       directly, so a drag stays live while the cursor sweeps over inert neighbours. */

    bool can_hover = ( s_interaction.active_id == IMGUI_ID_NONE || s_interaction.active_id == id );
    bool win_hover = ( s_build.win_id == s_interaction.hover_win );
    bool eligible  = can_hover && win_hover && !s_build.win_resize_hot && !s_build.win_grip_hot;

    /* While the keyboard is the active nav instrument (nav_highlight), the mouse does not set hover:
       the fill is mutually exclusive, so a mouse-hovered item never fills alongside the nav item
       (the nav ring still shows its location).  A mouse move or click drops nav_highlight
       (imgui_nav.c), re-enabling hover that same frame. */
    if ( eligible && !s_nav.highlight && rect_hit( s_build.clip_rect ) && rect_hit( r ) )
         s_interaction.hover_id = id;

    /* Press: capture active (and focus for focusable widgets) on button-down. */
    if ( s_interaction.hover_id == id && s_io.mouse_pressed[ 0 ] )
    {
        s_interaction.active_id = id;
        st.pressed      = true;
        if ( kind == WIDGET_KIND_FOCUSABLE )
            s_interaction.focused_id = id;

        /* Keep the nav ring synced to the last interacted item: a click moves the cursor here, so
           resuming the keyboard later continues from what was clicked (only once a ring exists). */
        if ( s_nav.active )
            s_nav.id = id;
    }

    st.hover   = ( s_interaction.hover_id == id );
    st.active  = ( s_interaction.active_id == id );
    st.focused = ( s_interaction.focused_id == id );
    st.clicked = s_io.mouse_released[ 0 ] && s_interaction.hover_id == id && s_interaction.active_id == id;

    /* Keyboard nav: an item in the nav window registers as a candidate and, if it is the nav
       cursor, takes a synthesized click from an Enter/Space activation -- the keyboard mirror of
       the mouse hit-test above, through the same one seam every widget already passes through. */
    if ( s_build.win_id == s_nav.win )
        nav_item_register( id, r, &st, kind );

    /* Auto-repeat (IMGUI_ITEM_BUTTON_REPEAT): while held with the cursor still over it, fire on the
       press then repeatedly on the timed cadence -- replacing the release-click for this widget.
       Gated on the cursor being over it so sliding off pauses the repeat, like a real spin button. */
    if ( ( s_build.cur_item_flags & IMGUI_ITEM_BUTTON_REPEAT ) && st.active && s_interaction.hover_id == id )
        st.clicked = widget_repeat_tick( st.pressed );

    /* Debug overlay: every interactive widget passes through here, so this one site captures
       the hit rects -- tinted by hover/active so the live interaction is visible.  Capture the
       *visible* rect (the widget clipped to the active region clip): a row scrolled fully
       outside its child box has an empty intersection and is not hit-testable, so it is dropped
       from the overlay too, rather than drawing an interaction rect outside the clip box. */
#ifdef IMGUI_DEBUG_OVERLAY
    {
        if ( eligible ) {
            imgui_rect_t vis = rect_intersect( r, s_build.clip_rect );
            if ( vis.w > 0.0f && vis.h > 0.0f )
                 DBG_WIDGET( id, vis, st.hover, st.active );
        }
    }
#endif

    s_build.last_item_status = st;   /* publish the resolved state for the item-query readers */
    return st;
}

/* Background color for a pushbutton / knob style widget, from its interaction state. */
static u32
widget_bg_color( widget_state_t st )
{
    if ( st.active )            return COL_WIDGET_ACT;
    if ( st.hover || st.nav )   return COL_WIDGET_HOT;   /* nav cursor lights the body like a hover */
    return COL_WIDGET_BG;
}

/* Frame-background tint for a "framed field" widget (checkbox box, slider track, drag box, input):
   the same hover / nav / active response as a button, but over a caller-supplied idle base so the
   field keeps its own resting colour.  Mouse hover and keyboard-nav highlight both light it -- one
   at a time, since they are mutually exclusive -- so it is clear what is under the cursor, matching
   how Dear ImGui's FrameBgHovered lifts every framed control, not just buttons. */
static u32
frame_bg_color( widget_state_t st, u32 idle )
{
    if ( st.active )            return COL_WIDGET_ACT;
    if ( st.hover || st.nav )   return COL_WIDGET_HOT;
    return idle;
}

// clang-format on
/*============================================================================================*/
