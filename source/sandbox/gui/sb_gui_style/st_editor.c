/*==============================================================================================

    sandbox/gui/sb_gui_style/st_editor.c -- Style Editor window: tune the live style.

    Hoisted out of sb_gui (which is the ImGui-demo replication, not a style bench) into the
    demo that owns look customization.  Unity-included by sb_gui_style.c.

    Pick a built-in theme, then tune every SEED / RAMP / CELL / VAR knob live.  The flow is the
    one the gui style API is built around: theme_list/theme_set switch named presets; style_peek
    reads the base style WITHOUT marking it anonymous (so the theme combo keeps naming the active
    theme until an edit lands); style_get commits an edit (theme goes "(custom)"); style_apply
    rescales the active metrics.  Widgets edit a local copy of the base style and the whole copy
    is committed once per frame only when something actually changed -- so merely opening the
    window never disturbs the theme.

    Nothing here names an individual colour or var: the editor walks the engine's own name
    tables (style_seed_name / _ramp_name / _role_name / _phase_name / _var_name / _var_class /
    _class_name), so a new seed, role, phase or var shows up with no edit to this file.  The one
    thing still authored per var is a SHAPE pick's value names, which the engine does not own.

==============================================================================================*/
// clang-format off

/*============================================================================================*/
/* Knob helpers -- one style field each, returning true on edit.                                */
/*============================================================================================*/

/* One color slot -> a color_edit4 bound to the packed u32 field. */
static bool
se_color( const char* label, u32* field )
{
    f32 c[ 4 ] = {
        (f32)(   *field         & 0xFF ) / 255.0f,
        (f32)( ( *field >> 8  ) & 0xFF ) / 255.0f,
        (f32)( ( *field >> 16 ) & 0xFF ) / 255.0f,
        (f32)( ( *field >> 24 ) & 0xFF ) / 255.0f,
    };
    if ( gui()->color_edit4( label, c, GUI_COLOR_EDIT_NONE ) )
    {
        u8 r = (u8)( c[ 0 ] * 255.0f + 0.5f ), g = (u8)( c[ 1 ] * 255.0f + 0.5f );
        u8 b = (u8)( c[ 2 ] * 255.0f + 0.5f ), a = (u8)( c[ 3 ] * 255.0f + 0.5f );
        *field = GUI_COLOR( r, g, b, a );
        return true;
    }
    return false;
}

/* One scalar -> a slider over [lo,hi]. */
static bool
se_f32( const char* label, f32* field, f32 lo, f32 hi )
{
    return gui()->slider_float( label, field, lo, hi );
}

/* One px-unit scalar -> an INTEGER slider bound to the f32 field.  Pixel metrics are authored
   as whole pixels at em=12 (the lattice snaps them anyway), so a float track only makes the
   values people actually want -- 4, 8, 16 -- hard to land on. */
static bool
se_px( const char* label, f32* field, i32 lo, i32 hi )
{
    i32 v = (i32)( *field + 0.5f );
    if ( gui()->slider_int( label, &v, lo, hi ) )
    {
        *field = (f32)v;
        return true;
    }
    return false;
}

/* One px-unit scalar -> an integer drag field bound to the f32 field -- se_px's drag twin, for
   the density-ramp triples where a slider per cell would crowd the row. */
static bool
se_px_drag( const char* label, f32* field )
{
    i32 v = (i32)( *field + 0.5f );
    if ( gui()->drag_int( label, &v, 0.25f, 0, 64, NULL ) )
    {
        *field = (f32)v;
        return true;
    }
    return false;
}

/* One enum-valued var -> a combo of named variants. */
static bool
se_shape( gui_style_var_t var, f32* vars, const char* const* names, i32 count )
{
    i32  cur     = (i32)vars[ var ];
    bool changed = false;
    if ( cur < 0 || cur >= count ) cur = 0;

    if ( gui()->combo_begin( gui()->style_var_name( var ), names[ cur ], GUI_COMBO_NONE ) )
    {
        for ( i32 i = 0; i < count; ++i )
        {
            bool sel = ( i == cur );
            if ( gui()->selectable( names[ i ], &sel ) )
            {
                vars[ var ] = (f32)i;
                changed     = true;
            }
        }
        gui()->combo_end();
    }
    return changed;
}

/*============================================================================================*/
/* Shape value names -- the one per-var table the engine does not publish.                      */
/*                                                                                              */
/* Shared with st_export.c, which emits these same picks as their enum identifiers: index i of  */
/* a names row is the enum value, so the two tables stay addressable by the same i.             */
/*============================================================================================*/

static const char* const se_nm_check   [] = { "Tick", "Disc", "Cross" };
static const char* const se_nm_bullet  [] = { "Disc", "Square" };
static const char* const se_nm_arrow   [] = { "Filled", "Chevron" };
static const char* const se_nm_sep     [] = { "Solid", "Dashed" };
static const char* const se_nm_progress[] = { "Solid", "Gradient" };
static const char* const se_nm_knob    [] = { "Bar", "Circle" };
static const char* const se_nm_menu    [] = { "Plain", "Box" };

/*============================================================================================*/
/* Window                                                                                       */
/*============================================================================================*/

static void
st_editor_window( void )
{
    if ( !st_begin( "Style Editor", 340.0f, 620.0f ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* --- Theme -----------------------------------------------------------------------------
       Theme controls go FIRST so a preset switch happens before we snapshot the base style
       below -- the tuning widgets then reflect the newly selected theme the same frame. */
    gui()->separator_text( "Theme" );

    const char* active = gui()->theme_get();            /* NULL after an anonymous edit */
    u32         n_themes;
    const gui_theme_t* themes = gui()->theme_list( &n_themes );

    /* [ combo (fill) | Reset (fixed) ] as an explicit two-track row, NOT combo + same_line(button):
       a fill widget leaves the pen at the region's right edge, so a same_line natural-width button
       reaches past content_w and (content_w chasing that) the row grows without bound each frame.
       A fixed second track parks the button in a content_w-independent cell instead.  The combo hides
       its own label ("##") since the separator above already titles the section. */
    f32 reset_w = gui()->button_width( "Reset" );
    gui()->row_cols( 0.0f, (f32[]){ 1.0f, reset_w, GUI_END } );

    if ( gui()->combo_begin( "##Theme", active ? active : "(custom)", GUI_COMBO_NONE ) )
    {
        for ( u32 i = 0; i < n_themes; ++i )
        {
            bool sel = ( active && strcmp( active, themes[ i ].name ) == 0 );
            if ( gui()->selectable( themes[ i ].name, &sel ) )
                gui()->theme_set( themes[ i ].name );
        }
        gui()->combo_end();
    }

    if ( gui()->button( "Reset" ) )
        gui()->theme_reset();   /* revert to the active theme's authored values, clear stacks */

    gui()->stack();   /* back to a single full-width column for the rest of the panel */

    if ( !active )
        gui()->text_disabled( "Edited -- pick a theme above to revert, or Style Export to keep it." );

    /* --- Snapshot --------------------------------------------------------------------------
       Read the base through style_peek (does not disturb the theme name).  Widgets edit this
       local copy; the whole copy is committed once at the end only if something changed. */
    gui_style_t work    = *gui()->style_peek();
    bool        changed = false;

    f32 label_width = gui()->text_size( "Separator Shape" ).x;
    gui()->form( GUI_LABEL_RIGHT, label_width );

    /* --- Palette: the AUTHORED colour, and the fastest knob in the panel --------------------
       Seventeen numbers that drive all 48 cells below.  Drag one ramp slider and the entire grid
       re-derives in the same frame -- which is the whole argument for the bake, made visible:
       the cells are a projection of this, not a parallel thing to keep in step with it.

       The re-bake is gated on the PALETTE changing rather than on `changed`, and the ordering
       matters both ways: it runs before the grid section so the swatches show freshly derived
       values this frame, and it does not run when only a cell changed, so a hand-edit below
       survives instead of being eaten on the next keystroke. */
    bool palette_changed = false;

    gui()->separator_text( "Palette -- seeds" );
    for ( u32 i = 0; i < GUI_SEED_COUNT; ++i )
        palette_changed |= se_color( gui()->style_seed_name( ( gui_style_seed_t )i ),
                                     &work.palette.seed[ i ] );

    gui()->separator_text( "Palette -- ramp" );
    for ( u32 i = 0; i < GUI_RAMP_COUNT; ++i )
    {
        /* NEST is the one signed ramp -- its sign is the direction a nested surface steps, so its
           track has to reach below zero for the lift half to be reachable at all. */
        const f32 lo = ( i == GUI_RAMP_NEST ) ? -1.0f : 0.0f;

        palette_changed |= se_f32( gui()->style_ramp_name( ( gui_style_ramp_t )i ),
                                   &work.palette.ramp[ i ], lo, 1.0f );
    }

    if ( palette_changed )
    {
        gui()->style_bake( &work );
        changed = true;
    }

    /* --- Colors: the DERIVED grid, a section per role ---------------------------------------
       Editing a cell here is legitimate and sticks: it is the "bake, then disagree" shape a kit
       uses, just spelled interactively.  Touch a seed or a ramp value above and the disagreement
       is overwritten, because that is what re-deriving means -- and Style Export knows the
       difference, emitting exactly these survivors as post-bake overrides.

       No SELECTED section: a selected read washes one of these cells live (style_col_selected),
       so there is nothing selection-specific to tune here beyond the GUI_RAMP_SELECT slider
       already in the ramp section above. */
    for ( u32 r = 0; r < GUI_ROLE_COUNT; ++r )
    {
        gui()->separator_text( gui()->style_role_name( ( gui_style_role_t )r ) );
        gui()->push_id( gui()->style_role_name( ( gui_style_role_t )r ) );
        for ( u32 s = 0; s < GUI_PHASE_COUNT; ++s )
            changed |= se_color( gui()->style_phase_name( ( gui_style_phase_t )s ),
                                 &work.col[ r ][ s ] );
        gui()->pop_id();
    }

    /* --- Scalars: a section per var CLASS, both walked from the engine -----------------------
       Asking each var for its class is what replaced two hardcoded enum ranges, so a new var
       appears in the right section on its own.  Shapes are skipped: a pick wants a combo over its
       value names, which the engine does not own -- that is the one thing authored above. */
    for ( u32 c = 0; c < GUI_CLASS_COUNT; ++c )
    {
        if ( c == GUI_CLASS_SHAPE ) continue;

        /* UNIT comes off the CLASS (px classes -- metric, stroke, skin, pitch -- edit as whole
           pixels on an integer slider; ratio and rate stay float), the RANGE off the var's own
           schema ceiling (style_var_max), so a border slider spans the few px a border can be
           rather than a shared 64px track.  Both asked of the engine, tabulated nowhere here. */
        bool px = ( c == GUI_CLASS_METRIC || c == GUI_CLASS_STROKE
                 || c == GUI_CLASS_SKIN   || c == GUI_CLASS_PITCH
                 || c == GUI_CLASS_TYPE );

        gui()->separator_text( gui()->style_class_name( ( gui_style_class_t )c ) );
        for ( u32 v = 0; v < GUI_VAR_COUNT; ++v )
            if ( gui()->style_var_class( ( gui_style_var_t )v ) == ( gui_style_class_t )c )
            {
                f32 hi = gui()->style_var_max( ( gui_style_var_t )v );
                changed |= px ? se_px ( gui()->style_var_name( ( gui_style_var_t )v ),
                                        &work.var[ v ], 0, (i32)hi )
                              : se_f32( gui()->style_var_name( ( gui_style_var_t )v ),
                                        &work.var[ v ], 0.0f, hi );
            }
    }

    gui()->separator_text( gui()->style_class_name( GUI_CLASS_SHAPE ) );
    changed |= se_shape( GUI_VAR_CHECK_SHAPE,     work.var, se_nm_check,    3 );
    changed |= se_shape( GUI_VAR_BULLET_SHAPE,    work.var, se_nm_bullet,   2 );
    changed |= se_shape( GUI_VAR_ARROW_SHAPE,     work.var, se_nm_arrow,    2 );
    changed |= se_shape( GUI_VAR_SEPARATOR_SHAPE, work.var, se_nm_sep,      2 );
    changed |= se_shape( GUI_VAR_PROGRESS_SHAPE,  work.var, se_nm_progress, 2 );
    changed |= se_shape( GUI_VAR_KNOB_SHAPE,      work.var, se_nm_knob,     2 );
    changed |= se_shape( GUI_VAR_MENU_CHECK,      work.var, se_nm_menu,     2 );

    /* --- Density ramp: the scale_push steps, three metrics each ------------------------------
       Authored per theme and instanced with everything else, so a kit's DENSE is its own.  Laid
       out as one row per step so the three columns read as the (row, pad, gap) triple they are. */
    gui()->form( GUI_LABEL_RIGHT, 0.0f );
    gui()->separator_text( "Density ramp (scale_push steps)" );

    static const char* const nm_scale[ GUI_SCALE_COUNT ] = { "Dense", "Std", "Roomy", "Bar" };
    f32 step_w = gui()->text_size( "Roomy " ).x;

    for ( u32 s = 0; s < GUI_SCALE_COUNT; ++s )
    {
        gui()->push_id( nm_scale[ s ] );
        gui()->row_cols( 0.0f, (f32[]){ step_w, 1.0f, 1.0f, 1.0f, GUI_END } );
        gui()->text( nm_scale[ s ] );
        changed |= se_px_drag( "##row", &work.scales[ s ].row );
        changed |= se_px_drag( "##pad", &work.scales[ s ].pad );
        changed |= se_px_drag( "##gap", &work.scales[ s ].gap );
        gui()->pop_id();
    }
    gui()->stack();
    gui()->text_disabled( "row / pad / gap, authored at em=12" );

    /* --- Live sample of what the knobs above affect ----------------------------------------
       Deliberately small: the Look Gallery window is the wide sweep.  These few are here so a
       knob can be judged without leaving the panel. */
    gui()->separator_text( "Preview" );
    static bool  sample_check = true;
    static f32   sample_val   = 0.4f;
    static i32   sample_int   = 3;
    gui()->checkbox( "Checkbox", &sample_check );
    gui()->button( "Button" );

    gui()->form( GUI_LABEL_RIGHT, label_width );
    gui()->slider_float( "Slider", &sample_val, 0.0f, 1.0f );
    gui()->slider_int( "Steps", &sample_int, 0, 10 );
    gui()->progress_bar( sample_val, NULL );

    /* Commit once: writing through style_get marks the theme anonymous (an intentional edit),
       then style_apply rescales the active metrics from the new base. */
    if ( changed )
    {
        *gui()->style_get() = work;
        gui()->style_apply();
    }

    gui()->window_end();
}

// clang-format on
