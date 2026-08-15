/*==============================================================================================

    sandbox/gui/sb_gui_style/st_gallery.c -- Look Gallery window: the style applied to widgets.

    A knob is only judged on something it is not attached to.  The Style Editor carries a
    five-widget preview so a drag can be sanity-checked without leaving the panel; this window
    is the wide sweep -- one pass over the widget vocabulary, organized by what the style
    actually addresses (roles, phases, the SELECT plane, the density ramp, the shape picks),
    so a seed change or a rounding change is visible everywhere it lands at once.

    Deliberately not an exhaustive feature demo -- sb_gui_example owns that.  Every widget here
    earns its place by painting through a part of the style grid nothing above it does.

==============================================================================================*/
// clang-format off

static void
st_gallery_window( void )
{
    if ( !st_begin( "Look Gallery", 560.0f, 640.0f ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* --- Text: the INK seed, across the phases that dim it -------------------------------- */
    gui()->separator_text( "Text (GUI_ROLE_TEXT_PRIMARY / TEXT_SECONDARY)" );
    gui()->text( "Regular body text -- TEXT_PRIMARY, GUI_PHASE_IDLE" );
    gui()->text_disabled( "Disabled text -- TEXT_PRIMARY, GUI_PHASE_INERT" );
    gui()->text_colored( gui()->style_color( GUI_ROLE_TEXT_SECONDARY, GUI_PHASE_IDLE ),
                         "Secondary text -- TEXT_SECONDARY, GUI_PHASE_IDLE" );
    gui()->text_wrapped( "Wrapped text runs the full content width and folds at the region edge, "
                         "which is where a padding or gap change shows up first." );

    /* --- Controls: the BG / BORDER / TEXT triple, hover and press live -------------------- */
    gui()->separator_text( "Controls (GUI_ROLE_BG / _BORDER)" );

    static bool  cb_a = true, cb_b = false;
    static i32   radio = 0;
    static f32   fval  = 0.42f;
    static i32   ival  = 3;
    static char  text[ 64 ] = "editable field";

    gui()->row_cols( 0.0f, (f32[]){ 1.0f, 1.0f, 1.0f, GUI_END } );
    gui()->button( "Button" );
    gui()->next_item_fit( 1.0f );
    gui()->button( "Hover Me" );
    gui()->next_item_fit( 1.0f );
    gui()->small_button( "Small" );

    gui()->stack();
    gui()->checkbox( "Checkbox on", &cb_a );
    gui()->checkbox( "Checkbox off", &cb_b );

    gui()->row_cols( 0.0f, (f32[]){ 1.0f, 1.0f, 1.0f, GUI_END } );
    gui()->radio_button( "One", &radio, 0 );
    gui()->radio_button( "Two", &radio, 1 );
    gui()->radio_button( "Three", &radio, 2 );

    gui()->stack();
    gui()->form( GUI_LABEL_RIGHT, gui()->text_size( "Slider float " ).x );
    gui()->slider_float( "Slider float", &fval, 0.0f, 1.0f );
    gui()->slider_int( "Slider int", &ival, 0, 10 );
    gui()->drag_float( "Drag float", &fval, 0.01f, 0.0f, 1.0f, NULL );
    gui()->input_text( "Input", text, sizeof text );
    gui()->form( GUI_LABEL_RIGHT, 0.0f );

    /* --- Value vs indicator: ACCENT holds, MARK shows, GRAB moves -------------------------- */
    gui()->separator_text( "Value + indicator (GUI_ROLE_ACCENT / _MARK / _GRAB)" );
    gui()->progress_bar( fval, NULL );
    gui()->progress_bar( 1.0f - fval, "custom overlay" );

    /* --- The SELECT plane: the same widgets, chosen ---------------------------------------- */
    gui()->separator_text( "SELECT plane (chosen items)" );

    static i32  sel_row = 1;
    static bool tog     = true;

    if ( gui()->child_begin( "##rows", 0.0f, gui()->sz_child_rows_h( 4 ), GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->scale_push( GUI_SCALE_DENSE );
        for ( i32 i = 0; i < 6; ++i )
        {
            char label[ 32 ];
            snprintf( label, sizeof label, "Selectable row %d", i );
            bool on = ( i == sel_row );
            if ( gui()->selectable( label, &on ) )
                sel_row = i;
        }
        gui()->scale_pop();
    }
    gui()->child_end();

    gui()->checkbox( "Toggle (latched)", &tog );

    static const char* const combo_items[] = { "Alpha", "Beta", "Gamma" };
    static i32 combo_sel = 0;
    if ( gui()->combo_begin( "Combo", combo_items[ combo_sel ], GUI_COMBO_NONE ) )
    {
        for ( i32 i = 0; i < 3; ++i )
        {
            bool on = ( i == combo_sel );
            if ( gui()->selectable( combo_items[ i ], &on ) )
                combo_sel = i;
        }
        gui()->combo_end();
    }

    /* --- Extended palette: the flat hues that are not the theme's role/phase grid ------------ */
    gui()->separator_text( "Extended palette (INFO / OK / WARN / ERROR / DROP)" );
    gui()->text_colored( gui()->style_ext( GUI_EXT_INFO  ), "Info -- a neutral notice" );
    gui()->text_colored( gui()->style_ext( GUI_EXT_OK    ), "Ok -- healthy, passing" );
    gui()->text_colored( gui()->style_ext( GUI_EXT_WARN  ), "Warn -- near a limit" );
    gui()->text_colored( gui()->style_ext( GUI_EXT_ERROR ), "Error -- failed" );
    gui()->text_colored( gui()->style_ext( GUI_EXT_DROP  ), "Drop -- a drop can land here" );

    /* --- Density ramp: the same row at every scale_push step -------------------------------- */
    gui()->separator_text( "Density ramp (scale_push)" );

    static const char* const nm_scale[ GUI_SCALE_COUNT ] = { "Dense", "Std", "Roomy", "Bar" };
    for ( u32 s = 0; s < GUI_SCALE_COUNT; ++s )
    {
        gui()->push_id( nm_scale[ s ] );
        gui()->scale_push( ( gui_scale_t )s );
        gui()->row_cols( 0.0f, (f32[]){ 1.0f, 1.0f, GUI_END } );
        gui()->button( nm_scale[ s ] );
        gui()->next_item_fit( 1.0f );
        gui()->button( "row / pad / gap" );
        gui()->scale_pop();
        gui()->pop_id();
    }

    gui()->stack();

    /* --- Panels: PANEL and TITLE, the container half of the grid ---------------------------- */
    gui()->separator_text( "Panels (GUI_ROLE_PANEL / _TITLE)" );
    if ( gui()->child_begin( "##panel", 0.0f, gui()->sz_child_rows_h( 3 ), GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "A child region paints GUI_ROLE_PANEL and takes PANEL_ROUND." );
        gui()->button( "Inside" );
    }
    gui()->child_end();

    gui()->window_end();
}

// clang-format on
