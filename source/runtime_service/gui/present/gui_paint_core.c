/*==============================================================================================

    runtime_service/gui/present/gui_paint_core.c -- Shared presentation primitives.

    The paint-facing foundation the widget layer draws through: the label grammar + content
    placement (rect_align / arrows), the text-fit helpers, the frame/background color policy,
    and the system adornments.  Both the leaf widgets (widgets/) and the window chrome
    (gui_window_free.c) draw through these, so they live here, ahead of both in the unity
    build.  The style vocabulary (WIDGET_* / WIN_* / COL_* macros) lives with its resolver in
    foundation/gui_style.c -- this file only consumes it.  The interaction state machine
    (widget_behavior) is a service -- it lives in interact/gui_item.c, included immediately
    after this file so it can invoke the adornment painters below.  The shared edge-resize
    geometry is interact/gui_resize.c and the layout engine (track resolver + cell emitters)
    is compose/gui_layout_core.c.

    Included by gui.c after foundation/gui_ctx.c + foundation/gui_io.c so s_interaction, s_build, s_io, s_style,
    rect_hit, and the draw helpers are all in scope.  Despite the name, this file has no
    dependency on gui_window.c -- window bookkeeping is a later, optional tier
    (window/gui_window.c).

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"   /* gui_widget_kind_t, gui_item_state_t */
// clang-format off

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
   through -- rect_align below for a box, and draw_text_in (gui_text.c) per line of a text
   block -- so a centered label, a right-flushed caption, and a bottom-anchored run share one rule. */
static f32
align_span( f32 org, f32 avail, f32 len, bool center, bool far )
{
    if ( center ) return org + ( avail - len ) * 0.5f;
    if ( far )    return org +   avail - len;
    return org;                                                   /* near edge -- LEFT / TOP default */
}

/* Horizontal / vertical placement within a cell span, reading the matching gui_align_t bits. */
static f32 align_x( f32 x, f32 w, f32 len, u32 a ) { return align_span( x, w, len, a & GUI_ALIGN_HCENTER, a & GUI_ALIGN_RIGHT  ); }
static f32 align_y( f32 y, f32 h, f32 len, u32 a ) { return align_span( y, h, len, a & GUI_ALIGN_VCENTER, a & GUI_ALIGN_BOTTOM ); }

/* Place a natural nat_w x nat_h box inside `cell` per the alignment flags (gui_align_t).  The
   single seam for positioning sub-cell content -- a button's label, a checkbox box, an aligned
   text run, a separator line -- so every widget edges / centers content the same way and a
   region's align setting flows through one place.  Returns the placed rect (w/h are nat_*).
   Thin alias for the public gui_rect_align (gui.h) so widgets and callers share one rule. */
static gui_rect_t
rect_align( gui_rect_t cell, f32 nat_w, f32 nat_h, u32 align )
{
    return gui_rect_align( cell, nat_w, nat_h, ( gui_align_t )align );
}

/* The symbol render primitives -- the glyph marks (arrow / collapse arrow / check / bullet / close /
   pointing arrow) and the broader shape palette (frames, rounded rects, polygons, arcs, curves,
   dashes, gradients, shadows, text effects, grips, spinners) -- live in gui_symbol.c,
   included later in the interleaved tier block (after interact/gui_item.c + gui_drag.c) so
   they may use the COL_* / ROUND_* /
   WIN_BORDER macros and col_lerp defined here, and so every widget file below resolves them by
   name.  The public gui_render_* surface over them is centralized there too. */

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
static gui_id_t
widget_id( const char* label )
{
    gui_id_t id = id_combine( id_seed(), id_hash( label_id_str( label ) ) );
    DBG_NAME( id, label );
    return id;
}

/* Width / draw of a label's visible span (markers stripped). */
static f32  label_width( const char* s )                         { return font_text_w_n( s, label_vis_len( s ) ); }
static void draw_label ( f32 x, f32 y, u32 c, const char* s )    { draw_push_text_n( x, y, c, s, label_vis_len( s ) ); }

/* The natural width a label-sized widget requests from the composer: the visible span plus the
   standard inset on both sides.  THE self-measurement formula -- button, small_button, menu
   items, and the public gui_button_width all speak it through this one helper. */
static f32  label_natural_w( const char* s )                     { return label_width( s ) + 2.0f * WIDGET_PAD; }

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

/* Split a labeled widget row into a control rect and its painted label.  The geometry halves
   live with the composer: field_split_resolve (compose/gui_layout_core.c, forward-declared
   here -- the one present->compose seam) lays the two tracks when a field split is active; the
   default trailing-label math is local.  This wrapper owns the PAINT: it draws the label and
   returns the control rect, which is why it sits here with the label grammar and not in the
   composer -- compose never colors a pixel.  In the default (trailing-label) mode the label
   keeps its natural width pinned at the row's right edge and the control takes the rest, never
   shrinking below min_control_w so it stays usable when the label is long (the control then
   overruns under the label); in field-split mode the label and control are two resolved tracks.
   The single seam every "control + trailing label" widget (slider_float, input_text, combo,
   drag_float, color_edit) routes through, so row proportions retune in one place. */

static bool field_split_resolve( gui_rect_t cell, f32 min_control_w, f32* out_label_x,
                                 f32* out_label_w, gui_rect_t* out_control );   /* compose */

static gui_rect_t
widget_split_label( gui_rect_t row, const char* label, f32 min_control_w, u32 label_color )
{
    /* Field split mode: the label sits in its track at full strength (the trailing-label dim hint,
       label_color, does not apply -- a field label reads as primary); the control fills the rest.
       The label is fitted to its track width so a narrow (fraction-shrunk) track ellipsizes it. */
    f32          label_x, label_w;
    gui_rect_t control;
    if ( field_split_resolve( row, min_control_w, &label_x, &label_w, &control ) )
    {
        draw_label_fit( label_x, text_center_y( row.y, row.h ), COL_TEXT, label, label_w );
        return control;
    }

    /* Default: control on the left, the label trailing at its natural width on the right.  When the
       control floors at min_control_w the label space narrows; fit it so it ellipsizes there
       instead of bleeding under the row's right edge.  No visible label ("##key") => full row. */
    label_w = label_width( label );
    if ( label_w == 0.0f ) return row;
    f32 control_w = row.w - label_w - WIDGET_PAD;
    if ( control_w < min_control_w ) control_w = min_control_w;

    control = ( gui_rect_t ){ row.x, row.y, control_w, row.h };

    f32 trail_x = control.x + control.w + WIDGET_PAD;
    draw_label_fit( trail_x, text_center_y( row.y, row.h ), label_color, label,
                    ( row.x + row.w ) - trail_x );
    return control;
}

/* Frame-background tint for a "framed field" widget (checkbox box, slider track, drag box, input):
   hover / nav / active lift it to the shared hot / active palette entries -- one at a time, since
   hover and nav-highlight are mutually exclusive -- over a caller-supplied idle_color_enum base 
   so each field keeps its own resting colour, matching how Dear ImGui's FrameBgHovered lifts every
   framed control, not just buttons. */

static u32
frame_bg_color( gui_item_state_t st, u32 idle_color_enum )
{
    if ( st.active )            return COL_WIDGET_ACT;
    if ( st.hover || st.nav )   return COL_WIDGET_HOT;   /* nav cursor lights the body like a hover */
    return idle_color_enum;
}

/* Background color for a pushbutton / knob style widget: frame_bg_color with the plain widget
   background as the idle base. */
static u32 widget_bg_color( gui_item_state_t st )
{
    return frame_bg_color( st, COL_WIDGET_BG );
}

/* Animated background for a pushbutton-like widget: widget_bg_color with the hover/active
   transitions smoothed through the animation service (interact/gui_anim.c).  Two damper channels --
   a hot layer (hover / nav focus) and an active layer -- run at their own rates through gui_anim_f32,
   then composite over the palette: BG -> HOT by the hot channel, then that -> ACT by the active one.
   The primitive owns all the storage, settle, and wants_redraw bookkeeping; an idle widget with no
   history reads both channels at 0 (no stamp) and lands on COL_WIDGET_BG. */

#define ANIM_TAG_HOT  0xA501u   /* id_combine salts: keep the two channels distinct from each other */
#define ANIM_TAG_ACT  0xA502u   /* and from all other per-widget state in the keyed pool */

static u32
widget_bg_color_anim( gui_id_t id, gui_item_state_t st )
{
    f32 hot = gui_anim_f32_from( id_combine( id, ANIM_TAG_HOT ), 0.0f, ( st.hover || st.nav ) ? 1.0f : 0.0f, 10.0f );
    f32 act = gui_anim_f32_from( id_combine( id, ANIM_TAG_ACT ), 0.0f, st.active              ? 1.0f : 0.0f, 20.0f );
    return col_lerp( col_lerp( COL_WIDGET_BG, COL_WIDGET_HOT, hot ), COL_WIDGET_ACT, act );
}

/*----------------------------------------------------------------------------------------------
    System adornments -- the uniform highlight rings and edge markers the interaction services
    invoke.  Behavior (interact/) decides WHEN one paints (the protocol point); the paint
    policy -- color, thickness, extent -- lives here with the rest of the skin, so the behavior
    tier never reads a style value to adorn an item.
----------------------------------------------------------------------------------------------*/

/* Focus-ring inset outside the item rect so the item's own fill spares it.  The nav scroll
   chase (interact/gui_item.c) also reads this to keep the ring clear of the view edge. */
#define NAV_RING 2.0f

/* Keyboard-nav focus ring: an outline just outside the item rect, painted before the item's
   own background so the fill leaves the border visible (nav_item_register invokes it). */
static void
draw_nav_ring( gui_rect_t r )
{
    draw_push_rect_outline( r.x - NAV_RING, r.y - NAV_RING,
                            r.w + 2.0f * NAV_RING, r.h + 2.0f * NAV_RING,
                            WIN_BORDER, 0, COL_NAV );
}

/* Focused-window frame: a bolder, accent-coloured outline painted over the window's own border to
   mark the one window that currently holds keyboard focus.  window_end (free floats) and
   dock_window_chrome (docked / floating groups) invoke it after their base border, gated on
   s_build.win.id == g_ctx->nav.focused_win, so it inherits the ambient rounding that border used and
   traces the same corners.  Thickness 0 (a theme that disables it) draws nothing. */
static void
draw_window_focus_border( gui_rect_t r )
{
    f32 t = WIN_FOCUS_BORDER;
    if ( t <= 0.0f ) return;
    draw_push_rect_outline( r.x, r.y, r.w, r.h, t, 0, COL_FOCUS_BORDER );
}

/* Drag-and-drop accept ring: a bolder outline around an open target whose type matched the
   payload, so the drop reads as "accepted here" (gui_drag_payload_accept invokes it). */
static void
draw_drop_ring( gui_rect_t r )
{
    draw_push_rect_outline( r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, 2.0f, 0, COL_NAV );
}

/* Child box chrome (compose/gui_layout_child.c invokes these around its region): the body
   fill under the region clips at child_begin, the border over the bar tracks at child_end. */
static void draw_child_bg    ( gui_rect_t r ) { draw_push_rect_filled ( r.x, r.y, r.w, r.h, 0,0,1,1, 0, COL_CHILD_BG ); }
static void draw_child_border( gui_rect_t r ) { draw_push_rect_outline( r.x, r.y, r.w, r.h, WIN_BORDER, 0, COL_BORDER ); }

/* Paint a bold line over each hot edge of an outline so it is obvious that the border is
   grabbable and which side will move.  Drawn just inside the rect, over the thin border.
   `edges` is the GUI_RESIZE_* mask from the edge-resize service (interact/gui_resize.c). */
static void
draw_resize_highlight( gui_rect_t r, u8 edges )
{
    const f32 t = WIN_BORDER * 2.0f + 1.0f;   /* bold relative to the 1px frame */

    if ( edges & GUI_RESIZE_L ) draw_push_rect_filled( r.x,             r.y,             t,   r.h, 0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & GUI_RESIZE_R ) draw_push_rect_filled( r.x + r.w - t,   r.y,             t,   r.h, 0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & GUI_RESIZE_T ) draw_push_rect_filled( r.x,             r.y,             r.w, t,   0,0,1,1, 0, COL_RESIZE_HOT );
    if ( edges & GUI_RESIZE_B ) draw_push_rect_filled( r.x,             r.y + r.h - t,   r.w, t,   0,0,1,1, 0, COL_RESIZE_HOT );
}

// clang-format on
/*============================================================================================*/
