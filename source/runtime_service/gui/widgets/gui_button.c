/*==============================================================================================

    runtime_service/gui/widgets/gui_button.c -- Press widgets.

    The controls whose story is a press: button / button_fill / small_button / arrow_button,
    checkbox and radio_button, and selectable (the list-row
    press).  Each takes its rect from cell_next and runs the standard item protocol
    (item_state, interact/gui_item.c) as ITEM_BUTTON, then paints its face with
    the present/ helpers -- the canonical compose -> behave -> present combine.

    Display-only rows (text, bullets, label_text, progress_bar, spacers) are in gui_text.c,
    included just before this file; folding rows in gui_tree.c and text fields in gui_input.c,
    after it.  Slider / drag widgets are gui_widget_slider.c, numeric inputs
    gui_widget_numeric.c.

==============================================================================================*/
// clang-format off

/* Shared button-face label draw: centered when it fits the frame, else a left-anchored
   ellipsized fit so an oversized label truncates cleanly instead of spilling past both edges.
   Every pushbutton-style widget (button, button_fill, small_button) draws its face this way --
   centered is a button's one layout difference from the trailing-label widgets below, which is
   why it is not routed through draw_field_label / rect_align's LEFT default like they are. */

static void
draw_button_label( gui_rect_t r, const char* label )
{
    f32 lw = label_width( label );
    f32 avail = r.w - 2.0f * WIDGET_PAD;
    if ( lw <= avail )
    {
        gui_rect_t lr = rect_align( r, lw, font_char_h(), GUI_ALIGN_CENTER );
        draw_label( lr.x, lr.y, COL_TEXT, label );
    }
    else
    {
        draw_label_fit( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label, avail );
    }
}

/*==============================================================================================

    button -- returns true on the frame the button is released while hovered

==============================================================================================*/

bool
gui_button( const char* label )
{
    /* Get this widgets unique identifier */
    gui_id_t id = item_id( label );

    /* Natural width = a react with room for label + padding.  
       Shrinks to this in stack and in same_line; Columns fills to fit. */
    gui_rect_t r  = cell_next_w( label_natural_w( label ), WIDGET_H );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    draw_fill( r, col_item_bg_anim( id, st ) );
    draw_button_label( r, label );

    return st.clicked;
}

/*==============================================================================================
    button_fill -- a button that fills the remaining height of its containing region.

    Identical to button() except the painted height grows to content_avail().y instead of the
    fixed WIDGET_H.  Intended as a terminal / slack-absorbing element (the tall side of a split
    panel, a lone action row that should reach the region floor).

    Layout footprint vs paint: a fill's remaining-height is measured in scroll-biased space against
    the fixed region bottom, so counting the filled height toward the region's content extent makes
    it feed back -- scroll down, the fill grows, content_h grows, the scroll range grows, forever.
    So the fill occupies only ONE ROW (WIDGET_H) in the layout flow -- that is all it contributes to
    high_y / the auto-size + scroll extent -- and merely PAINTS (and hit-tests) over the taller
    box.  Net: a fill can never push content past the region; content size stays bounded by the
    window, and the slack is what the fill expands into.  A widget emitted after a fill overlaps its
    paint (the fill occupies min in the flow) -- fills are meant to be terminal.
==============================================================================================*/

bool
gui_button_fill( const char* label )
{
    gui_id_t id = item_id( label );

    f32 avh = gui_content_avail().y;   /* to the region floor: bottom margin is the region's pad.b */
    if ( avh < WIDGET_H ) avh = WIDGET_H;

    gui_rect_t r = cell_next( WIDGET_H );   /* flow + extent contribution = one row */
    r.h          = avh;                            /* paint + hit-test over the filled height */

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    draw_fill( r, col_item_bg_anim( id, st ) );
    draw_button_label( r, label );

    return st.clicked;
}

/*==============================================================================================
    small_button -- a compact button with no vertical frame padding (the ImGui SmallButton): a
    text-height row instead of the full WIDGET_H, for inline controls packed onto a text line.
==============================================================================================*/

bool
gui_small_button( const char* label )
{
    gui_id_t   id = item_id( label );

    /* Height hugs the glyph (plus 2px so the frame does not touch the text); width is label + pad. */
    f32        h  = font_char_h() + 2.0f;
    gui_rect_t r  = cell_next_w( label_natural_w( label ), h );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    draw_fill( r, col_item_bg( st ) );
    draw_button_label( r, label );

    return st.clicked;
}

/* button_width -- the natural width button() would use for `label`, for callers that need to
   lay out around a button before emitting it. */
f32
gui_button_width( const char* label )
{
    return label_natural_w( label );
}

/*==============================================================================================
    arrow_button -- a small square button with a directional triangle instead of a text label.

    The non-text button: same interaction and framed background as button(), but it draws an arrow
    pointing `dir` and sizes to a square row-height cell (so a same_line pair sits snug, the spinner
    layout).  Pass a label for the id only -- "##left" / "##right" -- since nothing is displayed.
    Pairs naturally with the GUI_ITEM_BUTTON_REPEAT flag for press-and-hold stepping:

        gui()->push_item_flag( GUI_ITEM_BUTTON_REPEAT, true );
        if ( gui()->arrow_button( "##left",  GUI_DIR_LEFT  ) ) counter--;
        gui()->same_line( spacing );
        if ( gui()->arrow_button( "##right", GUI_DIR_RIGHT ) ) counter++;
        gui()->pop_item_flag();
==============================================================================================*/

bool
gui_arrow_button( const char* label, gui_dir_t dir )
{
    gui_id_t   id = item_id( label );

    /* Square natural size (row height), so a same_line row of arrows packs tightly. */
    gui_rect_t r  = cell_next_w( WIDGET_H, WIDGET_H );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );

    draw_fill( r, col_item_bg( st ) );
    draw_arrow( r, dir, COL_TEXT );

    return st.clicked;
}

/*==============================================================================================
    checkbox / radio_button -- an indicator box + a trailing label.

    Both share one cell recipe (natural width, field-split resolve, item protocol) and differ only
    in what they paint into the CHECKBOX_SZ indicator box: a square + check, or a disc.  That recipe
    -- including a subtle nav-rect alignment rule -- lives once in checkable_cell so the two widgets
    stay in step.
==============================================================================================*/

typedef struct
{
    gui_item_state_t st;
    gui_rect_t       box;                 /* CHECKBOX_SZ indicator box (paint the mark/disc here) */
    f32              label_x, label_w;    /* trailing label placement */
    f32              label_y;             /* label baseline (text_center_y of the row) */
} checkable_cell_t;

static checkable_cell_t
checkable_cell( gui_id_t id, const char* label )
{
    /* The box (CHECKBOX_SZ) is shorter than the row (WIDGET_H), so centering it vertically already
       gives it a free top/bottom margin -- side_pad matches that same margin on the left/right so
       the item rect (and the nav ring / hit-test that key off it) pads the box evenly on all four
       sides instead of hugging it flush left and at the label's last glyph. */
    f32 side_pad = ( WIDGET_H - CHECKBOX_SZ ) * 0.5f;

    /* Natural width = pad + box + gap + label + pad, so a same_line control shrinks to fit.
       Under a field split the cell must NOT shrink: the split resolves its label + control tracks
       over the cell, so a hugged cell collapses the control track to the CHECKBOX_SZ floor and the
       nav/hit rect shrinks to the bare box instead of spanning the field like input/slider rows. */
    bool       split_on = ( lf()->mod.field_side != 0 );
    gui_rect_t r  = cell_next_w( split_on ? -1.0f
                                                 : 2.0f * side_pad + CHECKBOX_SZ + WIDGET_PAD
                                                       + label_width( label ),
                                        WIDGET_H );

    /* Field split mode aligns with the other labeled widgets: the label takes its track and the
       box sits at the start of the control track.  Default mode keeps the box on the left with the
       label trailing it.  The box only needs CHECKBOX_SZ of the control track.

       Resolved BEFORE item_state so the hit/nav rect can match input_text / slider_float: those
       route through draw_field_label, which hands item_state the control track alone, starting
       after the label gutter.  Handing item_state the full (label-gutter-including, left-seated)
       cell here instead would put this widget's nav rect at a different X than every other field in
       the same form -- e.g. a checkbox at column 0 next to input/slider fields starting at column 90 --
       so directional nav (which keys off cross-axis overlap) would treat them as different columns. */
    checkable_cell_t c;
    gui_rect_t       control;
    f32              bx;
    bool             split = cell_split_field( r, CHECKBOX_SZ, &c.label_x, &c.label_w, &control );

    if ( split )
    {
        bx = control.x;
    }
    else
    {
        bx         = r.x + side_pad;
        c.label_x  = bx + CHECKBOX_SZ + WIDGET_PAD;         /* default: label just right of the box */
        c.label_w  = ( r.x + r.w - side_pad ) - c.label_x;  /* trails to the cell's right edge      */
    }

    c.st      = item_state( id, split ? control : r, ITEM_BUTTON );
    f32 by    = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, GUI_ALIGN_VCENTER ).y;
    c.box     = ( gui_rect_t ){ bx, by, CHECKBOX_SZ, CHECKBOX_SZ };
    c.label_y = text_center_y( r.y, r.h );
    return c;
}

bool
gui_checkbox( const char* label, bool* v )
{
    gui_id_t         id = item_id( label );
    checkable_cell_t c  = checkable_cell( id, label );

    draw_fill( c.box, col_item_bg( c.st ) );
    draw_outline( c.box, WIN_BORDER, COL_BORDER );

    if ( *v )
    {
        /* Indicator: a 'v' tick (default), a filled disc, or an 'X' cross per GUI_VAR_CHECK_STYLE. */
        draw_check_indicator( c.box, COL_CHECK_MARK );
    }

    /* Draw the label plainly -- no ellipsis (markers still stripped); a label too wide for its
       track overflows and is bounded by the window clip, matching text() and the input widgets. */
    draw_label( c.label_x, c.label_y, COL_TEXT, label );

    bool changed = false;
    if ( c.st.clicked )
    {
        *v      = !( *v );
        changed = true;
        /* The indicator above drew the OLD *v; the new state shows on next frame's emit.  Force
           that frame -- an isolated toggle changes no other UI, so nothing else would mark the
           retained cache dirty and the check would not appear until the next input event. */
        g_ctx->retained.wants_redraw = true;
    }
    return changed;
}

/*==============================================================================================
    radio_button -- one option of a mutually-exclusive set.  `v` holds the selected value and
    `value` is the one this button stands for: the button shows "on" while *v == value, and a click
    sets *v = value.  Emit several against the same v (commonly with same_line between them) to form
    a radio group; returns true only on the frame a click changes the selection.

        static i32 e = 0;
        gui()->radio_button( "a", &e, 0 ); gui()->same_line( -1 );
        gui()->radio_button( "b", &e, 1 ); gui()->same_line( -1 );
        gui()->radio_button( "c", &e, 2 );

    The round sibling of checkbox: the same cell split (indicator in the control track, label
    trailing) and the same natural width, but a disc indicator -- a border ring, a hover-tinted
    well, and a filled centre dot when selected -- instead of the square box + check. */

bool
gui_radio_button( const char* label, i32* v, i32 value )
{
    gui_id_t         id = item_id( label );
    checkable_cell_t c  = checkable_cell( id, label );

    /* Disc centred in the CHECKBOX_SZ indicator box. */
    f32 cx  = c.box.x + CHECKBOX_SZ * 0.5f;
    f32 cy  = c.box.y + CHECKBOX_SZ * 0.5f;
    f32 rad = CHECKBOX_SZ * 0.5f;

    const u32 segs = 16;   /* facets -- round at widget sizes */
    bool      on   = ( v && *v == value );

    /* Border ring, then the well (hover/active tinted like a button knob), then the selected dot. */
    draw_push_circle_filled( cx, cy, rad,              segs, COL_BORDER );
    draw_push_circle_filled( cx, cy, rad - WIN_BORDER, segs, col_item_bg( c.st ) );
    if ( on )
        draw_push_circle_filled( cx, cy, rad - CHECK_PAD, segs, COL_CHECK_MARK );

    draw_label_fit( c.label_x, c.label_y, COL_TEXT, label, c.label_w );

    bool changed = false;
    if ( c.st.clicked && v && *v != value )
    {
        *v      = value;
        changed = true;
        /* Same one-frame-late draw as checkbox: the dot above drew the OLD selection.  Force the
           next frame so the moved selection shows without waiting on another input event. */
        g_ctx->retained.wants_redraw = true;
    }
    return changed;
}

/*==============================================================================================
    selectable -- a full-width row that highlights on hover and fills when selected.

    The building block for list boxes: emit one per item (typically inside a child_begin
    region so they scroll and clip independently).  When `selected` is non-NULL a click
    toggles it; pass NULL for a click-only row.  Returns true on the frame it is clicked, so
    a caller managing single-selection can set its own index from the return without relying
    on the toggle.
==============================================================================================*/

bool
gui_selectable( const char* label, bool* selected )
{
    gui_id_t   id = item_id( label );
    gui_rect_t r  = cell_next( WIDGET_H );

    gui_item_state_t st = item_state( id, r, ITEM_BUTTON );
    nav_item_stamp_label( id, label );   /* type-ahead opt-in (GUI_ITEM_NO_TYPEAHEAD to skip) */

    /* Fill: selected rows use the active tint, a hovered row the hot tint; otherwise the row
       is transparent so the region background shows through. */
    bool on = ( selected && *selected );
    if ( on || st.hover || st.nav )
        draw_fill( r, on ? COL_WIDGET_ACT : COL_WIDGET_HOT );

    /* Label, left-aligned with the standard padding. */
    draw_label( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label );
    cell_reach( r.x + WIDGET_PAD + label_width( label ) );   /* natural width may exceed the row */

    if ( st.clicked && selected )
        *selected = !( *selected );

    if ( st.clicked )
    {
        /* Same one-frame-late fix as checkbox/radio_button: a click here almost always drives a
           caller-owned selection (this row, a picked index) that is not visible until the NEXT
           frame's emit -- often because the content it selects was already built earlier in this
           same frame.  Force that frame so the new selection shows without waiting on more input. */
        g_ctx->retained.wants_redraw = true;

        /* Inside a combo dropdown a clicked row dismisses the combo: flag it for combo_end to close
           (the popup machinery is not in scope here).  Inert for an ordinary list selectable. */
        if ( s_build.combo_open )
            s_build.combo_item_clicked = true;

        /* Close the enclosing popup on click (Dear ImGui default behavior).
           Suppressed by GUI_ITEM_NO_CLOSE_POPUP for callers that need the popup to stay open
           (e.g. a multi-select list inside a persistent popup). */
        if ( s_popup_begin_count > 0
             && !( s_scope.flags & GUI_ITEM_NO_CLOSE_POPUP ) )
            gui_popup_close_current();
    }

    return st.clicked;
}

// clang-format on
/*============================================================================================*/
