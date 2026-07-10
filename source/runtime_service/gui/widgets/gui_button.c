/*==============================================================================================

    runtime_service/gui/widgets/gui_button.c -- Press widgets.

    The controls whose story is a press: button / button_fill / small_button / arrow_button,
    checkbox and radio_button, and selectable (the list-row
    press).  Each takes its rect from widget_next_rect and runs the standard item protocol
    (widget_behavior, interact/gui_item.c) as GUI_WIDGET_KIND_BUTTON, then paints its face with
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
   why it is not routed through widget_split_label / rect_align's LEFT default like they are. */
static void
draw_button_label( gui_rect_t r, const char* label )
{
    f32 lw    = label_width( label );
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
    gui_id_t   id = widget_id( label );

    /* Natural width = label + padding.  Shrinks to this in stack and same_line; fills in columns. */
    gui_rect_t r  = widget_next_rect_w( label_natural_w( label ), WIDGET_H );

    gui_item_state_t st = widget_behavior( id, r, GUI_WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color_anim( id, st ) );
    draw_button_label( r, label );

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    button_fill -- a button that fills the remaining height of its containing region.

    Identical to button() in every respect except the height comes from content_avail().y
    instead of the fixed WIDGET_H.  Intended for use inside a split panel where you want the
    button to match the height of the adjacent panel's content.
----------------------------------------------------------------------------------------------*/

bool
gui_button_fill( const char* label )
{
    gui_id_t id = widget_id( label );

    f32 avh = gui_content_avail().y;
    if ( avh < WIDGET_H ) avh = WIDGET_H;

    gui_rect_t r = widget_next_rect( avh );   /* fill the cell; height from content_avail */

    gui_item_state_t st = widget_behavior( id, r, GUI_WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color_anim( id, st ) );
    draw_button_label( r, label );

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    small_button -- a compact button with no vertical frame padding (the ImGui SmallButton): a
    text-height row instead of the full WIDGET_H, for inline controls packed onto a text line.
----------------------------------------------------------------------------------------------*/

bool
gui_small_button( const char* label )
{
    gui_id_t   id = widget_id( label );

    /* Height hugs the glyph (plus 2px so the frame does not touch the text); width is label + pad. */
    f32          h  = font_char_h() + 2.0f;
    gui_rect_t r  = widget_next_rect_w( label_natural_w( label ), h );

    gui_item_state_t st = widget_behavior( id, r, GUI_WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );
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

/*----------------------------------------------------------------------------------------------
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
----------------------------------------------------------------------------------------------*/

bool
gui_arrow_button( const char* label, gui_dir_t dir )
{
    gui_id_t   id = widget_id( label );

    /* Square natural size (row height), so a same_line row of arrows packs tightly. */
    gui_rect_t r  = widget_next_rect_w( WIDGET_H, WIDGET_H );

    gui_item_state_t st = widget_behavior( id, r, GUI_WIDGET_KIND_BUTTON );

    draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0, widget_bg_color( st ) );
    draw_arrow( r, dir, COL_TEXT );

    return st.clicked;
}

/*----------------------------------------------------------------------------------------------
    checkbox -- returns true when the value toggles
----------------------------------------------------------------------------------------------*/

bool
gui_checkbox( const char* label, bool* v )
{
    gui_id_t   id = widget_id( label );

    /* The box (CHECKBOX_SZ) is shorter than the row (WIDGET_H), so centering it vertically already
       gives it a free top/bottom margin -- side_pad matches that same margin on the left/right so
       the item rect (and the nav ring / hit-test that key off it) pads the box evenly on all four
       sides instead of hugging it flush left and at the label's last glyph. */
    f32 side_pad = ( WIDGET_H - CHECKBOX_SZ ) * 0.5f;

    /* Natural width = pad + box + gap + label + pad, so a same_line checkbox shrinks to fit.
       Under a field split the cell must NOT shrink: the split resolves its label + control tracks
       over the cell, so a hugged cell collapses the control track to the CHECKBOX_SZ floor and the
       nav/hit rect shrinks to the bare box instead of spanning the field like input/slider rows. */
    bool       split_on = ( lf()->mod.field_side != 0 );
    gui_rect_t r  = widget_next_rect_w( split_on ? -1.0f
                                                 : 2.0f * side_pad + CHECKBOX_SZ + WIDGET_PAD
                                                       + label_width( label ),
                                        WIDGET_H );

    /* Field split mode aligns with the other labeled widgets: the label takes its track and the
       box sits at the start of the control track.  Default mode keeps the box on the left with the
       label trailing it.  The box only needs CHECKBOX_SZ of the control track.

       Resolved BEFORE widget_behavior so the hit/nav rect can match input_text / slider_float: those
       route through widget_split_label, which hands widget_behavior the control track alone, starting
       after the label gutter.  Handing widget_behavior the full (label-gutter-including, left-seated)
       cell here instead would put this widget's nav rect at a different X than every other field in
       the same form -- e.g. a checkbox at column 0 next to input/slider fields starting at column 90 --
       so directional nav (which keys off cross-axis overlap) would treat them as different columns. */

    f32         label_x, label_w;
    gui_rect_t  control;
    f32         bx;
    bool        split = field_split_resolve( r, CHECKBOX_SZ, &label_x, &label_w, &control );

    if ( split )
    {
        bx = control.x;
    }
    else
    {
        bx      = r.x + side_pad;
        label_x = bx + CHECKBOX_SZ + WIDGET_PAD;         /* default: label just right of the box */
        label_w = ( r.x + r.w - side_pad ) - label_x;    /* trails to the cell's right edge      */
    }

    gui_item_state_t st = widget_behavior( id, split ? control : r, GUI_WIDGET_KIND_BUTTON );

    f32 by = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, GUI_ALIGN_VCENTER ).y;
    draw_push_rect_filled( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, 0,0,1,1, 0, widget_bg_color( st ) );
    draw_push_rect_outline( bx, by, CHECKBOX_SZ, CHECKBOX_SZ, WIN_BORDER, 0, COL_BORDER );

    if ( *v )
    {
        /* Indicator: a 'v' tick (default), a filled disc, or an 'X' cross per GUI_VAR_CHECK_STYLE. */
        draw_check_indicator( ( gui_rect_t ){ bx, by, CHECKBOX_SZ, CHECKBOX_SZ }, COL_CHECK_MARK );
    }

    /* Draw the label plainly -- no ellipsis (markers still stripped); a label too wide for its
       track overflows and is bounded by the window clip, matching text() and the input widgets. */
    (void)label_w;
    draw_label( label_x, text_center_y( r.y, r.h ), COL_TEXT, label );

    bool changed = false;
    if ( st.clicked )
    {
        *v    = !( *v );
        changed = true;
        /* The indicator above drew the OLD *v; the new state shows on next frame's emit.  Force
           that frame -- an isolated toggle changes no other UI, so nothing else would mark the
           retained cache dirty and the check would not appear until the next input event. */
        g_ctx->retained.wants_redraw = true;
    }
    return changed;
}

/*----------------------------------------------------------------------------------------------
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
    gui_id_t   id = widget_id( label );

    /* Same flush-vs-padded fix as checkbox: side_pad matches the free top/bottom margin the disc
       already gets from being centred in the row, so the item rect pads it evenly on all sides. */
    f32 side_pad = ( WIDGET_H - CHECKBOX_SZ ) * 0.5f;

    /* Natural width = pad + disc + gap + label + pad, so a same_line radio shrinks to fit (a group
       on one row).  Under a field split the cell must not shrink -- same rule as checkbox: the
       split needs the full cell or its control track collapses to the floor. */
    bool       split_on = ( lf()->mod.field_side != 0 );
    gui_rect_t r  = widget_next_rect_w( split_on ? -1.0f
                                                 : 2.0f * side_pad + CHECKBOX_SZ + WIDGET_PAD
                                                       + label_width( label ),
                                        WIDGET_H );

    /* Same label/control split as checkbox, resolved BEFORE widget_behavior for the same reason: so
       the hit/nav rect starts at the control track (matching input_text / slider_float) instead of
       the cell's left edge, which would otherwise put this widget's nav rect at a different X than
       the other fields in the same form. */
    f32          label_x, label_w;
    gui_rect_t control;
    f32          bx;
    bool         split = field_split_resolve( r, CHECKBOX_SZ, &label_x, &label_w, &control );

    if ( split )
    {
        bx = control.x;
    }
    else
    {
        bx      = r.x + side_pad;
        label_x = bx + CHECKBOX_SZ + WIDGET_PAD;         /* default: label just right of the disc */
        label_w = ( r.x + r.w - side_pad ) - label_x;    /* trails to the cell's right edge        */
    }

    gui_item_state_t st = widget_behavior( id, split ? control : r, GUI_WIDGET_KIND_BUTTON );

    /* Disc centred in a CHECKBOX_SZ box, vertically centred in the row. */
    f32 by  = rect_align( r, CHECKBOX_SZ, CHECKBOX_SZ, GUI_ALIGN_VCENTER ).y;
    f32 cx  = bx + CHECKBOX_SZ * 0.5f;
    f32 cy  = by + CHECKBOX_SZ * 0.5f;
    f32 rad = CHECKBOX_SZ * 0.5f;

    const u32 segs = 16;   /* facets -- round at widget sizes */
    bool      on   = ( v && *v == value );

    /* Border ring, then the well (hover/active tinted like a button knob), then the selected dot. */
    draw_push_circle_filled( cx, cy, rad,              segs, COL_BORDER );
    draw_push_circle_filled( cx, cy, rad - WIN_BORDER, segs, widget_bg_color( st ) );
    if ( on )
        draw_push_circle_filled( cx, cy, rad - (f32)s_style.checkmark_pad, segs, COL_CHECK_MARK );

    draw_label_fit( label_x, text_center_y( r.y, r.h ), COL_TEXT, label, label_w );

    bool changed = false;
    if ( st.clicked && v && *v != value )
    {
        *v      = value;
        changed = true;
        /* Same one-frame-late draw as checkbox: the dot above drew the OLD selection.  Force the
           next frame so the moved selection shows without waiting on another input event. */
        g_ctx->retained.wants_redraw = true;
    }
    return changed;
}

/*----------------------------------------------------------------------------------------------
    selectable -- a full-width row that highlights on hover and fills when selected.

    The building block for list boxes: emit one per item (typically inside a child_begin
    region so they scroll and clip independently).  When `selected` is non-NULL a click
    toggles it; pass NULL for a click-only row.  Returns true on the frame it is clicked, so
    a caller managing single-selection can set its own index from the return without relying
    on the toggle.
----------------------------------------------------------------------------------------------*/

bool
gui_selectable( const char* label, bool* selected )
{
    gui_id_t   id = widget_id( label );
    gui_rect_t r  = widget_next_rect( WIDGET_H );

    gui_item_state_t st = widget_behavior( id, r, GUI_WIDGET_KIND_BUTTON );

    /* Fill: selected rows use the active tint, a hovered row the hot tint; otherwise the row
       is transparent so the region background shows through. */
    bool on = ( selected && *selected );
    if ( on || st.hover || st.nav )
        draw_push_rect_filled( r.x, r.y, r.w, r.h, 0,0,1,1, 0,
                               on ? COL_WIDGET_ACT : COL_WIDGET_HOT );

    /* Label, left-aligned with the standard padding. */
    draw_label( r.x + WIDGET_PAD, text_center_y( r.y, r.h ), COL_TEXT, label );
    widget_track_width( r.x + WIDGET_PAD + label_width( label ) );   /* natural width may exceed the row */

    if ( st.clicked && selected )
        *selected = !( *selected );

    if ( st.clicked )
    {
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
