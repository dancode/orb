/*==============================================================================================

    sandbox/gui/sb_gui_bench/bench_scenes.c -- the shared scene emitters.

    Every case's workload is one of these, run once per frame.  Scenes are DETERMINISTIC: all
    per-item variety derives from a hash of the item index, and anything that animates derives
    from the runner's frame counter -- never from wall time -- so two runs of a case emit
    byte-identical command streams and their numbers compare.

    Scenes that must exercise the retained path are STATIC (identical commands every frame);
    the animated scene changes a value in every window every frame so no window can retain.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Deterministic variety
==============================================================================================*/

/* Knuth multiplicative hash -- all per-item variety (position, color, phase) derives here. */
static u32
bh_hash( u32 i )
{
    return i * 2654435761u;
}

/* Bright-ish opaque ABGR from three hash bytes. */
static u32
bh_color( u32 h )
{
    u32 r = 96 + ( ( h       ) & 127 );
    u32 g = 96 + ( ( h >> 8  ) & 127 );
    u32 b = 96 + ( ( h >> 16 ) & 127 );
    return 0xFF000000u | ( b << 16 ) | ( g << 8 ) | r;
}

/*==============================================================================================
    Widget walls -- one window, param rows of one widget kind (the per-widget emit price)
==============================================================================================*/

#define BENCH_WALL_MAX 2048

static bool s_wall_check[ BENCH_WALL_MAX ];
static f32  s_wall_value[ BENCH_WALL_MAX ];

/* Every row is emitted -- no rows_clip -- because the emit cost of the whole list IS the
   measurement; offscreen rows still run layout, id, and state work. */
static void
scene_wall_begin( void )
{
    gui()->window_set_next_pos ( 20.0f, 40.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 700.0f, 640.0f, GUI_COND_ONCE );
}

static void
scene_wall_buttons( const bench_case_t* c, u32 frame )
{
    UNUSED( frame );
    scene_wall_begin();
    if ( gui()->window_begin( "Bench Wall", GUI_WIN_NONE ) )
    {
        gui()->cols_n( 4 );
        for ( u32 i = 0; i < c->param; ++i )
        {
            gui()->push_id_int( ( i32 )i );
            if ( gui()->button( "poke" ) )
                s_wall_check[ i % BENCH_WALL_MAX ] = !s_wall_check[ i % BENCH_WALL_MAX ];
            gui()->pop_id();
        }
    }
    gui()->window_end();
}

static void
scene_wall_sliders( const bench_case_t* c, u32 frame )
{
    UNUSED( frame );
    scene_wall_begin();
    if ( gui()->window_begin( "Bench Wall", GUI_WIN_NONE ) )
    {
        gui()->cols_n( 2 );
        for ( u32 i = 0; i < c->param; ++i )
        {
            gui()->push_id_int( ( i32 )i );
            gui()->slider_float( "##s", &s_wall_value[ i % BENCH_WALL_MAX ], 0.0f, 1.0f );
            gui()->pop_id();
        }
    }
    gui()->window_end();
}

static void
scene_wall_labels( const bench_case_t* c, u32 frame )
{
    UNUSED( frame );
    scene_wall_begin();
    if ( gui()->window_begin( "Bench Wall", GUI_WIN_NONE ) )
    {
        gui()->cols_n( 4 );
        for ( u32 i = 0; i < c->param; ++i )
            gui()->textf( "label %04u", i );
    }
    gui()->window_end();
}

/* The realistic emit shape: a striped table, every row emitted. */
static void
scene_table_rows( const bench_case_t* c, u32 frame )
{
    UNUSED( frame );
    gui()->window_set_next_pos ( 20.0f, 40.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 700.0f, 640.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Bench Table", GUI_WIN_NONE ) )
    {
        gui()->stack();
        if ( gui()->table_begin( "##bench", 5,
                                 GUI_TABLE_BORDERS_OUTER | GUI_TABLE_BORDERS_V
                                 | GUI_TABLE_ROW_STRIPES | GUI_TABLE_SCROLL_Y, 0.0f ) )
        {
            gui()->table_setup_column( "Name",  GUI_TABLE_COL_STRETCH, 0.0f  );
            gui()->table_setup_column( "Id",    GUI_TABLE_COL_FIXED,   70.0f );
            gui()->table_setup_column( "Kind",  GUI_TABLE_COL_FIXED,   70.0f );
            gui()->table_setup_column( "Size",  GUI_TABLE_COL_FIXED,   80.0f );
            gui()->table_setup_column( "Crc",   GUI_TABLE_COL_FIXED,   90.0f );
            gui()->table_headers_row();

            static const char* k_kind[] = { "mesh", "tex", "sfx", "mat", "anim" };

            for ( u32 i = 0; i < c->param; ++i )
            {
                u32 h = bh_hash( i );
                gui()->table_next_row( 0.0f );
                gui()->table_next_column(); gui()->textf( "asset_%04u", i );
                gui()->table_next_column(); gui()->textf( "%u", h & 0xFFFF );
                gui()->table_next_column(); gui()->text ( k_kind[ h % 5 ] );
                gui()->table_next_column(); gui()->textf( "%u KB", ( h >> 8 ) % 4096 );
                gui()->table_next_column(); gui()->textf( "%08x", h );
            }
            gui()->table_end();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    The six-window scene -- static and animated twins

    The static form emits byte-identical commands every frame: with the retained cache on it is
    the diff-dominates workload, with the cache off the same frame prices full re-tessellation.
    The animated form changes one value in EVERY window each frame, so retention never engages
    and upload + submit carry the frame.
==============================================================================================*/

static bool s_six_check[ 24 ];
static f32  s_six_value[ 24 ];

static void
scene_six_windows( u32 frame, bool animated )
{
    for ( u32 w = 0; w < 6; ++w )
    {
        char title[ 32 ];
        fmt_snprintf( title, sizeof( title ), "Bench Win %u", w );

        gui()->window_set_next_pos ( 20.0f + ( f32 )( w % 3 ) * 320.0f,
                                     40.0f + ( f32 )( w / 3 ) * 330.0f, GUI_COND_ONCE );
        gui()->window_set_next_size( 300.0f, 310.0f, GUI_COND_ONCE );
        if ( gui()->window_begin( title, GUI_WIN_NONE ) )
        {
            gui()->stack();
            if ( animated )
            {
                /* One changing line + one changing fill per window -- the cheapest edit that
                   still dirties the whole window's command stream. */
                gui()->textf( "frame %u", frame );
                gui()->progress_bar( ( f32 )( frame % 120u ) * ( 1.0f / 120.0f ), NULL );
            }
            else
            {
                gui()->text( "static content" );
                gui()->progress_bar( 0.6f, NULL );
            }

            switch ( w % 3 )
            {
                case 0:
                    gui()->cols_n( 2 );
                    for ( u32 i = 0; i < 8; ++i )
                    {
                        gui()->push_id_int( ( i32 )( w * 8 + i ) );
                        if ( i & 1 ) gui()->checkbox( "opt", &s_six_check[ ( w * 4 + i / 2 ) % 24 ] );
                        else if ( gui()->small_button( "act" ) )
                            s_six_check[ w % 24 ] = !s_six_check[ w % 24 ];
                        gui()->pop_id();
                    }
                    break;

                case 1:
                    gui()->stack();
                    for ( u32 i = 0; i < 6; ++i )
                    {
                        gui()->push_id_int( ( i32 )( w * 8 + i ) );
                        gui()->slider_float( "##s", &s_six_value[ ( w * 4 + i ) % 24 ], 0.0f, 1.0f );
                        gui()->pop_id();
                    }
                    break;

                default:
                {
                    /* A drawn panel: shapes from the catalog, positions hashed, all static. */
                    gui_rect_t cv = gui()->canvas( 180.0f );
                    gui()->push_clip( cv.x, cv.y, cv.w, cv.h );
                    for ( u32 i = 0; i < 24; ++i )
                    {
                        u32 h = bh_hash( w * 131u + i );
                        f32 x = cv.x + ( f32 )( h % 256 )          * ( 1.0f / 256.0f ) * cv.w;
                        f32 y = cv.y + ( f32 )( ( h >> 8 ) % 256 ) * ( 1.0f / 256.0f ) * cv.h;
                        f32 r = 4.0f + ( f32 )( ( h >> 16 ) & 7 );
                        if ( i & 1 ) gui()->draw_circle( x, y, r, 1.5f, bh_color( h ) );
                        else         gui()->draw_rect  ( x, y, r * 2.0f, r * 2.0f, bh_color( h ) );
                    }
                    gui()->pop_clip();
                    break;
                }
            }
        }
        gui()->window_end();
    }
}

static void
scene_static_six( const bench_case_t* c, u32 frame )
{
    UNUSED( c ); UNUSED( frame );
    scene_six_windows( 0, false );
}

static void
scene_animated_six( const bench_case_t* c, u32 frame )
{
    UNUSED( c );
    scene_six_windows( frame, true );
}

/*==============================================================================================
    Text walls -- equal glyph count under four draw paths

    120 lines of the same 80-character string: ~8000 glyph decodes and quads per frame whatever
    the mode, so quad_count in the report confirms parity and the deltas are the path's price.
    The count is chosen to sit WELL UNDER GUI_MAX_QUADS -- every glyph here is on-screen, so
    unlike the widget walls nothing culls, and a taller wall trips the pool overflow (which
    sb_gui_stress owns; this bench prices the shipping pipeline inside its caps).
==============================================================================================*/

#define BENCH_TEXT_LINES 120u

/* The SDF face for the outline mode; 0 = bake not found and the outline case is skipped.
   Loaded once after boot by bench_assets_init (font_load activates, so it restores). */
static u32 s_bench_font_sdf = 0;

static const char*
bench_text_line( void )
{
    static char s_line[ 81 ];
    if ( s_line[ 0 ] == '\0' )
    {
        static const char* k_src = "the quick brown fox jumps over the lazy dog 0123456789 ";
        u32 n = ( u32 )strlen( k_src );
        for ( u32 i = 0; i < 80; ++i )
            s_line[ i ] = k_src[ i % n ];
        s_line[ 80 ] = '\0';
    }
    return s_line;
}

typedef enum
{
    BENCH_TEXT_PLAIN = 0,
    BENCH_TEXT_CLIPPED,
    BENCH_TEXT_XF,
    BENCH_TEXT_OUTLINE,

} bench_text_mode_t;

static void
scene_text_wall( const bench_case_t* c, u32 frame )
{
    UNUSED( frame );
    const char* line = bench_text_line();

    gui()->window_set_next_pos ( 20.0f, 40.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 940.0f, 660.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Bench Text", GUI_WIN_NONE ) )
    {
        gui()->stack();
        f32        avail = gui()->view_avail().y;
        gui_rect_t cv    = gui()->canvas( avail > 40.0f ? avail : 40.0f );

        /* EVERY mode runs in the same SDF face when the bake exists, not just the outline --
           otherwise the outline row would be comparing a 32 px field font against the 16 px
           boot font and the delta would mostly be pixel coverage, not the path. */
        bool sdf     = ( s_bench_font_sdf != 0 );
        bool outline = ( c->param == BENCH_TEXT_OUTLINE ) && sdf;
        if ( sdf )
            gui()->push_font( s_bench_font_sdf );
        if ( outline )
            gui()->draw_set_text_edge( 2.0f, 0xFF000000u );

        gui()->push_clip( cv.x, cv.y, cv.w, cv.h );
        f32 step = cv.h / ( f32 )BENCH_TEXT_LINES;
        for ( u32 i = 0; i < BENCH_TEXT_LINES; ++i )
        {
            f32 y   = cv.y + ( f32 )i * step;
            u32 col = bh_color( bh_hash( i ) );

            switch ( ( bench_text_mode_t )c->param )
            {
                case BENCH_TEXT_CLIPPED:
                    gui()->draw_text_clipped( ( gui_rect_t ){ cv.x, y, cv.w, step },
                                              GUI_ALIGN_LEFT, col, line );
                    break;
                case BENCH_TEXT_XF:
                    gui()->draw_text_xf( cv.x, y, col, line, 1.0f, 0.15f );
                    break;
                case BENCH_TEXT_OUTLINE:   /* falls back to plain when the bake is missing */
                default:
                    gui()->draw_text( cv.x, y, col, line );
                    break;
            }
        }
        gui()->pop_clip();

        if ( outline )
            gui()->draw_set_text_edge( 0.0f, 0u );
        if ( sdf )
            gui()->pop_font();
    }
    gui()->window_end();
}

/*==============================================================================================
    The composite scene -- the style suite's shared workload

    Three windows of everyday chrome: a form, a list, a reading pane.  Static values, so the
    geometry is stable and the style variants change only how paint lands -- gpu_ms and the
    quad / style-record counts are the readout.
==============================================================================================*/

static bool s_comp_check[ 8 ];
static f32  s_comp_value[ 8 ];

static void
scene_composite( const bench_case_t* c, u32 frame )
{
    UNUSED( c ); UNUSED( frame );

    gui()->window_set_next_pos ( 20.0f, 40.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 360.0f, 560.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Bench Form", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->separator_text( "actions" );
        gui()->cols_n( 4 );
        for ( u32 i = 0; i < 20; ++i )
        {
            gui()->push_id_int( ( i32 )i );
            if ( gui()->button( "do" ) )
                s_comp_check[ i % 8 ] = !s_comp_check[ i % 8 ];
            gui()->pop_id();
        }
        gui()->stack();
        gui()->separator_text( "values" );
        for ( u32 i = 0; i < 6; ++i )
        {
            gui()->push_id_int( ( i32 )( 100 + i ) );
            gui()->slider_float( "##s", &s_comp_value[ i % 8 ], 0.0f, 1.0f );
            gui()->pop_id();
        }
        gui()->separator_text( "options" );
        gui()->cols_n( 2 );
        for ( u32 i = 0; i < 6; ++i )
        {
            gui()->push_id_int( ( i32 )( 200 + i ) );
            gui()->checkbox( "opt", &s_comp_check[ i % 8 ] );
            gui()->pop_id();
        }
        gui()->stack();
        gui()->progress_bar( 0.7f, NULL );
    }
    gui()->window_end();

    gui()->window_set_next_pos ( 400.0f, 40.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 420.0f, 560.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Bench List", GUI_WIN_NONE ) )
    {
        gui()->stack();
        if ( gui()->table_begin( "##list", 4,
                                 GUI_TABLE_BORDERS_OUTER | GUI_TABLE_BORDERS_V
                                 | GUI_TABLE_ROW_STRIPES, 0.0f ) )
        {
            gui()->table_setup_column( "Item",  GUI_TABLE_COL_STRETCH, 0.0f  );
            gui()->table_setup_column( "Id",    GUI_TABLE_COL_FIXED,   60.0f );
            gui()->table_setup_column( "State", GUI_TABLE_COL_FIXED,   70.0f );
            gui()->table_setup_column( "Crc",   GUI_TABLE_COL_FIXED,   80.0f );
            gui()->table_headers_row();
            for ( u32 i = 0; i < 12; ++i )
            {
                u32 h = bh_hash( i );
                gui()->table_next_row( 0.0f );
                gui()->table_next_column(); gui()->textf( "entry_%02u", i );
                gui()->table_next_column(); gui()->textf( "%u", h & 0xFFF );
                gui()->table_next_column(); gui()->text ( ( h & 1 ) ? "live" : "cold" );
                gui()->table_next_column(); gui()->textf( "%06x", h & 0xFFFFFF );
            }
            gui()->table_end();
        }
        if ( gui()->collapsing_header( "details" ) )
        {
            gui()->textf( "twelve entries, four columns" );
            gui()->textf( "striped, bordered, static" );
        }
    }
    gui()->window_end();

    gui()->window_set_next_pos ( 860.0f, 40.0f, GUI_COND_ONCE );
    gui()->window_set_next_size( 380.0f, 560.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( "Bench Reading", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->separator_text( "body text" );
        for ( u32 i = 0; i < 24; ++i )
            gui()->textf( "%02u  the quick brown fox jumps over it", i );
        gui()->separator();
        gui()->text_wrapped( "A wrapped closing paragraph long enough to break across several "
                             "lines, so the wrap path is part of the styled workload too." );
    }
    gui()->window_end();
}

// clang-format on
/*============================================================================================*/
