/*==============================================================================================

    runtime_service/gui/present/gui_paint_core.c -- Shared presentation primitives.

    What remains after the R1/R3/R4/R5 carves (GUI_SERVER_PLAN.md): the impure per-item
    wrappers (item_flags_resolve / item_flags_chrome_reset -- style/draw application over
    core's pure seams) and the system adornments + draw_field_label (styled painters --
    -> element in R8).  The pure paint floor and text painters moved to draw/gui_paint.c; the
    placement math to rect/; the label grammar's id half to core/gui_id.c; the state -> color
    projections to the style unit (style/gui_style_core.c, R5).  The style vocabulary
    (WIDGET_* / WIN_* / COL_* macros) lives with its resolver in style/gui_style.h -- this
    file only consumes it.

    The interaction state machine (item_state) is a service -- it lives in core/gui_item.c
    (the interact server unit) and invokes the adornment painter below across the one
    documented upward seam (draw_nav_ring, core/gui_core.h).

    The shared edge-resize geometry is interact/gui_resize.c and the layout engine
    (track resolver + cell emitters) is compose/gui_layout_core.c.

    Included by gui.c (the frame unit); the ambient records (s_build, s_io) and s_style reach
    it through the per-unit header externs, the style seams through style/gui_style.h.
    Despite the name, this file has no dependency on gui_window.c -- window bookkeeping is a
    later, optional tier (window/gui_window.c).

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"   /* gui_item_kind_t, gui_item_state_t */
// clang-format off

/* text_center_y / draw_fill / draw_outline / the text-fit painters (draw_label, label_width,
   label_natural_w, draw_text_fit_n, draw_label_fit, the compact ellipsis) moved to
   draw/gui_paint.c -- the draw unit's paint floor (GUI_SERVER_PLAN.md R3). */

/* The symbol render primitives (the glyph marks + the broader shape palette) moved to
   draw/gui_symbol.c -- the draw unit (GUI_SERVER_PLAN.md R3). */

/* The widget label GRAMMAR (label_vis_len, label_id_str, item_id) moved to core/gui_id.c at
   the R4 carve: the id half of a label is identity derivation, interact-server material.  The
   PAINT half of a labeled row (draw_field_label below, label_width / draw_label in draw/)
   stays presentation. */

/*==============================================================================================
    Per-item ambient application -- the impure wrappers over the interact server's pure seams.

    The core unit resolves WHAT the flags are (item_flags_take / item_flags_chrome_drop,
    core/gui_ctx.c); these wrappers apply the style and draw consequences -- the per-item style
    commit, the disabled dim, the default rounding -- which the interact server itself must
    never touch.  Callers (the cell emit seam, the chrome seams, the pane bracket) keep the
    original names.  Placement refined at R5/R8 (style owns the commit, element the adornment).
==============================================================================================*/

/* Disabled items draw at this opacity (the rest of the dim is in the draw list's global alpha). */
#define GUI_DISABLED_ALPHA 0.5f

/* Resolve the flags for the item now emitting, then apply their ambient consequences.  Called
   once per item from cell_next_w (the universal emit seam). */
gui_item_flags_t
item_flags_resolve( void )
{
    gui_item_flags_t f = item_flags_take();

    /* Same seam for the style stacks: promote any next_style_* override into the active per-item
       layer so it applies for this widget's whole draw, then clears for the following one. */
    style_item_commit();

    draw_set_alpha( ( f & GUI_ITEM_DISABLED ) ? GUI_DISABLED_ALPHA : 1.0f );
    /* Default this widget's rects to the control-frame radius (base + push/pop + next-* override).
       A widget that draws a grab (slider knob, scrollbar) or a squared-off mark (check, bullet)
       overrides draw_set_rounding locally for that sub-element. */
    draw_set_rounding( ROUND_WIDGET );
    return f;
}

/* Clear the per-item state before chrome runs.  Window/child borders, scrollbars, titlebars, and
   the collapse arrow are not items -- they never go through cell_next_w, so without this they
   would inherit whatever the last body widget latched (a disabled trailing widget would dim the
   border and deaden the scrollbar).  Called at the chrome seams (begin/window_end, child_begin,
   layout_pop_region) so chrome always interacts undisabled and paints opaque. */
void
item_flags_chrome_reset( void )
{
    item_flags_chrome_drop();
    draw_set_alpha( 1.0f );
    style_chrome_reset();   /* drop lingering next_style_* overrides; keep the push/pop stack */
    /* Chrome (window / child / dock backgrounds, title bars, borders) defaults to the window radius,
       read after the item override is cleared so a trailing widget's next-* radius cannot leak in. */
    draw_set_rounding( style_var( GUI_VAR_WIN_ROUNDING ) );
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

/* The state -> color projections (col_frame_bg, col_item_bg, col_item_bg_anim) moved to the
   style unit at the R5 carve (style/gui_style_core.c): state -> color is style resolution by
   nature, and all three take the interact state as a parameter (the R5 purity rule). */

/*==============================================================================================
    System adornments -- the uniform highlight rings and edge markers the interaction services
    invoke.  Behavior (interact/) decides WHEN one paints (the protocol point); the paint
    policy -- color, thickness, extent -- lives here with the rest of the skin, so the behavior
    tier never reads a style value to adorn an item.
==============================================================================================*/

/* NAV_RING (the focus-ring inset) is declared in core/gui_core.h beside this painter's upward
   seam: the nav scroll chase (core/gui_item.c) reads it to keep the ring clear of the view edge. */

/* Keyboard-nav focus ring: an outline just outside the item rect, painted before the item's
   own background so the fill leaves the border visible (nav_item_register invokes it across
   the core unit's one paint seam -- see the upward-seam block in core/gui_core.h).
   captured selects the ring color: plain nav-highlight (COL_NAV) vs. a value widget that has
   captured the keyboard for Left/Right editing (COL_NAV_CAPTURE) -- this is the one adornment
   every widget passes through, so it is the single place that makes "input captured" read as a
   real, theme-wide-consistent state change instead of looking identical to plain nav focus. */
void
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
