/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_style.c -- "Style" category demos.

    The theming machinery: named themes, the push/pop style stacks (colors + vars), the density
    scale ramp, the widget shape tags, and the font registry (load / push / measure / atlas
    preview).  Included by ex_demos.c (the root unity TU).

==============================================================================================*/

/*==============================================================================================
    Themes -- named gui_style_t snapshots; switching resets the push stacks app-wide.
==============================================================================================*/

static void
ex_style_themes( void )
{
    if ( ex_begin( "Themes", 400, 420, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "A theme is the root layer every push/pop" );
        gui()->text( "overrides against.  Switching applies to the" );
        gui()->text( "WHOLE app and clears the style stacks." );
        gui()->separator();

        u32                n      = 0;
        const gui_theme_t* themes = gui()->theme_list( &n );
        const char*        active = gui()->theme_get();
        gui()->textf( "active: %s", active ? active : "(anonymous -- style_get was edited)" );

        for ( u32 i = 0; i < n; i++ )
        {
            gui()->push_id_int( (i32)i );
            bool on = ( active && strcmp( active, themes[ i ].name ) == 0 );
            if ( gui()->selectable( themes[ i ].name, &on ) )
                gui()->theme_set( themes[ i ].name );
            gui()->pop_id();
        }

        gui()->separator();
        if ( gui()->button( "theme_reset (revert edits, clear stacks)" ) )
            gui()->theme_reset();

        /* style_peek reads the base without marking the theme anonymous. */
        const gui_style_t* st = gui()->style_peek();

        /* The AUTHORED half -- what the theme actually wrote.  The 32 cells below are not in the
           theme file at all; style_bake derives every one of them from these twelve numbers. */
        gui()->separator_text( "Seed palette (style_peek->palette)" );
        for ( u32 i = 0; i < GUI_SEED_COUNT; ++i )
        {
            u32 c = st->palette.seed[ i ];
            gui()->textf( "%-8s  %02X %02X %02X",
                          gui()->style_seed_name( (gui_style_seed_t)i ),
                          c & 0xFF, ( c >> 8 ) & 0xFF, ( c >> 16 ) & 0xFF );
        }
        for ( u32 i = 0; i < GUI_RAMP_COUNT; ++i )
            gui()->textf( "%-12s %.2f", gui()->style_ramp_name( (gui_style_ramp_t)i ),
                          st->palette.ramp[ i ] );

        gui()->separator_text( "Base metrics (style_peek)" );
        gui()->textf( "row %.0f   gap %.0f   pad %.0f",
                      st->var[ GUI_VAR_ROW ], st->var[ GUI_VAR_GAP ], st->var[ GUI_VAR_PAD ] );
        gui()->textf( "title_h %.0f   border %.0f   grid_q %.0f",
                      st->var[ GUI_VAR_TITLE_H ], st->var[ GUI_VAR_BORDER ], st->var[ GUI_VAR_GRID_Q ] );
        gui()->textf( "rounding: panel %.0f  widget %.0f",
                      st->var[ GUI_VAR_PANEL_ROUND ], st->var[ GUI_VAR_ROUND ] );
        gui()->text_disabled( "(a full live editor lives in sb_gui's Style Editor)" );
    }
    gui()->window_end();
}

/*==============================================================================================
    Style Stacks -- push/pop color + var overrides, one-shot next_*, and the scale ramp.
==============================================================================================*/

/* Axis names come from the engine rather than tables kept in step with the enums by hand --
   built once, since combo() wants a contiguous array of pointers. */
static const char**
ex_role_names( void )
{
    static const char* names[ GUI_ROLE_COUNT ];
    static bool        built = false;

    if ( !built )
    {
        for ( u32 i = 0; i < GUI_ROLE_COUNT; ++i )
            names[ i ] = gui()->style_role_name( ( gui_style_role_t )i );
        built = true;
    }
    return names;
}

/* The state list carries one extra entry past the four cells: GUI_PHASE_ALL, the whole-row push.
   Showing it in the same combo is the point -- picking "All" and picking "Hot" are the same
   verb with a different reach, not two different APIs. */
static const char**
ex_state_names( void )
{
    static const char* names[ GUI_PHASE_COUNT + 1 ];
    static bool        built = false;

    if ( !built )
    {
        for ( u32 i = 0; i < GUI_PHASE_COUNT; ++i )
            names[ i ] = gui()->style_phase_name( ( gui_style_phase_t )i );
        names[ GUI_PHASE_ALL ] = "All (whole row)";
        built = true;
    }
    return names;
}

/* Seed names, from the engine like the other axes. */
static const char**
ex_seed_names( void )
{
    static const char* names[ GUI_SEED_COUNT ];
    static bool        built = false;

    if ( !built )
    {
        for ( u32 i = 0; i < GUI_SEED_COUNT; ++i )
            names[ i ] = gui()->style_seed_name( ( gui_style_seed_t )i );
        built = true;
    }
    return names;
}

/* The var subset with obvious visual effect, plus a sensible slider range for each. */
typedef struct { const char* name; gui_style_var_t var; f32 lo, hi; } ex_var_row_t;

static const ex_var_row_t s_var_rows[] = {
    { "ROW",         GUI_VAR_ROW,         12.0f, 48.0f },
    { "GAP",         GUI_VAR_GAP,          0.0f, 24.0f },
    { "PAD",         GUI_VAR_PAD,          0.0f, 24.0f },
    { "ROUND",       GUI_VAR_ROUND,        0.0f, 16.0f },
    { "PANEL_ROUND", GUI_VAR_PANEL_ROUND,  0.0f, 24.0f },
    { "INDICATOR",   GUI_VAR_INDICATOR,    8.0f, 32.0f },
    { "GUTTER",      GUI_VAR_GUTTER,       4.0f, 32.0f },
};

#define EX_VAR_ROWS ( (i32)( sizeof( s_var_rows ) / sizeof( s_var_rows[ 0 ] ) ) )

static void
ex_style_stacks( void )
{
    /* col_on/col_sel/... must outlive this window's body: the push wraps a SECOND window
       (the sample), spawned after this one closes, so the pushed slot/value has to survive
       past window_end below. */
    static i32  role_sel     = GUI_ROLE_BG;
    static i32  state_sel    = GUI_PHASE_ALL;
    static f32  col_val[ 4 ] = { 0.8f, 0.2f, 0.2f, 1.0f };
    static bool col_on       = true;
    static i32  var_sel       = 0;
    static f32  var_val       = 8.0f;
    static bool var_on        = false;
    static i32  seed_sel      = GUI_SEED_ACCENT;
    static f32  seed_val[ 4 ] = { 0.78f, 0.59f, 0.24f, 1.0f };
    static bool seed_on       = false;

    if ( ex_begin( "Style Stacks", 440, 460, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "The overrides below bracket the WHOLE 'Style Stacks Sample'" );
        gui()->text( "window -- chrome, background, and every widget in it -- so" );
        gui()->text( "it is obvious at a glance what a slot does and does not touch." );
        if ( gui()->button( "Re-open sample window" ) )
            gui()->window_set_open( "Style Stacks Sample", true );

        /* --- color override: pick a slot + a color, pushed around the sample window --------- */
        gui()->separator_text( "push_style_color (scopes the sample window)" );
        gui()->checkbox( "Push the color", &col_on );
        gui()->combo( "role",  &role_sel,  ex_role_names(),  GUI_ROLE_COUNT );
        gui()->combo( "state", &state_sel, ex_state_names(), GUI_PHASE_COUNT + 1 );
        gui()->color_edit4( "color", col_val, GUI_COLOR_EDIT_NONE );

        /* --- var override: pick a metric + value ------------------------------------------- */
        gui()->separator_text( "push_style_var (scopes the sample window)" );
        gui()->checkbox( "Push the var", &var_on );
        static const char* var_names[ EX_VAR_ROWS ];
        for ( i32 i = 0; i < EX_VAR_ROWS; i++ ) var_names[ i ] = s_var_rows[ i ].name;
        gui()->combo( "var", &var_sel, var_names, EX_VAR_ROWS );
        gui()->slider_float( "value", &var_val, s_var_rows[ var_sel ].lo, s_var_rows[ var_sel ].hi );

        /* --- seed override: replace a SOURCE colour and re-derive the grid ----------------- */
        gui()->separator_text( "push_style_seed (scopes the sample window)" );
        gui()->text_disabled( "Turn this on with role=BG state=All above and compare:" );
        gui()->text_disabled( "PHASE_ALL flattens the row (hover dies), a seed re-derives it." );
        gui()->checkbox( "Push the seed", &seed_on );
        gui()->combo( "seed", &seed_sel, ex_seed_names(), GUI_SEED_COUNT );
        gui()->color_edit4( "seed color", seed_val, GUI_COLOR_EDIT_NO_ALPHA );

        /* --- next_style_color / next_style_var: one widget only, no pop -------------------- */
        gui()->separator_text( "next_style_color (one-shot)" );
        gui()->next_style_color( GUI_ROLE_BG, GUI_PHASE_ALL, GUI_COLOR( 0x20, 0x70, 0x30, 0xFF ) );
        gui()->button( "only this button is green" );
        gui()->button( "this one is stock again" );

        /* --- the scale ramp: the same widgets at each density step ------------------------- */
        gui()->separator_text( "scale_push: DENSE / STD / ROOMY / BAR" );
        static const char* scale_names[ GUI_SCALE_COUNT ] = { "DENSE", "STD", "ROOMY", "BAR" };
        for ( i32 s = 0; s < GUI_SCALE_COUNT; s++ )
        {
            gui()->push_id_int( s );
            gui()->scale_push( (gui_scale_t)s );
            if ( gui()->child_begin( "ramp", 0, 0, GUI_WIN_NOSCROLL ) )
            {
                gui()->stack();
                gui()->textf( "%s row: %.0f px", scale_names[ s ], gui()->sz_scale_row( (gui_scale_t)s ) );
                gui()->button( "button at this step" );
            }
            gui()->child_end();
            gui()->scale_pop();
            gui()->pop_id();
        }
    }
    gui()->window_end();

    /* --- the sample window: pushed BEFORE window_begin and popped AFTER window_end, so the
       override brackets the window's own chrome (the PANEL and BORDER roles) as well as every
       widget kind inside it -- a cell that only affects one interaction state, or only shows on
       a specific widget kind, is easy to tell apart from one that simply does not apply here.
       Picking role PANEL vs role BG is the clearest demonstration of the split the grid added:
       one repaints the window and its children, the other repaints the controls on them. */
    u32 abgr = GUI_COLOR( (u8)( col_val[ 0 ] * 255.0f ), (u8)( col_val[ 1 ] * 255.0f ),
                          (u8)( col_val[ 2 ] * 255.0f ), (u8)( col_val[ 3 ] * 255.0f ) );
    u32 seed_abgr = GUI_COLOR( (u8)( seed_val[ 0 ] * 255.0f ), (u8)( seed_val[ 1 ] * 255.0f ),
                               (u8)( seed_val[ 2 ] * 255.0f ), 0xFF );

    /* The seed push goes OUTSIDE the cell push, which is the nesting the verbs are built for:
       re-derive the whole grid from a new source, then disagree with the ramp on named cells
       inside it.  Reversing them would work too, but the re-bake would simply overwrite the
       cell push, which is exactly what "bulk" means. */
    if ( seed_on ) gui()->push_style_seed( (gui_style_seed_t)seed_sel, seed_abgr );
    if ( col_on ) gui()->push_style_color( (gui_style_role_t)role_sel, (gui_style_phase_t)state_sel, abgr );
    if ( var_on ) gui()->push_style_var( s_var_rows[ var_sel ].var, var_val );

    if ( ex_begin( "Style Stacks Sample", 380, 620, GUI_WIN_NONE ) )
    {
        gui()->stack();

        gui()->separator_text( "Text (COL_TEXT_IDLE vs COL_TEXT_DIM)" );
        gui()->text( "Plain text -- COL_TEXT_IDLE" );
        gui()->text_disabled( "Disabled/dim text -- COL_TEXT_DIM" );

        gui()->separator_text( "Buttons (COL_BG_IDLE / _HOT / _ACT, label = COL_TEXT_IDLE)" );
        gui()->button( "Sample button (hover/press me)" );
        static bool small_sb = true;
        gui()->small_button( "small" ); gui()->same_line( 8.0f );
        gui()->checkbox( "small_button's sibling", &small_sb );

        gui()->separator_text( "Checkbox / radio (box = COL_BG_IDLE, mark = COL_MARK_IDLE)" );
        static bool sb = true;
        gui()->checkbox( "Sample checkbox (label = COL_TEXT_IDLE)", &sb );
        static i32 mode = 0;
        gui()->radio_button( "A", &mode, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "B", &mode, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "C", &mode, 2 );

        gui()->separator_text( "Slider / input (field label = COL_TEXT_DIM, border = COL_BORDER_IDLE)" );
        static f32 sv = 5.0f;
        gui()->slider_float( "sample slider", &sv, 0.0f, 10.0f );
        static char stxt[ 24 ] = "sample";
        gui()->input_text( "sample input (click to focus)", stxt, sizeof( stxt ) );

        gui()->separator_text( "Combo / selectable (row hover/select = COL_BG_HOT/_ACT)" );
        static i32          combo_sel      = 0;
        static const char*  combo_items[]  = { "Alpha", "Beta", "Gamma" };
        gui()->combo( "sample combo", &combo_sel, combo_items, 3 );
        static bool sel_a = false, sel_b = true;
        gui()->selectable( "selectable row A", &sel_a );
        gui()->selectable( "selectable row B", &sel_b );

        gui()->separator_text( "Progress + child region (COL_ACCENT_IDLE / COL_PANEL_DIM)" );
        gui()->progress_bar( 0.66f, NULL );
        if ( gui()->child_begin( "sample child", 0, 60.0f, GUI_WIN_NONE ) )
        {
            gui()->stack();
            gui()->text( "child region body -- COL_PANEL_DIM" );
        }
        gui()->child_end();
    }
    gui()->window_end();

    if ( var_on ) gui()->pop_style_var( 1 );
    if ( col_on ) gui()->pop_style_color( 1 );
    if ( seed_on ) gui()->pop_style_seed( 1 );
}

/*==============================================================================================
    Widget Shape Tags -- the per-emit enum style vars that re-shape the chrome glyphs.
==============================================================================================*/

static void
ex_style_shape_tags( void )
{
    if ( ex_begin( "Widget Shape Tags", 420, 520, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Style tags force a shape on the widget emitting." );
        gui()->text( "Pushed here, so only the sample block re-shapes;" );
        gui()->text( "set_*_style would apply globally." );

        gui()->separator_text( "Tags" );
        static i32  check_idx   = 0;     /* 0 tick / 1 disc / 2 cross */
        static bool square_bull = false;
        static bool chevron     = false;
        static bool dashed      = false;
        static bool gradient    = false;
        static bool circle_knob = false;

        gui()->radio_button( "Tick",  &check_idx, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Disc",  &check_idx, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Cross", &check_idx, 2 );
        gui()->checkbox( "Square bullets",     &square_bull );
        gui()->checkbox( "Chevron arrows",     &chevron );
        gui()->checkbox( "Dashed separators",  &dashed );
        gui()->checkbox( "Gradient progress",  &gradient );
        gui()->checkbox( "Circle slider knob", &circle_knob );

        gui()->push_style_var( GUI_VAR_CHECK_SHAPE,     (f32)check_idx );
        gui()->push_style_var( GUI_VAR_BULLET_SHAPE,    square_bull ? 1.0f : 0.0f );
        gui()->push_style_var( GUI_VAR_ARROW_SHAPE,     chevron     ? 1.0f : 0.0f );
        gui()->push_style_var( GUI_VAR_SEPARATOR_SHAPE, dashed      ? 1.0f : 0.0f );
        gui()->push_style_var( GUI_VAR_PROGRESS_SHAPE,  gradient    ? 1.0f : 0.0f );
        gui()->push_style_var( GUI_VAR_KNOB_SHAPE,     circle_knob ? 1.0f : 0.0f );

        gui()->separator_text( "Sample widgets honoring the tags" );

        static bool a = true, b = false, c = true;
        gui()->checkbox( "Enabled", &a ); gui()->same_line( 12.0f );
        gui()->checkbox( "Visible", &b ); gui()->same_line( 12.0f );
        gui()->checkbox( "Locked",  &c );

        gui()->push_item_flag( GUI_ITEM_BUTTON_REPEAT, true );
        gui()->arrow_button( "##l", GUI_DIR_LEFT  ); gui()->same_line( -1.0f );
        gui()->arrow_button( "##r", GUI_DIR_RIGHT ); gui()->same_line( 12.0f );
        gui()->arrow_button( "##u", GUI_DIR_UP    ); gui()->same_line( -1.0f );
        gui()->arrow_button( "##d", GUI_DIR_DOWN  );
        gui()->pop_item_flag();

        gui()->separator();                                  /* solid or dashed per the tag */

        static f32 sval = 0.5f;
        gui()->slider_float( "Level", &sval, 0.0f, 1.0f );   /* bar or circle knob per the tag */
        gui()->progress_bar( 0.66f, NULL );                  /* solid or gradient per the tag  */
        gui()->bullet_text( "first bullet item" );
        gui()->bullet_text( "second bullet item" );

        if ( gui()->collapsing_header( "fold arrow follows the arrow tag" ) )
            gui()->text( "open" );

        gui()->pop_style_var( 6 );
    }
    gui()->window_end();
}

/*==============================================================================================
    Fonts -- the id-addressed registry: load, activate, push/pop, measure, atlas preview.
==============================================================================================*/

static void
ex_style_fonts( void )
{
    /* Load two extra faces once, exe-relative like the built-in presets.  font_load activates
       the new font, so remember + restore the active id around the loads. */
    static bool s_loaded  = false;
    static u32  s_id_cas  = 0;
    static u32  s_id_rob  = 0;
    if ( !s_loaded )
    {
        u32  prev = gui()->font_active_id();
        char dir[ 512 ];
        char path[ 576 ];
        sys_exe_dir( dir, (int)sizeof( dir ) );

        snprintf( path, sizeof( path ), "%s/../assets/font/CascadiaMono_16px.orb_font", dir );
        s_id_cas = gui()->font_load( path );
        snprintf( path, sizeof( path ), "%s/../assets/font/Roboto-Regular_16px.orb_font", dir );
        s_id_rob = gui()->font_load( path );

        gui()->font_use( prev );
        s_loaded = true;
    }

    if ( ex_begin( "Fonts", 460, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* Live font switch -- pick a face; the sample below re-renders in it live.  push_font
           brackets the block (font_use under the hood), so layout follows the selected metrics. */
        gui()->separator_text( "live font switch" );
        static i32 face = 0;
        gui()->radio_button( "slot 0 (boot)", &face, 0 ); gui()->same_line( -1.0f );
        if ( s_id_cas ) { gui()->radio_button( "cascadia", &face, 1 ); gui()->same_line( -1.0f ); }
        if ( s_id_rob )   gui()->radio_button( "roboto",   &face, 2 );
        if ( !s_id_cas && !s_id_rob )
            gui()->text_disabled( "(extra .orb_font loads failed -- check assets/font/)" );

        u32 face_id = face == 1 ? s_id_cas : face == 2 ? s_id_rob : 0;

        /* text_size measures in the active font, so it tracks the switch too. */
        static char probe[ 48 ] = "The quick brown fox jumps over the lazy dog";
        gui()->input_text( "string", probe, sizeof( probe ) );

        gui()->push_font( face_id );
        gui()->textf( "active font id: %u", gui()->font_active_id() );
        gui()->text( probe );
        gui()->text( "0123456789   ( ) [ ] { } < >   = + - * / & @ #" );
        gui_vec2_t sz = gui()->text_size( probe );
        gui()->textf( "text_size: %.1f x %.1f px   (sz_chars(10) = %.1f)",
                      sz.x, sz.y, gui()->sz_chars( 10.0f ) );
        gui()->pop_font();

        /* GPU atlas preview -- one shared R8 atlas holds every loaded font as a packed tenant, so
           this shows the SAME texture for any face (font_atlas_idx returns the shared bindless slot);
           the live text above is the real per-face test. */
        gui()->separator_text( "shared font atlas (font_atlas_idx)" );
        u32 tex     = gui()->font_atlas_idx( face_id );
        gui_vec2_t asz = gui()->font_atlas_size( face_id );
        gui()->textf( "atlas %u: %.0f x %.0f texels", tex, asz.x, asz.y );
        if ( tex && asz.x > 0.0f )
        {
            f32 w = gui()->content_avail().x;
            if ( w > asz.x ) w = asz.x;
            gui()->image_texture( tex, w, w * ( asz.y / asz.x ), 0 );
        }
    }
    gui()->window_end();
}

/*============================================================================================*/
