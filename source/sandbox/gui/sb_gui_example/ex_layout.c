/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_layout.c -- "Layout" category demos.

    The composition machinery: layout headers (stack / rows / columns / grid / pack), field
    forms, alignment, sub-layouts, child regions, the sz_ sizing family, and the rect-carving
    toolkit (split / carve / anchor / overlay).  Included by ex_demos.c (the root unity TU).

==============================================================================================*/

/*==============================================================================================
    Rows & Columns -- the repeating row template and the full layout descriptor.
==============================================================================================*/

static void
ex_layout_rows( void )
{
    if ( ex_begin( "Rows & Columns", 460, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();                         /* heading lines sit in a plain stack */
        gui()->text( "One overloaded f32 sizes every track:" );
        gui()->text( ">1 px   1.0 fill   (0,1) fraction   <0 END" );

        gui()->separator_text( "row_cols_n( 0, 3 ) -- three equal columns" );
        gui()->row_cols_n( 0, 3 );
        gui()->button( "A" );
        gui()->button( "B" );
        gui()->button( "C" );
        gui()->row( 0 );                        /* back to a single column */

        gui()->separator_text( "row2 / row3 / row4 -- weighted" );
        gui()->row2( 0.3f, 0.7f );
        gui()->button( "30%" );
        gui()->button( "70%" );
        gui()->row3( 0.25f, 0.5f, 0.25f );
        gui()->button( "1/4" );
        gui()->button( "1/2" );
        gui()->button( "1/4" );
        gui()->row4( 1.0f, 1.0f, 1.0f, 1.0f );
        gui()->button( "a" );
        gui()->button( "b" );
        gui()->button( "c" );
        gui()->button( "d" );
        gui()->row( 0 );

        gui()->separator_text( "row_cols -- 120px + fill + 80px" );
        gui()->row_cols( 0, ( f32[] ){ 120, 1, 80, GUI_END } );
        gui()->button( "fixed 120" );
        gui()->button( "fill" );
        gui()->button( "80" );
        gui()->row( 0 );

        gui()->separator_text( "row( 48 ) -- one tall row" );
        gui()->row( 48 );
        gui()->button( "tall button" );
        gui()->row( 0 );

    }
    gui()->window_end();
}

/*==============================================================================================
    Field Forms -- aligned "Label  [control]" rows from a single widget call.
==============================================================================================*/

static void
ex_layout_fields( void )
{
    static char f_name[ 32 ] = "player";
    static f32  f_speed      = 5.0f;
    static f32  f_volume     = 7.0f;
    static bool f_enabled    = true;

    if ( ex_begin( "Field Forms", 420, 520, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* The label gutter width is itself a live parameter. */
        static i32 label_w = 90;
        gui()->slider_int( "Label width", &label_w, 40, 180 );

        /* Each section reuses the same labels, so scope ids with push_id. */
        gui()->separator_text( "field_label_left" );
        gui()->push_id( "left" );
        gui()->field_label_left( (f32)label_w );
        gui()->input_text  ( "Name",    f_name, sizeof( f_name ) );
        gui()->slider_float( "Speed",   &f_speed, 0.0f, 10.0f );
        gui()->checkbox    ( "Enabled", &f_enabled );
        gui()->field_label_left( 0.0f );        /* clear the split */
        gui()->pop_id();

        gui()->separator_text( "field_label_right" );
        gui()->push_id( "right" );
        gui()->field_label_right( (f32)label_w );
        gui()->input_text  ( "Name",    f_name, sizeof( f_name ) );
        gui()->checkbox    ( "Enabled", &f_enabled );
        gui()->field_label_right( 0.0f );
        gui()->pop_id();

        gui()->separator_text( "field_split( LEFT, 0.4, 0.6 ) -- fractional" );
        gui()->push_id( "split" );
        gui()->field_split( GUI_LABEL_LEFT, 0.4f, 0.6f );
        gui()->slider_float( "Volume", &f_volume, 0.0f, 10.0f );
        gui()->field_label_left( 0.0f );
        gui()->pop_id();

        /* form() -- the one-call header: a stack with the label track built in. */
        gui()->separator_text( "form( LEFT, label_w )" );
        gui()->push_id( "form" );
        gui()->form( GUI_LABEL_LEFT, (f32)label_w );
        gui()->input_text  ( "Name",    f_name, sizeof( f_name ) );
        gui()->slider_float( "Speed",   &f_speed, 0.0f, 10.0f );
        gui()->checkbox    ( "Enabled", &f_enabled );
        gui()->pop_id();
        gui()->layout_default();
    }
    gui()->window_end();
}

/*==============================================================================================
    Grid -- a fixed cols x rows matrix in a bounded box, shape driven live.
==============================================================================================*/

static void
ex_layout_grid( void )
{
    if ( ex_begin( "Grid", 440, 480, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "grid_cells( nc, nr ): bounded matrix, row-major," );
        gui()->text( "nothing scrolls.  skip() leaves a cell blank." );

        static i32  nc        = 3;
        static i32  nr        = 3;
        static bool skip_ctr  = true;
        gui()->slider_int( "Columns", &nc, 1, 6 );
        gui()->slider_int( "Rows",    &nr, 1, 6 );
        gui()->checkbox( "skip() the center cell", &skip_ctr );

        /* The grid fills the remaining content box, so bound it in a fixed-height child. */
        if ( gui()->child_begin( "grid_box", 0, 280, GUI_WIN_NOSCROLL ) )
        {
            gui()->grid_cells( (u32)nc, (u32)nr );
            i32 center = ( nr / 2 ) * nc + nc / 2;
            for ( i32 i = 0; i < nc * nr; i++ )
            {
                if ( skip_ctr && i == center )
                {
                    gui()->skip();
                    continue;
                }
                gui()->push_id_int( i );
                char label[ 8 ];
                snprintf( label, sizeof( label ), "%d", i );
                gui()->button( label );
                gui()->pop_id();
            }
        }
        gui()->child_end();

        gui()->textf( "%d x %d = %d cells", nc, nr, nc * nr );
    }
    gui()->window_end();
}

/*==============================================================================================
    Align & Spacing -- cell alignment on both axes, same_line, new_line, next_item_fit.
==============================================================================================*/

static void
ex_layout_align( void )
{
    if ( ex_begin( "Align & Spacing", 420, 540, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* Interactive alignment: two radio axes feed one gui_align_t for the sample rows. */
        gui()->separator_text( "align() -- pick both axes" );
        static i32 h_sel = 0;   /* 0 left, 1 center, 2 right  */
        static i32 v_sel = 1;   /* 0 top,  1 center, 2 bottom */
        gui()->radio_button( "Left",    &h_sel, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "HCenter", &h_sel, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Right",   &h_sel, 2 );
        gui()->radio_button( "Top",     &v_sel, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "VCenter", &v_sel, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Bottom",  &v_sel, 2 );

        gui_align_t al = ( h_sel == 1 ? GUI_ALIGN_HCENTER : h_sel == 2 ? GUI_ALIGN_RIGHT  : GUI_ALIGN_LEFT )
                       | ( v_sel == 1 ? GUI_ALIGN_VCENTER : v_sel == 2 ? GUI_ALIGN_BOTTOM : GUI_ALIGN_TOP );

        /* Tall two-column row: natural-size text obeys the alignment; buttons fill anyway. */
        gui()->row_cols_n( 64, 2 );
        gui()->align( al );
        gui()->text( "text (natural)" );
        gui()->text( "in a 64px cell" );
        gui()->align( GUI_ALIGN_LEFT );
        gui()->row( 0 );

        gui()->separator_text( "same_line" );
        gui()->button( "OK" );
        gui()->same_line( 8.0f );
        gui()->button( "Cancel" );
        gui()->same_line( 8.0f );
        gui()->button( "Apply" );

        gui()->separator_text( "new_line( h )" );
        static i32 gap_h = 32;
        gui()->slider_int( "gap height", &gap_h, 0, 96 );
        gui()->text( "above the gap" );
        gui()->new_line( (f32)gap_h );
        gui()->text( "below the gap" );

        gui()->separator_text( "next_item_fit -- one-shot size override" );
        gui()->row_cols_n( 0, 2 );
        gui()->button( "natural cell fill" );
        gui()->next_item_fit( 0.5f );           /* half its column, then align seats it */
        gui()->button( "half" );
        gui()->row( 0 );
    }
    gui()->window_end();
}

/*==============================================================================================
    Sub-layout -- a transient nested layout inside one cell.
==============================================================================================*/

static void
ex_layout_sublayout( void )
{
    if ( ex_begin( "Sub-layout", 420, 380, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "row_cols_n( sz_rows_h(4), 2 ): col 0 is a sub-layout" );
        gui()->separator();

        /* A sub-layout does not grow its parent row (see push_layout) -- fitting its content inside
           the cell handed to it is the caller's job.  Column 0 stacks 4 lines (a label + 3 buttons),
           so the row height is declared up front instead of left auto (auto would take column 0's
           bare cell request, one line, and the sub-layout's extra content would overflow it). */
        gui()->row_cols_n( gui()->sz_rows_h( 4 ), 2 );

        /* Column 0: open a sub-layout and stack three buttons inside the single cell. */
        gui()->push_layout();
            gui()->stack();                     /* the sub-layout is a region too */
            gui()->text( "stacked:" );
            gui()->button( "one" );
            gui()->button( "two" );
            gui()->button( "three" );
        gui()->pop_layout();

        /* Column 1: a single widget filling the other cell. */
        gui()->text( "single cell" );

        gui()->row( 0 );

        gui()->separator_text( "nested shapes" );
        gui()->row_cols_n( 0, 3 );
        for ( i32 c = 0; c < 3; c++ )
        {
            gui()->push_id_int( c );
            gui()->push_layout();
                gui()->row_cols_n( 0, 2 );      /* a 2-col row INSIDE one cell */
                gui()->button( "a" );
                gui()->button( "b" );
            gui()->pop_layout();
            gui()->pop_id();
        }
        gui()->row( 0 );
    }
    gui()->window_end();
}

/*==============================================================================================
    Pack & Bars -- natural-size print runs: toolbars (bar), vertical strips, wrap, fill.
==============================================================================================*/

static void
ex_layout_pack( void )
{
    if ( ex_begin( "Pack & Bars", 460, 460, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Pack mode: the widget sizes itself (vs cells" );
        gui()->text( "sizing the widget in columns / grid)." );

        gui()->separator_text( "bar() -- toolbar + fill search box" );
        gui()->bar();
        gui()->button( "New" );
        gui()->button( "Open" );
        gui()->button( "Save" );
        static char find[ 32 ] = "";
        gui()->pack_size( 1.0f );                   /* next item fills the rest of the line */
        gui()->input_text( "##find", find, sizeof( find ) );

        gui()->stack();
        gui()->separator_text( "pack_nextline() -- wrap the run" );
        static i32 per_line = 3;
        gui()->slider_int( "items per line", &per_line, 1, 6 );
        gui()->bar();
        for ( i32 i = 0; i < 9; i++ )
        {
            gui()->push_id_int( i );
            char b[ 12 ];
            snprintf( b, sizeof( b ), "btn %d", i );
            gui()->button( b );
            if ( i % per_line == per_line - 1 )
                gui()->pack_nextline();
            gui()->pop_id();
        }

        gui()->stack();
        gui()->separator_text( "strip() -- the vertical pack" );
        gui()->text( "Items flow top to bottom at natural height:" );
        if ( gui()->child_begin( "strip_box", 0, 120, GUI_WIN_NOSCROLL ) )
        {
            gui()->strip();
            gui()->button( "first" );
            gui()->button( "second" );
            gui()->pack_size( 1.0f );               /* fill the rest of the column */
            gui()->button( "fills the rest" );
        }
        gui()->child_end();

        gui()->separator_text( "pack_size fractions" );
        static f32 frac = 0.5f;
        gui()->slider_float( "next item fraction", &frac, 0.1f, 1.0f );
        gui()->bar();
        gui()->button( "fixed" );
        gui()->pack_size( frac );
        gui()->button( "fraction of the line" );
        gui()->stack();
    }
    gui()->window_end();
}

/*==============================================================================================
    Child Regions -- independent scroll boxes, resize grips, auto-size + constraints.
==============================================================================================*/

static void
ex_layout_children( void )
{
    if ( ex_begin( "Child Regions", 400, 620, GUI_WIN_HSCROLL ) )
    {
        gui()->stack();
        gui()->text( "List box (scrolls independently):" );

        static i32 sel = -1;
        if ( gui()->child_begin( "rows", 0, 200, GUI_WIN_NONE ) )
        {
            gui()->stack();                     /* the child is its own region -- declare its mode */
            for ( i32 i = 0; i < 40; i++ )
            {
                gui()->push_id_int( i );
                char row[ 32 ];
                snprintf( row, sizeof( row ), "row item %02d", i );
                bool on = ( sel == i );
                if ( gui()->selectable( row, &on ) )
                    sel = on ? i : -1;
                gui()->pop_id();
            }
        }
        gui()->child_end();
        gui()->textf( "selected: %d", sel );

        /* User-resizable child: the bottom grip makes the height user-owned + persisted. */
        gui()->separator_text( "GUI_WIN_CHILD_RESIZE_Y" );
        gui()->help_marker( "Drag the bottom border to resize this box vertically." );
        if ( gui()->child_begin( "resizeable", 0, 120, GUI_WIN_CHILD_RESIZE_Y ) )
        {
            gui()->stack();
            for ( i32 i = 0; i < 12; i++ )
                gui()->textf( "resizeable line %02d", i );
        }
        gui()->child_end();
        gui()->text( "this line sits below the resizeable box" );

        /* Auto-resize with a height cap: hug content up to max_lines rows, then scroll. */
        gui()->separator_text( "Auto-size + size constraints" );
        static i32 draw_lines = 3;
        static i32 max_lines  = 10;
        gui()->drag_int( "Lines Count",    &draw_lines, 0.2f, 0, 30, "%d" );
        gui()->drag_int( "Max (in lines)", &max_lines,  0.2f, 1, 20, "%d" );

        f32 line = gui()->sz_fit_row( gui()->sz_line_h() );
        gui()->window_set_next_size_constraints( 0.0f, line, 0.0f, line * (f32)max_lines );
        if ( gui()->child_begin( "constrained", 0, 0, GUI_WIN_NONE ) )
        {
            gui()->stack();
            for ( i32 n = 0; n < draw_lines; n++ )
                gui()->textf( "Line %04d", n );
        }
        gui()->child_end();
    }
    gui()->window_end();
}

/*==============================================================================================
    Sizing Helpers -- the sz_ family readouts, content_avail, empty(), the layout pen.
==============================================================================================*/

static void
ex_layout_sizing( void )
{
    if ( ex_begin( "Sizing Helpers", 440, 520, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Intent -> pixels; every value below is live." );

        static i32 n = 3;
        gui()->slider_int( "n", &n, 0, 12 );

        gui()->separator_text( "The sz_ family" );
        gui()->label_text( "sz_u( n )",       "grid quanta" );
        gui()->textf( "  sz_u( %d )        = %.1f px", n, gui()->sz_u( (f32)n ) );
        gui()->textf( "  sz_rows_h( %d )   = %.1f px  (n uniform rows + gaps)", n, gui()->sz_rows_h( (u32)n ) );
        gui()->textf( "  sz_chars( %d )    = %.1f px  (n glyph advances)", n, gui()->sz_chars( (f32)n ) );
        gui()->textf( "  sz_line_h()      = %.1f px  (font line advance)", gui()->sz_line_h() );
        gui()->textf( "  sz_row_gap()     = %.1f px  (inter-row gap)", gui()->sz_row_gap() );
        gui()->textf( "  sz_fit_row( 64 ) = %.1f px  (64px content + margin)", gui()->sz_fit_row( 64.0f ) );
        gui()->textf( "  sz_fit_col( 64 ) = %.1f px", gui()->sz_fit_col( 64.0f ) );

        gui()->separator_text( "Ramp-step rows (sz_scale_row)" );
        gui()->textf( "DENSE %.0f   STD %.0f   ROOMY %.0f   BAR %.0f",
                      gui()->sz_scale_row( GUI_SCALE_DENSE ), gui()->sz_scale_row( GUI_SCALE_STD ),
                      gui()->sz_scale_row( GUI_SCALE_ROOMY ), gui()->sz_scale_row( GUI_SCALE_BAR ) );

        /* empty() reserves a block; cursor_screen_pos is the pen -- mark both visually. */
        gui()->separator_text( "empty() + cursor_screen_pos" );
        gui_rect_t slot = gui()->empty( 160.0f, 40.0f );
        gui()->draw_frame( slot, GUI_COLOR( 0x20, 0x30, 0x40, 0xFF ),
                                 GUI_COLOR( 0x4F, 0xC3, 0xF7, 0xFF ), 1.0f );
        gui()->draw_text_in( slot, GUI_ALIGN_CENTER, 0xFFE0E0E0u, "empty( 160, 40 )" );

        gui_vec2_t pen = gui()->cursor_screen_pos();
        gui()->textf( "pen after the slot: (%.0f, %.0f)", pen.x, pen.y );

        /* content_avail shrinks as the window shrinks -- resize to watch it. */
        gui()->separator_text( "content_avail (resize the window)" );
        gui_vec2_t avail = gui()->content_avail();
        gui()->textf( "%.0f x %.0f px left below this line", avail.x, avail.y );
    }
    gui()->window_end();
}

/*==============================================================================================
    Split & Carve -- rect composition: split panels, carve forms, anchors, layout overlays.
==============================================================================================*/

static void
ex_layout_carve( void )
{
    if ( ex_begin( "Split & Carve", 500, 640, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* split_begin / split_next / split_end -- two flow panels sharing a Y-level. */
        gui()->separator_text( "split_begin -- left flow + right button" );
        const char* bake = "Bake + Preview";
        gui()->split_begin( "##src", gui()->button_width( bake ) );
            gui()->stack();
            static i32 quality = 2;
            gui()->slider_int( "Quality", &quality, 0, 4 );
            static bool fast = true;
            gui()->checkbox( "Fast path", &fast );
        gui()->split_next();
            gui()->stack();
            gui()->button_fill( bake );         /* fills the right panel's height */
        gui()->split_end();

        /* split() -- pure rect math into a canvas, ratio + gap live. */
        gui()->separator_text( "split() -- rect math (parametric)" );
        static f32 ratio = 0.3f;
        static i32 gap   = 6;
        gui()->slider_float( "sidebar fraction", &ratio, 0.1f, 0.9f );
        gui()->slider_int  ( "gap", &gap, 0, 16 );
        {
            gui_rect_t area = gui()->canvas( 90.0f );
            gui_rect_t panes[ 3 ];
            u32 count = gui()->split( area, GUI_AXIS_X,
                                      ( f32[] ){ ratio, 1.0f, 80.0f, GUI_END }, (f32)gap, panes );
            static const char* tags[ 3 ] = { "fraction", "fill", "80 px" };
            for ( u32 i = 0; i < count; i++ )
            {
                gui()->draw_frame( panes[ i ], GUI_COLOR( 0x24, 0x2C, 0x34, 0xFF ),
                                               GUI_COLOR( 0x4F, 0xC3, 0xF7, 0xFF ), 1.0f );
                gui()->draw_text_in( panes[ i ], GUI_ALIGN_CENTER, 0xFFE0E0E0u, tags[ i ] );
            }
        }

        /* carve() -- one flat form describing a whole nested partition. */
        gui()->separator_text( "carve() -- nested partition from one form" );
        {
            /* A header band, then a body split into a sidebar and a two-row content column. */
            static const f32 form[] = {
                GUI_CUT_Y,                       /* root: split area into rows        */
                    28.0f,                       /* header leaf, 28px                 */
                    1.0f, GUI_CUT_X,             /* body: fill row, split into cols   */
                        0.3f,                    /* sidebar leaf, 30%                 */
                        1.0f, GUI_CUT_Y,         /* content: fill col, split into rows */
                            1.0f,                /* view leaf                          */
                            24.0f,               /* status leaf, 24px                  */
                        GUI_END,
                    GUI_END,
                GUI_END,
            };
            gui_rect_t area = gui()->canvas( 150.0f );
            gui_rect_t leaf[ 8 ];
            u32 count = gui()->carve( form, area, 4.0f, leaf, 8 );
            static const char* tags[ 4 ] = { "header", "sidebar", "view", "status" };
            for ( u32 i = 0; i < count && i < 4; i++ )
            {
                gui()->draw_frame( leaf[ i ], GUI_COLOR( 0x2A, 0x24, 0x34, 0xFF ),
                                              GUI_COLOR( 0xFF, 0xB0, 0x40, 0xFF ), 1.0f );
                gui()->draw_text_in( leaf[ i ], GUI_ALIGN_CENTER, 0xFFE0E0E0u, tags[ i ] );
            }
        }

        /* anchor() -- point-pin vs stretch, every field on a slider. */
        gui()->separator_text( "anchor() -- point pin vs stretch" );
        static bool stretch = false;
        static f32  ax      = 0.5f;
        static f32  pivot   = 0.5f;
        gui()->checkbox( "Stretch band (min.x < max.x)", &stretch );
        gui()->slider_float( "anchor x (fraction)", &ax, 0.0f, 1.0f );
        if ( !stretch )
            gui()->slider_float( "pivot x", &pivot, 0.0f, 1.0f );
        {
            gui_rect_t parent = gui()->canvas( 90.0f );
            gui()->draw_frame( parent, GUI_COLOR( 0x1E, 0x1E, 0x1E, 0xFF ),
                                       GUI_COLOR( 0x60, 0x60, 0x60, 0xFF ), 1.0f );
            gui_anchor_t a = { 0 };
            if ( stretch )
            {
                a.min = ( gui_vec2_t ){ 0.0f, 0.5f };           /* stretch x, point-pin y  */
                a.max = ( gui_vec2_t ){ ax > 0.1f ? ax : 0.1f, 0.5f };
                a.pivot = ( gui_vec2_t ){ 0.5f, 0.5f };
                a.size  = ( gui_vec2_t ){ 0.0f, 30.0f };
                a.off   = ( gui_pad_t ){ 8.0f, 8.0f, 0.0f, 0.0f };
            }
            else
            {
                a.min = a.max = ( gui_vec2_t ){ ax, 0.5f };     /* point-anchored both axes */
                a.pivot = ( gui_vec2_t ){ pivot, 0.5f };
                a.size  = ( gui_vec2_t ){ 120.0f, 30.0f };
            }
            gui_rect_t child = gui()->anchor( parent, a );
            gui()->draw_frame( child, GUI_COLOR( 0x20, 0x40, 0x28, 0xFF ),
                                      GUI_COLOR( 0x48, 0xE6, 0x18, 0xFF ), 1.0f );
            gui()->draw_text_in( child, GUI_ALIGN_CENTER, 0xFFE0E0E0u,
                                 stretch ? "stretch" : "pinned" );
        }

        /* push_layout_overlay -- real widgets inside a carved rect, parent flow untouched. */
        gui()->separator_text( "push_layout_overlay -- widgets in a carved rect" );
        {
            gui_rect_t area  = gui()->canvas( 70.0f );
            gui()->draw_frame( area, GUI_COLOR( 0x1E, 0x1E, 0x1E, 0xFF ),
                                     GUI_COLOR( 0x60, 0x60, 0x60, 0xFF ), 1.0f );
            gui_rect_t inner = gui_rect_pad( area, 8.0f );
            gui()->push_layout_overlay( inner );
                gui()->row_cols_n( 0, 2 );
                static bool ov_flag = true;
                gui()->checkbox( "live widget", &ov_flag );
                static f32 ov_val = 0.5f;
                gui()->slider_float( "##ov", &ov_val, 0.0f, 1.0f );
            gui()->pop_layout();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Flow Seams -- flow_begin / flow_cell / flow_end: the recursive rect <-> flow contract.
    The canonical nesting from docs/GUI_STACK_PLAN.md section 4, live: carve a rect, auto-flow
    into it, take one flow element back out as a rect, carve THAT, flow into the remainder --
    three flow depths under one window, every crossing through the same two verbs.
==============================================================================================*/

static void
ex_layout_flow( void )
{
    if ( ex_begin( "Flow Seams", 540, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "carve -> flow -> cell -> carve -> flow: one contract, both directions." );

        static f32 panel_w = 220.0f;
        gui()->slider_float( "panel px", &panel_w, 140.0f, 320.0f );

        /* Rect production by hand: a canvas cut into a flow panel and a marked remainder. */
        gui_rect_t area  = gui()->canvas( 320.0f );
        gui_rect_t panel = gui_rect_cut_left( &area, panel_w );
        gui()->draw_frame( area, GUI_COLOR( 0x1E, 0x1E, 0x1E, 0xFF ),
                                 GUI_COLOR( 0x60, 0x60, 0x60, 0xFF ), 1.0f );
        gui()->draw_text_in( area, GUI_ALIGN_CENTER, 0xFF707070u, "carved remainder" );

        /* Depth 1: auto-flow into the carved panel. */
        gui()->flow_begin( gui_rect_pad( panel, 4.0f ) );
            gui()->stack();
            gui()->text( "depth 1: flow in a cut rect" );
            static bool stock = true;
            gui()->checkbox( "stock widget", &stock );

            /* One flow element back out as a rect, carved by hand again. */
            gui_rect_t cell = gui()->flow_cell( 0.0f, 120.0f );
            gui_rect_t half = gui_rect_cut_left( &cell, cell.w * 0.5f );

            /* Custom widget in the carved half: rect + item() + draw_*. */
            static i32 clicks = 0;
            gui_item_state_t st = gui()->item( "half##flow", half );
            if ( st.clicked ) { clicks++; gui()->request_redraw(); }
            gui()->draw_frame( half,
                               st.active ? GUI_COLOR( 0x30, 0x50, 0x70, 0xFF )
                                         : GUI_COLOR( 0x24, 0x2C, 0x34, 0xFF ),
                               st.hover  ? GUI_COLOR( 0x4F, 0xC3, 0xF7, 0xFF )
                                         : GUI_COLOR( 0x60, 0x60, 0x60, 0xFF ), 1.0f );
            gui()->draw_text_in( half, GUI_ALIGN_CENTER, 0xFFE0E0E0u, "item()" );

            /* Depth 2: flow into the remainder of that same cell. */
            gui()->flow_begin( gui_rect_pad( cell, 4.0f ) );
                gui()->stack();
                static bool deep = false;
                gui()->checkbox( "depth 2", &deep );

                /* Depth 3: cell -> pad (carve) -> flow once more. */
                gui_rect_t c2 = gui()->flow_cell( 0.0f, 44.0f );
                gui()->draw_frame( c2, GUI_COLOR( 0x2A, 0x24, 0x34, 0xFF ),
                                       GUI_COLOR( 0xFF, 0xB0, 0x40, 0xFF ), 1.0f );
                gui()->flow_begin( gui_rect_pad( c2, 6.0f ) );
                    gui()->stack();
                    gui()->text( "depth 3" );
                gui()->flow_end();
            gui()->flow_end();

            gui()->textf( "item clicks: %d", clicks );
        gui()->flow_end();

        /* The outer stack resumes below the canvas as if nothing happened. */
        gui()->text( "outer flow resumed below the canvas." );
    }
    gui()->window_end();
}

/*==============================================================================================
    Natural & Wrap -- measured natural columns, the next_item_* one-shots, pack auto-wrap.
==============================================================================================*/

static void
ex_layout_natural( void )
{
    if ( ex_begin( "Natural & Wrap", 460, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "A natural (0) column sizes to its widest" );
        gui()->text( "natural item -- measured feedback, one frame." );

        /* The 0 track hugs the widest button below; the fill track takes the rest. */
        gui()->separator_text( "cols( {0, 1} ) -- natural label column" );
        gui()->cols( ( f32[] ){ 0, 1.0f, GUI_END } );
        gui()->button( "Ok" );                gui()->text( "<- fill track" );
        gui()->button( "A longer label" );    gui()->text( "column = widest" );
        gui()->button( "Mid one" );           gui()->text( "left edge aligned" );
        gui()->stack();

        /* One-shots: this item only, the region's align / auto height come right back. */
        gui()->separator_text( "next_item_align / next_item_h" );
        gui()->next_item_align( GUI_ALIGN_RIGHT );
        gui()->button( "align-self RIGHT" );
        gui()->button( "back on region align" );
        gui()->next_item_h( gui()->sz_u( 12 ) );
        gui()->button( "next_item_h( sz_u( 12 ) )" );

        /* Auto-wrap: natural-width items break to a fresh line at the edge -- resize me. */
        gui()->separator_text( "bar() + pack_wrap() -- resize the window" );
        gui()->bar();
        gui()->pack_wrap();
        static const char* tags[] = { "alpha", "beta", "gamma", "delta", "epsilon",
                                      "zeta", "eta", "theta", "iota", "kappa" };
        for ( u32 i = 0; i < 10; i++ )
            gui()->button( tags[ i ] );
        gui()->stack();
    }
    gui()->window_end();
}

/*==============================================================================================
    Panel Shell -- one carve form spent on a whole app shell, real widgets inside every leaf.

    The applied companion to "Split & Carve" above: that demo shows what carve() RETURNS (frames
    drawn into the leaves, every parameter on a slider), this one spends the result -- a fixed
    sidebar beside a filling content column, and that column cut top to bottom into header /
    body / footer.  Known sizes, one pass, plain gui_rect_t locals: no layout tree and no cached
    heights.  Absolute rects leave the pen where it started, so the band is reserved with empty()
    at the end or the window would size to nothing.
==============================================================================================*/

static void
ex_layout_shell( void )
{
    if ( ex_begin( "Panel Shell", 520, 380, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text_wrapped( "One gui()->carve form describes the whole nested layout: a column split "
                             "(80px sidebar + fill content), the content track itself cut into rows "
                             "(header / body / footer).  Leaf rects come back in reading order." );

        /* The entire layout as one flat form -- structure lives in where the CUT/END sentinels sit.
           Leaves stream back in reading order: 0 sidebar, 1 header, 2 body, 3 footer. */
        static const f32 FORM[] =
        {
            GUI_CUT_X,                  /* root: cut the band into columns           */
                80.0f,                  /*   leaf 0 : 80px sidebar                    */
                1.0f, GUI_CUT_Y,        /*   fill content column, cut into rows:      */
                    28.0f,              /*       leaf 1 : 28px header                 */
                    1.0f,               /*       leaf 2 : fill body                   */
                    28.0f,              /*       leaf 3 : 28px footer                 */
                GUI_END,                /*   close rows                               */
                // 128.0f,                  /*   leaf 4 : 128px right sidebar         */
            GUI_END,                    /* close columns                              */
        };

        /* A fixed-height band carved from the region's available area. */
        gui_rect_t band = gui()->content_rect();
        band.h = 180.0f;

        gui_rect_t cell[ GUI_LAYOUT_COLS ];
        u32        n = gui()->carve( FORM, band, -1.0f, cell, GUI_LAYOUT_COLS );
        if ( n >= 4 )
        {
            /* Sidebar -- a stack of nav buttons. */
            gui()->push_layout_overlay( cell[ 0 ] );
                gui()->stack();
                gui()->button( "Nav A" );
                gui()->button( "Nav B" );
                gui()->button( "Nav C" );
            gui()->pop_layout();

            /* Header. */
            gui()->push_layout_overlay( cell[ 1 ] );
                gui()->stack();
                gui()->text( "Header" );
            gui()->pop_layout();

            /* Body. */
            gui()->push_layout_overlay( cell[ 2 ] );
                gui()->child_begin( "##body", 0.0f, 0.0f, 0 );   /* clip content to the body rect */
                    gui()->stack();
                    gui()->text( "Body content fills the middle." );
                    gui()->text( "The layout is one flat f32 form." );
                    gui()->text( "Each leaf is a plain gui_rect_t." );
                gui()->child_end();
            gui()->pop_layout();

            /* Footer. */
            gui()->push_layout_overlay( cell[ 3 ] );
                gui()->stack();
                gui()->text_disabled( "Footer" );
            gui()->pop_layout();
        }

        /* The panels used absolute rects, so the window pen has not moved -- reserve the band. */
        gui()->empty( 0.0f, band.h );
    }
    gui()->window_end();
}

/*==============================================================================================
    HUD Overlay -- free placement over one content area, the companion to split / carve.

    Every element takes the HUD rect and returns its own rect (no pen, no flow), so the order
    below is just draw order: a stretched top bar (anchor mixing per-axis stretch + point pin),
    corner-anchored minimap / health / ammo (gui_anchor_box), a fraction-pinned banner (anchor
    pivot), and a centered crosshair (gui_rect_align).  Real widgets drop into an anchored rect
    through push_layout_overlay -- the health bar is a stock progress_bar in a corner box.
==============================================================================================*/

static void
ex_layout_hud( void )
{
    if ( ex_begin( "HUD Overlay", 560, 460, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text_wrapped( "Overlay placement: every element positions itself inside one HUD rect via "
                             "gui_anchor_box (corners), gui()->anchor (stretch / fraction) and "
                             "gui_rect_align (center).  No layout pen -- draw order is z order." );

        /* The HUD viewport: a fixed-height band carved from the region's available area. */
        gui_rect_t hud = gui()->content_rect();
        hud.h = 260.0f;

        const gui_pad_t  pad   = { 10, 10, 10, 10 };
        const u32        back  = 0xC0141820;   /* ABGR: dark translucent backdrop  */
        const u32        panel = 0xE0283038;   /* a HUD panel fill                 */
        const u32        ink   = 0xFFE0E8F0;   /* near-white text                  */
        const u32        good  = 0xFF50C878;   /* health green                     */
        const u32        warn  = 0xFF30A0FF;   /* ammo amber                       */

        gui()->draw_rect( hud.x, hud.y, hud.w, hud.h, back );

        /* Top status bar -- one anchor, two axis behaviors: stretch across X (min.x 0 -> max.x 1, the
           off.l / off.r become margins), point-pin to the top on Y (min.y == max.y == 0, fixed height). */
        {
            gui_anchor_t a = { .min = { 0.0f, 0.0f }, .max = { 1.0f, 0.0f },
                               .size = { 0.0f, 22.0f }, .pivot = { 0.0f, 0.0f },
                               .off  = { 10, 10, 10, 0 } };
            gui_rect_t bar = gui()->anchor( hud, a );
            gui()->draw_rect( bar.x, bar.y, bar.w, bar.h, panel );
            gui()->draw_text_in( bar, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, ink, " Sector 7 - Clear" );
            gui()->draw_text_in( bar, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER, ink, "12:04  " );
        }

        /* Minimap -- fixed box anchored to the top-right corner with a uniform margin. */
        {
            gui_rect_t mm = gui_anchor_box( hud, 92.0f, 92.0f, GUI_ALIGN_RIGHT | GUI_ALIGN_TOP,
                                            ( gui_pad_t ){ 10, 42, 10, 10 } );
            gui()->draw_rect( mm.x, mm.y, mm.w, mm.h, panel );
            gui()->draw_circle( mm.x + mm.w * 0.5f, mm.y + mm.h * 0.5f, 5.0f, true, 0.0f, good );
            gui()->draw_text_in( mm, GUI_ALIGN_CENTER | GUI_ALIGN_BOTTOM, ink, "MAP" );
        }

        /* Health bar -- anchored bottom-left; a real progress_bar widget fills the anchored rect. */
        {
            gui_rect_t hb = gui_anchor_box( hud, 200.0f, 20.0f, GUI_ALIGN_LEFT | GUI_ALIGN_BOTTOM, pad );
            gui()->push_layout_overlay( hb );
                gui()->stack();
                gui()->push_style_color( GUI_ROLE_ACCENT, GUI_PHASE_IDLE, good );
                gui()->progress_bar( 0.72f, "HP 72/100" );
                gui()->pop_style_color( 1 );
            gui()->pop_layout();
        }

        /* Ammo readout -- anchored bottom-right, drawn directly. */
        {
            gui_rect_t am = gui_anchor_box( hud, 120.0f, 40.0f, GUI_ALIGN_RIGHT | GUI_ALIGN_BOTTOM, pad );
            gui()->draw_rect( am.x, am.y, am.w, am.h, panel );
            gui()->draw_text_in( am, GUI_ALIGN_CENTER, warn, "24 / 120" );
        }

        /* Wave banner -- point-anchored 50% across, near the top, hung off its own center (pivot 0.5)
           so it stays visually centered regardless of width. */
        {
            gui_anchor_t a = { .min = { 0.5f, 0.18f }, .max = { 0.5f, 0.18f },
                               .size = { 120.0f, 24.0f }, .pivot = { 0.5f, 0.5f } };
            gui_rect_t banner = gui()->anchor( hud, a );
            gui()->draw_rect( banner.x, banner.y, banner.w, banner.h, panel );
            gui()->draw_text_in( banner, GUI_ALIGN_CENTER, ink, "WAVE 3" );
        }

        /* Crosshair -- a fixed box centered in the HUD; gui_rect_align is the pure-center case. */
        {
            gui_rect_t cr = gui_rect_align( hud, 18.0f, 18.0f, GUI_ALIGN_CENTER );
            f32 cx = cr.x + cr.w * 0.5f, cy = cr.y + cr.h * 0.5f;
            gui()->draw_line( cx - 9.0f, cy, cx + 9.0f, cy, 2.0f, ink );
            gui()->draw_line( cx, cy - 9.0f, cx, cy + 9.0f, 2.0f, ink );
        }

        /* Placement used absolute rects, so reserve the band so the window sizes around it. */
        gui()->empty( 0.0f, hud.h );
    }
    gui()->window_end();
}

/*============================================================================================*/
