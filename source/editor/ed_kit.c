/*==============================================================================================

    editor/ed_kit.c -- editor kit implementation.

    Thin like every kit: no state, no style of its own.  The label column reads the
    installed style's secondary text (labels recede, values read), and the value zone is
    plain flow -- the stock chrome renders itself.

==============================================================================================*/

#include "editor/ed_kit.h"

// clang-format off

/* Label column fraction of the row -- the classic inspector split. */
#define ED_PROP_LABEL_FRAC 0.38f

/* Take one standard row from the ambient flow and split off the label column. */
static gui_rect_t
ed_prop_row( const char* label )
{
    gui_rect_t row = gui()->flow_cell( 0.0f, 0.0f );    /* natural width, one standard row */
    gui_rect_t lab = gui_rect_cut_left( &row, row.w * ED_PROP_LABEL_FRAC );

    gui()->draw_text_in( lab, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                         gui()->style_color( GUI_ROLE_TEXT_SECONDARY, GUI_PHASE_IDLE ), label );
    return row;
}

void
ed_prop_begin( const char* label )
{
    gui_rect_t zone = ed_prop_row( label );

    gui()->flow_begin( zone );
    gui()->stack();
}

void
ed_prop_end( void )
{
    gui()->flow_end();
}

void
ed_prop_text( const char* label, const char* value )
{
    gui_rect_t zone = ed_prop_row( label );
    gui()->stock_label( zone, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                     ( value && value[ 0 ] ) ? value : "-" );
}

// clang-format on
/*============================================================================================*/
