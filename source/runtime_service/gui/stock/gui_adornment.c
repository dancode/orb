/*==============================================================================================

    runtime_service/gui/stock/gui_adornment.c -- Per-item ambient application + the system
    adornments.

    Everything here is styled paint, or the application of style / draw state -- stock material
    by definition, since stock is the first layer astride both servers.  Three groups, in file
    order:

      - per-item ambient application (item_flags_resolve / item_flags_chrome_reset): the style
        and draw consequences over the interact server's pure seams (item_flags_take /
        item_flags_chrome_drop, core/gui_ctx.c);
      - the label paint (label_natural_w + gui_field_row): gui_field_row draws a labeled widget's
        own label per the ambient gui_field_t.  Its geometry half (field_geom_split) lives with
        the composer in flow -- the paint and the WIDGET_PAD self-measure are here;
      - the system adornments (nav ring, focus border, drop ring, child box, resize highlight):
        the units below decide WHEN one paints, across their documented upward seams
        (core/gui_core.h, interact/gui_interact.h, flow/gui_flow.h), and the paint POLICY --
        color, thickness, extent -- lives here with the rest of the skin.

    The style vocabulary this consumes (WIDGET_* / WIN_* / COL_*) and the state -> color
    projections (col_item_bg & co) both live with their resolver in the style unit; state -> color
    is resolution by nature, and all three take the interact state as a parameter.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Per-item ambient application -- the impure wrappers over the interact server's pure seams.

    The core unit resolves WHAT the flags are (item_flags_take / item_flags_chrome_drop,
    core/gui_ctx.c); these wrappers apply the style and draw consequences -- the per-item style
    commit, the disabled dim, the default rounding -- which the interact server itself must
    never touch.  Style owns the commit, stock the adornment.
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
    draw_set_rounding( style_var( GUI_VAR_PANEL_ROUND ) );
}

/*==============================================================================================
    Label paint -- the self-measure, and the one caption seam.
==============================================================================================*/

/* The natural width a label-sized widget requests from the composer: the visible span plus the
   standard inset on both sides.  THE self-measurement formula -- button, small_button, menu
   items, and the public gui_button_width all speak it through this one helper.  WIDGET_PAD
   is a style read, so it lives with the styled painters. */
f32  label_natural_w( const char* s )
{
    return label_width( s ) + 2.0f * WIDGET_PAD;
}

/* The caption label seam -- the ONE place a labeled value widget routes its own label through, so
   there is no second "_label" widget set: the label param a widget already carries becomes the
   label, and the ambient gui_field_t (gui_field_get) decides whether and where it renders.  Reads
   the ambient, consumes one WIDGET_H row cell, paints the label into its track, and arms
   next_item_rect with the control track so the widget's own cell_next fills it and keeps the hit --
   the label is passive text.  A widget calls it right before taking its cell:

       bool gui_slider_float( const char* label, f32* v, f32 lo, f32 hi )
       { gui_field_row( label ); ... id = item_id(label); cell = cell_next(WIDGET_H); ... }

   It emits nothing (consumes no cell, arms nothing -- the widget's cell_next then returns a full
   cell) in three cases, so one call handles labeled AND bare: field.hide (the master property-panel
   toggle), skip_label() armed for this one widget, or an empty ("##id") label.  The geometry half
   (field_geom_split) lives with the composer in flow; this owns the PAINT and the pen -- flow
   never colors a pixel. */

void
gui_field_row( const char* label )
{
    gui_field_t* fld   = gui_field_get();   /* the one field authority (set-once, like a style) */
    f32          vis_w = label_width( label );
    bool         skip  = field_skip_take();  /* always consume the one-shot, even when hidden */
    if ( fld->hide || skip || vis_w == 0.0f ) return;   /* bare: control fills its cell */

    gui_rect_t cell = cell_next( WIDGET_H );

    /* Natural label track = the measured width; an explicit field.label (LEFT/RIGHT column) wins. */
    f32 label_track = ( fld->side != GUI_LABEL_NONE && fld->label > 0.0f ) ? fld->label : vis_w;

    gui_rect_t label_r, control_r;
    field_geom_split( cell, (gui_label_side_t)fld->side, fld->control > 0.0f ? fld->control : 1.0f,
                      label_track, WIDGET_MIN_W, WIDGET_PAD, &label_r, &control_r );

    draw_label_fit( label_r.x, text_center_y( cell.y, cell.h ), COL_TEXT, label, label_r.w );
    gui_next_item_rect( control_r );
}

/*==============================================================================================
    System adornments -- the uniform highlight rings and edge markers the interaction services
    invoke.  Behavior (interact/) decides WHEN one paints (the protocol point); the paint
    policy -- color, thickness, extent -- lives here with the rest of the skin, so the behavior
    tier never reads a style value to adorn an item.
==============================================================================================*/

/* NAV_RING (the focus-ring inset) is declared in core/gui_core.h beside this painter's upward
   seam: the nav scroll chase (flow/gui_scroll.c) reads it to keep the ring clear of the view edge. */

/* Keyboard-nav focus ring: an outline just outside the item rect, painted before the item's
   own background so the fill leaves the border visible (nav_item_register invokes it across
   the core unit's one paint seam -- see the upward-seam block in core/gui_core.h).
   captured selects the ring color: plain nav-highlight (COL_NAV) vs. a value widget that has
   captured the keyboard for Left/Right editing (COL_CHECK_MARK) -- this is the one adornment
   every widget passes through, so it is the single place that makes "input captured" read as a
   real, theme-wide-consistent state change instead of looking identical to plain nav focus. */
void
draw_nav_ring( gui_rect_t r, bool captured )
{
    draw_push_rect_outline( r.x - NAV_RING, r.y - NAV_RING,
                            r.w + 2.0f * NAV_RING, r.h + 2.0f * NAV_RING,
                            WIN_BORDER, 0, captured ? COL_CHECK_MARK : COL_NAV );
}

/* Focused-window frame: a bolder, accent-coloured outline painted over the window's own border to
   mark the one window that currently holds keyboard focus.  window_end (free floats) and
   dock_window_chrome (docked / floating groups) invoke it after their base border, gated on
   s_build.win.id == g_ctx->nav.focused_win, so it inherits the ambient rounding that border used and
   traces the same corners.  Thickness 0 (a theme that disables it) draws nothing. */
void
draw_window_focus_border( gui_rect_t r )
{
    f32 t = WIN_BORDER;
    if ( t <= 0.0f ) return;
    draw_outline( r, t, COL_FOCUS_BORDER );
}

/* Drag-and-drop accept ring: a bolder outline around an open target whose type matched the
   payload, so the drop reads as "accepted here".  gui_drag_payload_accept (the interact unit)
   invokes it across that unit's one documented upward paint seam (interact/gui_interact.h) --
   the drop decision and its feedback land in the same call, like core's draw_nav_ring. */
void
draw_drop_ring( gui_rect_t r )
{
    draw_push_rect_outline( r.x - 2.0f, r.y - 2.0f, r.w + 4.0f, r.h + 4.0f, 2.0f, 0, COL_NAV );
}

/* Child box chrome (flow/gui_layout_child.c invokes these around its region): the body
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
