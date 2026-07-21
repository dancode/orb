/*==============================================================================================

    runtime_service/gui/present/gui_paint_core.c -- Shared presentation primitives.

    What remains after the R1/R3 carves (GUI_SERVER_PLAN.md): the label GRAMMAR (the id half
    -- -> core in R4), the frame/background color policy (state -> color -- style unit
    material), and the system adornments + draw_field_label (styled painters -- -> element in
    R8).  The pure paint floor and text painters moved to draw/gui_paint.c; the placement
    math to rect/.  The style vocabulary (WIDGET_* / WIN_* / COL_* macros) lives with its
    resolver in style/gui_style.h -- this file only consumes it.
    
    The interaction state machine (item_state) is a service -- it lives in interact/gui_item.c,
    included immediately after this file so it can invoke the adornment painters below.

    The shared edge-resize geometry is interact/gui_resize.c and the layout engine
    (track resolver + cell emitters) is compose/gui_layout_core.c.

    Included by gui.c after core/gui_ctx.c + core/gui_io.c so s_interaction, s_build, s_io, s_style,
    rect_hit, and the draw helpers are all in scope.  Despite the name, this file has no
    dependency on gui_window.c -- window bookkeeping is a later, optional tier
    (window/gui_window.c).

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"   /* gui_item_kind_t, gui_item_state_t */
// clang-format off

/* text_center_y / draw_fill / draw_outline / the text-fit painters (draw_label, label_width,
   label_natural_w, draw_text_fit_n, draw_label_fit, the compact ellipsis) moved to
   draw/gui_paint.c -- the draw unit's paint floor (GUI_SERVER_PLAN.md R3). */

/* The symbol render primitives (the glyph marks + the broader shape palette) moved to
   draw/gui_symbol.c -- the draw unit (GUI_SERVER_PLAN.md R3). */

/*==============================================================================================

    Widget label grammar  (Dear ImGui style)

        "Text"        -> display "Text",  id = hash("Text")
        "Text##key"   -> display "Text",  id = hash("Text##key")   distinct ids, same visible text
        "pre###key"   -> display "pre",   id = hash("###key")      id ignores a dynamic prefix

    The visible span ends at the first "##".  A "###" additionally re-roots the id hash at that
    "###", so a label whose visible part changes every frame (a counter, a name) keeps one stable
    id.  Every labeled widget routes its display through label_width / draw_label and its id
    through item_id, so the grammar is honored uniformly in one place.

==============================================================================================*/

/* Visible byte count: up to the first "##" marker, or the whole string.  Non-static: a
   cross-unit seam (gui_internal.h) -- the element unit's el_button honors the same label
   grammar, so the rule stays authored in one place. */
u32
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
const char*
label_id_str( const char* s )
{
    for ( u32 i = 0; s[ i ]; ++i )
        if ( s[ i ] == '#' && s[ i + 1 ] == '#' && s[ i + 2 ] == '#' )    /* reads stop at NUL */
            return s + i;
    return s;
}

/* The id for a labeled widget: the active scope seed combined with the label's id key. */
gui_id_t
item_id( const char* label )
{
    gui_id_t id = id_combine( id_seed(), id_hash( label_id_str( label ) ) );
    DBG_NAME( id, label );
    return id;
}

/* Split a labeled widget row into a control rect and its painted label.  The geometry halves
   live with the composer: cell_split_field (compose/gui_layout_core.c, forward-declared
   here -- the one present->compose seam) lays the two tracks when a field split is active; the
   default trailing-label math is local.  This wrapper owns the PAINT: it draws the label and
   returns the control rect, which is why it sits here with the label grammar and not in the
   composer -- compose never colors a pixel.  In the default (trailing-label) mode the label
   keeps its natural width pinned at the row's right edge and the control takes the rest, never
   shrinking below min_control_w so it stays usable when the label is long (the control then
   overruns under the label); in field-split mode the label and control are two resolved tracks.
   The single seam every "control + trailing label" widget (slider_float, input_text, combo,
   drag_float, color_edit) routes through, so row proportions retune in one place. */

/* cell_split_field (compose, flow unit) is declared in gui_internal.h since the TU split. */

gui_rect_t
draw_field_label( gui_rect_t row, const char* label, f32 min_control_w, u32 label_color )
{
    /* Field split mode: the label sits in its track at full strength (the trailing-label dim hint,
       label_color, does not apply -- a field label reads as primary); the control fills the rest.
       The label is fitted to its track width so a narrow (fraction-shrunk) track ellipsizes it. */
    f32          label_x, label_w;
    gui_rect_t control;
    if ( cell_split_field( row, min_control_w, &label_x, &label_w, &control ) )
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

u32
col_frame_bg( gui_item_state_t st, u32 idle_color_enum )
{
    if ( st.active )            return COL_WIDGET_ACT;
    if ( st.hover || st.nav )   return COL_WIDGET_HOT;   /* nav cursor lights the body like a hover */
    return idle_color_enum;
}

/* Common case background color for a pushbutton / knob style widget.
   col_frame_bg with the plain widget background as the idle base. */
u32 col_item_bg( gui_item_state_t st )
{
    return col_frame_bg( st, COL_WIDGET_BG );
}

/* Animated background for a pushbutton-like widget: col_item_bg with the hover/active
   transitions smoothed through the animation service (interact/gui_anim.c).  Two damper channels in
   one gui_anim4 slot -- a hot layer (hover / nav focus) at speed 10 and an active layer (pressed) at
   speed 20 -- both rest at 0 so they ramp up from the palette base; the spare two channels sit unused
   (0/0/0) and are free for a widget-specific flourish later.  Composite over the palette: BG -> HOT by
   the hot channel, then that -> ACT by the active one.  The primitive owns all storage, settle, and
   wants_redraw bookkeeping in a single peek; an idle widget with no history lands on COL_WIDGET_BG. */

#define ANIM_TAG_BG  0xA501u   /* id_combine salt: keeps this slot distinct from all other per-widget state */

u32
col_item_bg_anim( gui_id_t id, gui_item_state_t st )
{
    gui_anim4_t rest   = { 0.0f, 0.0f, 0.0f, 0.0f };
    gui_anim4_t target = { ( st.hover || st.nav ) ? 1.0f : 0.0f, st.active ? 1.0f : 0.0f, 0.0f, 0.0f };
    gui_anim4_t speed  = { 10.0f, 20.0f, 0.0f, 0.0f };
    gui_anim4_t a      = gui_anim4( id_combine( id, ANIM_TAG_BG ), rest, target, speed );
    return col_lerp( col_lerp( COL_WIDGET_BG, COL_WIDGET_HOT, a.x ), COL_WIDGET_ACT, a.y );
}

/*==============================================================================================
    System adornments -- the uniform highlight rings and edge markers the interaction services
    invoke.  Behavior (interact/) decides WHEN one paints (the protocol point); the paint
    policy -- color, thickness, extent -- lives here with the rest of the skin, so the behavior
    tier never reads a style value to adorn an item.
==============================================================================================*/

/* Focus-ring inset outside the item rect so the item's own fill spares it.  The nav scroll
   chase (interact/gui_item.c) also reads this to keep the ring clear of the view edge. */
#define NAV_RING 2.0f

/* Keyboard-nav focus ring: an outline just outside the item rect, painted before the item's
   own background so the fill leaves the border visible (nav_item_register invokes it).
   captured selects the ring color: plain nav-highlight (COL_NAV) vs. a value widget that has
   captured the keyboard for Left/Right editing (COL_NAV_CAPTURE) -- this is the one adornment
   every widget passes through, so it is the single place that makes "input captured" read as a
   real, theme-wide-consistent state change instead of looking identical to plain nav focus. */
static void
draw_nav_ring( gui_rect_t r, bool captured )
{
    draw_push_rect_outline( r.x - NAV_RING, r.y - NAV_RING,
                            r.w + 2.0f * NAV_RING, r.h + 2.0f * NAV_RING,
                            WIN_BORDER, 0, captured ? COL_NAV_CAPTURE : COL_NAV );
}

/* Focused-window frame: a bolder, accent-coloured outline painted over the window's own border to
   mark the one window that currently holds keyboard focus.  window_end (free floats) and
   dock_window_chrome (docked / floating groups) invoke it after their base border, gated on
   s_build.win.id == g_ctx->nav.focused_win, so it inherits the ambient rounding that border used and
   traces the same corners.  Thickness 0 (a theme that disables it) draws nothing. */
void
draw_window_focus_border( gui_rect_t r )
{
    f32 t = WIN_FOCUS_BORDER;
    if ( t <= 0.0f ) return;
    draw_outline( r, t, COL_FOCUS_BORDER );
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
void draw_child_bg    ( gui_rect_t r ) { draw_fill   ( r, COL_CHILD_BG ); }
void draw_child_border( gui_rect_t r ) { draw_outline( r, WIN_BORDER, COL_BORDER ); }

/* Paint a bold line over each hot edge of an outline so it is obvious that the border is
   grabbable and which side will move.  Drawn just inside the rect, over the thin border.
   `edges` is the GUI_RESIZE_* mask from the edge-resize service (interact/gui_resize.c). */
void
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
