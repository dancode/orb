/*==============================================================================================

    runtime_service/gui/chrome/widgets/gui_input.c -- Single-line text field variants.

    input_text / input_text_ex / input_text_with_hint share one layout (label split, one
    WIDGET_H row) and one frame draw (focused-tinted fill + hot-tinted border); input_text_begin
    factors those shared steps so each variant reduces to its one point of difference.  All
    editing logic (cursor movement, selection, insertion, deletion, horizontal scroll,
    rendering) delegates to input_field_edit (gui_text_edit.c, included before this file).

    Numeric text inputs (input_int, _float, _double, _float2/3/4) are gui_widget_numeric.c,
    included after gui_widget_slider.c.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    input_text / input_text_ex / input_text_with_hint -- single-line text field variants.

    All three share the same layout (label split, one WIDGET_H row) and the same frame draw
    (focused-tinted fill + hot-tinted border).  input_text_begin factors out those shared steps
    so each variant reduces to its one point of difference.  All editing logic (cursor movement,
    selection, insertion, deletion, horizontal scroll, rendering) delegates to input_field_edit
    (gui_text_edit.c); the wrapper handles only the label split, box background, border, and
    focus claim.
==============================================================================================*/

typedef struct { gui_id_t id; gui_rect_t box; gui_item_state_t st; } input_text_frame_t;

static input_text_frame_t
input_text_begin( const char* label )
{
    gui_id_t id = item_id( label );

    /* Label via the ambient field seam (gui_field_row): aligned column under a form / field_split,
       trailing otherwise, or dropped when hidden / skipped -- then the box fills the control cell. */
    gui_field_row( label );
    gui_rect_t       box_r = cell_next( WIDGET_H );
    gui_item_state_t st    = item_state( id, box_r, ITEM_FOCUSABLE );

    /* A field's ground does not travel: BG/IDLE hot, focused, or at rest.  The phase axis is
       what a BUTTON spends -- it lights under the cursor and sinks under a press because those
       are the two halves of "you may click this, and you did".  A text field offers neither: it
       is a surface you type INTO, its engaged state lasts as long as the caret is in it rather
       than as long as a button is held, and the ink plus the caret already say so.  Spending
       BG/ACTIVE on focus also cost the selection band its only distinct cell -- the highlight
       drew the pressed face over a pressed face (see edit_sel_color).

       The border carries focus, which is the whole affordance a field needs and the one place
       the signal cannot be mistaken for content. */
    draw_face( box_r, GUI_ROLE_BG, GUI_PHASE_IDLE );
    draw_outline( box_r, WIN_BORDER, col_field_border( st ) );
    return ( input_text_frame_t ){ id, box_r, st };
}

bool
gui_input_text( const char* label, char* buf, u32 bufsz )
{
    input_text_frame_t f = input_text_begin( label );
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, NULL, NULL ).enter;
}

bool
gui_input_text_ex( const char* label, char* buf, u32 bufsz,
                     gui_text_cb_fn on_change, void* cb_user )
{
    input_text_frame_t f = input_text_begin( label );
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, on_change, cb_user ).enter;
}

bool
gui_input_text_with_hint( const char* label, const char* hint, char* buf, u32 bufsz )
{
    input_text_frame_t f = input_text_begin( label );
    if ( !f.st.focused && buf[ 0 ] == '\0' && hint && hint[ 0 ] )
    {
        /* No per-widget clip: the hint fits the box in the common case, and the window's clip rect
           already bounds any overflow -- so no scissor (no batch split) and no ellipsis. */
        draw_push_text( f.box.x + WIDGET_PAD, text_center_y( f.box.y, f.box.h ),
                        COL_TEXT_DIM, hint );
    }
    return input_field_edit( f.id, f.box, f.st, buf, bufsz, NULL, NULL ).enter;
}

// clang-format on
/*============================================================================================*/
