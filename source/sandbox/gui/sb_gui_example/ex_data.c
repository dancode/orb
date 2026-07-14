/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_data.c -- "Data" category demos.

    Tables (flags, sortable headers, widgets in cells, background tints) and the debug / stats
    surface (overlay layers, render modes, the cache levers, render + memory statistics).
    Included by ex_demos.c (the root unity TU).

==============================================================================================*/

/*==============================================================================================
    Tables -- flags toggled live, built-in sorting, interactive cells, index readouts.
==============================================================================================*/

/* Row record for the sortable table; the accessor hands the table one cell's sort key. */
typedef struct { const char* name; const char* kind; f32 value; } ex_item_t;

static void
ex_item_sort_value( i32 row, i32 col, gui_table_sort_value_t* out, void* user )
{
    const ex_item_t* items = (const ex_item_t*)user;
    if ( col == 0 )      out->str = items[ row ].name;
    else if ( col == 1 ) out->str = items[ row ].kind;
    else               { out->num = items[ row ].value; out->is_num = true; }
}

static void
ex_data_tables( void )
{
    if ( ex_begin( "Tables", 520, 700, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* --- the flag suite ----------------------------------------------------------------- */
        gui()->separator_text( "gui_table_flags_t (applied below)" );
        static u32 tflags = GUI_TABLE_SORTABLE | GUI_TABLE_ROW_STRIPES | GUI_TABLE_RESIZABLE |
                            GUI_TABLE_BORDERS_V | GUI_TABLE_BORDERS_OUTER | GUI_TABLE_SCROLL_Y;
        gui()->row_cols_n( 0, 2 );
        gui()->push_layout();
            gui()->stack();
            ex_flag_checkbox( "BORDERS_H",     &tflags, GUI_TABLE_BORDERS_H );
            ex_flag_checkbox( "BORDERS_V",     &tflags, GUI_TABLE_BORDERS_V );
            ex_flag_checkbox( "BORDERS_OUTER", &tflags, GUI_TABLE_BORDERS_OUTER );
            ex_flag_checkbox( "SCROLL_Y",      &tflags, GUI_TABLE_SCROLL_Y );
        gui()->pop_layout();
        gui()->push_layout();
            gui()->stack();
            ex_flag_checkbox( "SORTABLE",      &tflags, GUI_TABLE_SORTABLE );
            ex_flag_checkbox( "ROW_STRIPES",   &tflags, GUI_TABLE_ROW_STRIPES );
            ex_flag_checkbox( "RESIZABLE",     &tflags, GUI_TABLE_RESIZABLE );
            ex_flag_checkbox( "NO_HEADER",     &tflags, GUI_TABLE_NO_HEADER );
        gui()->pop_layout();
        gui()->row( 0 );

        static i32 theight = 180;
        gui()->slider_int( "body height (SCROLL_Y)", &theight, 80, 320 );

        /* --- sortable three-column table ---------------------------------------------------- */
        gui()->separator_text( "sortable data (click the headers)" );

        static const ex_item_t k_items[] = {
            { "pos_x",   "float",  1.234f   },
            { "pos_y",   "float",  -5.678f  },
            { "pos_z",   "float",  0.0f     },
            { "vel_x",   "float",  -0.5f    },
            { "vel_y",   "float",  2.0f     },
            { "health",  "int",    100.0f   },
            { "shield",  "int",    42.0f    },
            { "armor",   "int",    7.0f     },
            { "speed",   "float",  9.81f    },
            { "stamina", "float",  -3.5f    },
            { "alive",   "bool",   1.0f     },
            { "frozen",  "bool",   0.0f     },
            { "level",   "int",    12.0f    },
            { "score",   "int",    31337.0f },
        };
        const i32 k_item_count = (i32)( sizeof( k_items ) / sizeof( k_items[ 0 ] ) );

        static i32  s_order[ 32 ];
        static bool s_order_init = false;
        if ( !s_order_init )
        {
            for ( i32 i = 0; i < k_item_count; ++i ) s_order[ i ] = i;
            s_order_init = true;
        }

        f32 body_h = ( tflags & GUI_TABLE_SCROLL_Y ) ? (f32)theight : 0.0f;
        if ( gui()->table_begin( "props", 3, (gui_table_flags_t)tflags, body_h ) )
        {
            gui()->table_setup_column( "Name",  GUI_TABLE_COL_STRETCH,   0     );
            gui()->table_setup_column( "Type",  GUI_TABLE_COL_FIXED,     64.0f );
            gui()->table_setup_column( "Value", GUI_TABLE_COL_FIXED,    128.0f );
            if ( !( tflags & GUI_TABLE_NO_HEADER ) )
                gui()->table_headers_row();

            /* Built-in sort: reorder s_order to match the active header click. */
            gui()->table_sort_order( s_order, k_item_count, ex_item_sort_value, NULL, (void*)k_items );

            for ( i32 r = 0; r < k_item_count; ++r )
            {
                const i32 i = s_order[ r ];
                gui()->table_next_row( 0 );
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    gui()->text( k_items[ i ].name );
                }
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    gui()->text_disabled( k_items[ i ].kind );
                }
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    /* Tint the cell red when the value is negative (CELL bg override). */
                    if ( k_items[ i ].value < 0.0f )
                        gui()->table_set_bg_color( GUI_TABLE_BG_CELL,
                                                   GUI_COLOR( 0xC0, 0x30, 0x30, 0x60 ) );
                    char buf[ 24 ];
                    snprintf( buf, sizeof( buf ), "%.3g", k_items[ i ].value );
                    gui()->text( buf );
                }
            }

            /* Raw sort state -- the do-it-yourself alternative to table_sort_order. */
            gui_table_sort_specs_t specs;
            gui()->table_get_sort_specs( &specs );
            gui()->table_end();
            gui()->textf( "sort: col %d %s   (cols=%d)", specs.col,
                          specs.col < 0 ? "" : ( specs.descending ? "desc" : "asc" ),
                          3 );
        }

        /* --- interactive cells --------------------------------------------------------------- */
        gui()->separator_text( "interactive cells (no header)" );

        static f32 s_vals[ 4 ] = { 0.25f, 0.5f, 0.75f, 1.0f };
        static const char* k_labels[] = { "Alpha", "Beta", "Gamma", "Delta" };

        if ( gui()->table_begin( "sliders", 2, GUI_TABLE_BORDERS_H, 0.0f ) )
        {
            gui()->table_setup_column( "Label",  GUI_TABLE_COL_FIXED,  60.0f );
            gui()->table_setup_column( "Slider", GUI_TABLE_COL_STRETCH, 0    );

            for ( i32 i = 0; i < 4; ++i )
            {
                gui()->table_next_row( 0 );
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    gui()->text( k_labels[ i ] );
                }
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    gui()->push_id_int( i );
                    gui()->slider_float( "##v", &s_vals[ i ], 0.0f, 1.0f );
                    gui()->pop_id();
                }
            }
            gui()->table_end();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    Debug & Stats -- overlay layers, render modes, the cache levers, live statistics.
==============================================================================================*/

static void
ex_data_debug( void )
{
    if ( ex_begin( "Debug & Stats", 460, 700, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->textf( "debug hotkey driver: %s", gui()->debug_is_enabled() ? "armed" : "off" );
        gui()->text_disabled( "F1-F5 layers  F9 render mode  F10 dashboard" );
        gui()->text_disabled( "P perf  O state  C retained  F force  I idle" );

        /* Overlay layers -- Debug builds only (no-op + zero in Release). */
        gui()->separator_text( "debug overlay layers (Debug builds)" );
        u32 layers = gui()->debug_get_layers();
        u32 prev   = layers;
        ex_flag_checkbox( "WINDOW (frames)",          &layers, GUI_DBG_WINDOW );
        ex_flag_checkbox( "INTERACT (widget rects)",  &layers, GUI_DBG_INTERACT );
        ex_flag_checkbox( "RESIZE (grab bands)",      &layers, GUI_DBG_RESIZE );
        ex_flag_checkbox( "CLIP (scissor stack)",     &layers, GUI_DBG_CLIP );
        ex_flag_checkbox( "LAYOUT (allocated space)", &layers, GUI_DBG_LAYOUT );
        if ( layers != prev )
            gui()->debug_set_layers( layers );

        /* Render mode -- live in every build. */
        gui()->separator_text( "render mode" );
        i32 mode = (i32)gui()->debug_get_render_mode();
        bool mode_changed = false;
        mode_changed |= gui()->radio_button( "Normal",    &mode, GUI_RENDER_NORMAL );    gui()->same_line( -1.0f );
        mode_changed |= gui()->radio_button( "Wireframe", &mode, GUI_RENDER_WIREFRAME ); gui()->same_line( -1.0f );
        mode_changed |= gui()->radio_button( "Batch",     &mode, GUI_RENDER_BATCH );
        if ( mode_changed )
            gui()->debug_set_render_mode( (gui_render_mode_t)mode );

        /* The three cache / pacing levers. */
        gui()->separator_text( "levers" );
        bool retained = gui()->retained_skip();
        if ( gui()->checkbox( "retained skip (render-side geometry cache)", &retained ) )
            gui()->set_retained_skip( retained );
        bool force = gui()->force_redraw();
        if ( gui()->checkbox( "force redraw (pin frame_dirty true)", &force ) )
            gui()->set_force_redraw( force );
        bool idle = gui()->idle_skip();
        if ( gui()->checkbox( "idle skip (block on OS input when static)", &idle ) )
            gui()->set_idle_skip( idle );
        gui()->textf( "frame_dirty this frame: %d   wants_redraw: %d",
                      gui()->frame_dirty(), gui()->wants_redraw() );

        /* Render statistics -- previous frame's totals (published at frame_begin). */
        gui()->separator_text( "render_stats (last frame)" );
        gui_render_stats_t rs = gui()->render_stats();
        gui()->textf( "cmds %u   verts %u   tris %u   draw calls %u",
                      rs.cmd_count, rs.vert_count, rs.tri_count, rs.draw_calls );
        gui()->textf( "windows retained %u / %u", rs.win_retained, rs.win_total );
        gui()->textf( "verts retained %u   tris retained %u", rs.vert_retained, rs.tri_retained );
        gui()->textf( "uploads: %u batches, %u bytes", rs.upload_batches, rs.upload_bytes );
        gui()->textf( "volatile patched: %u", rs.volatile_patched );

        /* Memory footprint -- exact resident totals per bucket. */
        gui()->separator_text( "mem_stats" );
        gui_mem_stats_t ms = gui()->mem_stats();
        gui()->textf( "GPU: %u KB (%u viewports)", ms.gpu_total / 1024, ms.viewport_count );
        gui()->textf( "CPU static (.bss): %u KB", ms.cpu_static_total / 1024 );
        gui()->textf( "CPU heap: %u KB (%u contexts)", ms.cpu_dynamic_total / 1024, ms.context_count );
        gui()->textf( "TOTAL: %u KB", ms.total_bytes / 1024 );

        gui()->row_cols_n( 0, 2 );
        if ( gui()->button( "print_mem_stats -> stdout" ) )
            gui()->print_mem_stats();
        if ( gui()->button( "debug_dump_geometry -> stdout" ) )
            gui()->debug_dump_geometry();
        gui()->row( 0 );
    }
    gui()->window_end();
}

/*============================================================================================*/
