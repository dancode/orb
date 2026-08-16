/*==============================================================================================

    sandbox/gui/sb_gui_style/st_export.c -- Style Export window: the live style as C source.

    The keep half of the bench.  The Style Editor finds a look by dragging knobs; this window
    turns the result into code you can paste, because a theme in this engine IS code -- k_themes[]
    in gui_theme.c is a table of complete gui_style_t literals, and a project kit is the same
    literal living in project source.  There is no style file format to write and no parser to
    feed; the compiler is the loader.

    Two emit modes, matching the engine's two authoring doors:

      TABLE   -- a `gui_theme_t` entry for k_themes[] (or a project's own theme table).  What you
                 want when the look is a named preset the engine should ship with.
      RUNTIME -- a setup function that pokes gui()->style_get() and calls style_bake/style_apply,
                 the door sb_gui's `modify_style` block demonstrates.  What you want when an app
                 restyles at boot without owning a theme entry.

    Both emit the AUTHORED half only -- seeds, ramp, vars, density ramp -- plus, separately, the
    colour cells that survive a re-bake.  That split is the point: gui_style_bake derives all 48
    cells from the eleven seeds and six ramp values, so dumping the grid verbatim would be 48
    numbers of noise hiding the twelve that matter.  We re-bake a copy of the live palette and
    diff: cells that come back identical are DERIVED and emitted as nothing at all; cells that
    differ are hand edits and are emitted after the bake call, which is exactly where they have
    to run to survive it (see theme_install in gui_theme.c).

    Enum identifier tables below are designated by the enum itself, so a reordered or extended
    enum cannot silently misname a slot -- a new member just emits as its raw index until it is
    named here.

==============================================================================================*/
// clang-format off

#define STX_CAP  ( 48 * 1024 )   /* worst case: 48 override cells + every var + a fat header */

typedef enum
{
    STX_MODE_TABLE = 0,   /* gui_theme_t entry for k_themes[]                */
    STX_MODE_RUNTIME,     /* style_get() + style_bake() + style_apply() call */

} stx_mode_t;

typedef struct
{
    char       name[ 64 ];      /* theme / function name, sanitized to a C identifier */
    stx_mode_t mode;
    bool       show_preview;
    bool       inited;

    char       text[ STX_CAP ]; /* the emitted source                                 */
    u32        len;
    u32        cells;           /* hand-edited (post-bake) colour cells in the emit    */

    char       status[ 512 ];
    bool       status_ok;

} stx_state_t;

static stx_state_t s_stx;

/*============================================================================================*/
/* Enum identifier tables -- designated by the enum, so a slot cannot slide.                    */
/*============================================================================================*/

static const char* const k_seed_id[ GUI_SEED_COUNT ] =
{
    [ GUI_SEED_SURFACE ] = "GUI_SEED_SURFACE",
    [ GUI_SEED_CONTROL ] = "GUI_SEED_CONTROL",
    [ GUI_SEED_INK     ] = "GUI_SEED_INK",
    [ GUI_SEED_LINE    ] = "GUI_SEED_LINE",
    [ GUI_SEED_ACCENT  ] = "GUI_SEED_ACCENT",
    [ GUI_SEED_MARK    ] = "GUI_SEED_MARK",
    [ GUI_SEED_GRAB    ] = "GUI_SEED_GRAB",
};

static const char* const k_ext_id[ GUI_EXT_RESERVED_COUNT ] =
{
    [ GUI_EXT_INFO  ] = "GUI_EXT_INFO",
    [ GUI_EXT_OK    ] = "GUI_EXT_OK",
    [ GUI_EXT_WARN  ] = "GUI_EXT_WARN",
    [ GUI_EXT_ERROR ] = "GUI_EXT_ERROR",
    [ GUI_EXT_DROP  ] = "GUI_EXT_DROP",
    [ GUI_EXT_SHADOW] = "GUI_EXT_SHADOW",
};

static const char* const k_ramp_id[ GUI_RAMP_COUNT ] =
{
    [ GUI_RAMP_HOVER  ] = "GUI_RAMP_HOVER",
    [ GUI_RAMP_PRESS  ] = "GUI_RAMP_PRESS",
    [ GUI_RAMP_FADE   ] = "GUI_RAMP_FADE",
    [ GUI_RAMP_RECESS ] = "GUI_RAMP_RECESS",
    [ GUI_RAMP_NEST   ] = "GUI_RAMP_NEST",
    [ GUI_RAMP_STEP   ] = "GUI_RAMP_STEP",
    [ GUI_RAMP_SELECT ] = "GUI_RAMP_SELECT",
};

static const char* const k_var_id[ GUI_VAR_COUNT ] =
{
    [ GUI_VAR_ROW             ] = "GUI_VAR_ROW",
    [ GUI_VAR_PAD             ] = "GUI_VAR_PAD",
    [ GUI_VAR_GAP             ] = "GUI_VAR_GAP",
    [ GUI_VAR_BORDER          ] = "GUI_VAR_BORDER",
    [ GUI_VAR_INDICATOR       ] = "GUI_VAR_INDICATOR",
    [ GUI_VAR_GUTTER          ] = "GUI_VAR_GUTTER",
    [ GUI_VAR_MIN_CELL        ] = "GUI_VAR_MIN_CELL",
    [ GUI_VAR_TITLE_H         ] = "GUI_VAR_TITLE_H",
    [ GUI_VAR_GRID_Q          ] = "GUI_VAR_GRID_Q",
    [ GUI_VAR_ROUND           ] = "GUI_VAR_ROUND",
    [ GUI_VAR_PANEL_ROUND     ] = "GUI_VAR_PANEL_ROUND",
    [ GUI_VAR_SHADOW          ] = "GUI_VAR_SHADOW",
    [ GUI_VAR_FOCUS_RING      ] = "GUI_VAR_FOCUS_RING",
    [ GUI_VAR_CHECK_SHAPE     ] = "GUI_VAR_CHECK_SHAPE",
    [ GUI_VAR_BULLET_SHAPE    ] = "GUI_VAR_BULLET_SHAPE",
    [ GUI_VAR_ARROW_SHAPE     ] = "GUI_VAR_ARROW_SHAPE",
    [ GUI_VAR_SEPARATOR_SHAPE ] = "GUI_VAR_SEPARATOR_SHAPE",
    [ GUI_VAR_PROGRESS_SHAPE  ] = "GUI_VAR_PROGRESS_SHAPE",
    [ GUI_VAR_KNOB_SHAPE      ] = "GUI_VAR_KNOB_SHAPE",
    [ GUI_VAR_MENU_CHECK      ] = "GUI_VAR_MENU_CHECK",
    [ GUI_VAR_DISABLED_ALPHA  ] = "GUI_VAR_DISABLED_ALPHA",
    [ GUI_VAR_ANIM_HOT        ] = "GUI_VAR_ANIM_HOT",
    [ GUI_VAR_ANIM_ACTIVE     ] = "GUI_VAR_ANIM_ACTIVE",
    [ GUI_VAR_ANIM_SELECT     ] = "GUI_VAR_ANIM_SELECT",
    [ GUI_VAR_ANIM_SIZE       ] = "GUI_VAR_ANIM_SIZE",
};

static const char* const k_role_id[ GUI_ROLE_COUNT ] =
{
    [ GUI_ROLE_PANEL       ] = "GUI_ROLE_PANEL",
    [ GUI_ROLE_PANEL_CHILD ] = "GUI_ROLE_PANEL_CHILD",
    [ GUI_ROLE_TITLE  ] = "GUI_ROLE_TITLE",
    [ GUI_ROLE_BG     ] = "GUI_ROLE_BG",
    [ GUI_ROLE_BORDER ] = "GUI_ROLE_BORDER",
    [ GUI_ROLE_TEXT_PRIMARY   ] = "GUI_ROLE_TEXT_PRIMARY",
    [ GUI_ROLE_TEXT_SECONDARY ] = "GUI_ROLE_TEXT_SECONDARY",
    [ GUI_ROLE_ACCENT ] = "GUI_ROLE_ACCENT",
    [ GUI_ROLE_MARK   ] = "GUI_ROLE_MARK",
    [ GUI_ROLE_GRAB   ] = "GUI_ROLE_GRAB",
};

static const char* const k_phase_id[ GUI_PHASE_COUNT ] =
{
    [ GUI_PHASE_IDLE   ] = "GUI_PHASE_IDLE",
    [ GUI_PHASE_HOT    ] = "GUI_PHASE_HOT",
    [ GUI_PHASE_ACTIVE ] = "GUI_PHASE_ACTIVE",
    [ GUI_PHASE_INERT  ] = "GUI_PHASE_INERT",
};

static const char* const k_scale_id[ GUI_SCALE_COUNT ] =
{
    [ GUI_SCALE_DENSE ] = "GUI_SCALE_DENSE",
    [ GUI_SCALE_STD   ] = "GUI_SCALE_STD",
    [ GUI_SCALE_ROOMY ] = "GUI_SCALE_ROOMY",
    [ GUI_SCALE_BAR   ] = "GUI_SCALE_BAR",
};

/* Shape picks emit their VALUE as an enum identifier too -- index i is the enum value, matching
   the display-name rows in st_editor.c. */
static const char* const k_check_val   [] = { "GUI_CHECK_TICK", "GUI_CHECK_DISC", "GUI_CHECK_CROSS" };
static const char* const k_bullet_val  [] = { "GUI_BULLET_DISC", "GUI_BULLET_SQUARE" };
static const char* const k_arrow_val   [] = { "GUI_ARROW_FILLED", "GUI_ARROW_CHEVRON" };
static const char* const k_sep_val     [] = { "GUI_SEPARATOR_SOLID", "GUI_SEPARATOR_DASHED" };
static const char* const k_progress_val[] = { "GUI_PROGRESS_SOLID", "GUI_PROGRESS_GRADIENT" };
static const char* const k_knob_val    [] = { "GUI_SLIDER_KNOB_BAR", "GUI_SLIDER_KNOB_CIRCLE" };
static const char* const k_menu_val    [] = { "GUI_MENU_CHECK_PLAIN", "GUI_MENU_CHECK_BOX" };

/* The value-name row for a SHAPE var, or NULL if the var is not a pick. */
static const char* const*
stx_shape_values( gui_style_var_t var, i32* count_out )
{
    switch ( var )
    {
        case GUI_VAR_CHECK_SHAPE:     *count_out = 3; return k_check_val;
        case GUI_VAR_BULLET_SHAPE:    *count_out = 2; return k_bullet_val;
        case GUI_VAR_ARROW_SHAPE:     *count_out = 2; return k_arrow_val;
        case GUI_VAR_SEPARATOR_SHAPE: *count_out = 2; return k_sep_val;
        case GUI_VAR_PROGRESS_SHAPE:  *count_out = 2; return k_progress_val;
        case GUI_VAR_KNOB_SHAPE:      *count_out = 2; return k_knob_val;
        case GUI_VAR_MENU_CHECK:      *count_out = 2; return k_menu_val;
        default:                      *count_out = 0; return NULL;
    }
}

/*============================================================================================*/
/* Emit primitives                                                                              */
/*============================================================================================*/

/* Append a formatted line to the emit buffer.  Silently stops at capacity -- the window reports
   the overflow rather than truncating mid-token into something that looks compilable. */
static void
stx_put( const char* fmt, ... )
{
    if ( s_stx.len >= STX_CAP - 1 )
        return;

    va_list args;
    va_start( args, fmt );
    int n = vsnprintf( s_stx.text + s_stx.len, STX_CAP - s_stx.len, fmt, args );
    va_end( args );

    if ( n > 0 )
        s_stx.len += ( (u32)n < STX_CAP - s_stx.len ) ? (u32)n : ( STX_CAP - s_stx.len - 1 );
}

/* The formatters below hand back a scratch string, and several of them routinely appear in ONE
   stx_put argument list ("{ %s, %s, %s }") -- which a single static buffer would collapse into
   three copies of the last value.  One small ring, shared by all three, sized well past the
   widest call site. */
#define STX_SCRATCH 8

static char s_scratch[ STX_SCRATCH ][ 64 ];
static u32  s_scratch_next;

static char*
stx_scratch( void )
{
    char* p = s_scratch[ s_scratch_next ];
    s_scratch_next = ( s_scratch_next + 1 ) % STX_SCRATCH;
    return p;
}

/* A packed cell as its GUI_COLOR( r, g, b, a ) constructor -- the form the theme table authors. */
static const char*
stx_color( u32 c )
{
    char* buf = stx_scratch();
    snprintf( buf, 64, "GUI_COLOR( 0x%02X, 0x%02X, 0x%02X, 0x%02X )",
              (unsigned)(   c         & 0xFF ),
              (unsigned)( ( c >> 8  ) & 0xFF ),
              (unsigned)( ( c >> 16 ) & 0xFF ),
              (unsigned)( ( c >> 24 ) & 0xFF ) );
    return buf;
}

/* A float as a C float literal: shortest form that round-trips the authored value, always with a
   decimal point so the suffix reads as a float and never as an int promoted by accident. */
static const char*
stx_f32( f32 v )
{
    char* buf = stx_scratch();
    snprintf( buf, 64, "%.4g", (double)v );

    if ( !strchr( buf, '.' ) && !strchr( buf, 'e' ) )
        snprintf( buf + strlen( buf ), 64 - strlen( buf ), ".0" );

    snprintf( buf + strlen( buf ), 64 - strlen( buf ), "f" );
    return buf;
}

/* An identifier from a designated table, falling back to the raw index for a slot this file has
   not been taught yet (a new enum member) -- the emit stays valid, it just names a number. */
static const char*
stx_id( const char* const* table, u32 count, u32 i )
{
    if ( i < count && table[ i ] )
        return table[ i ];

    char* buf = stx_scratch();
    snprintf( buf, 64, "%u", i );
    return buf;
}

/* Sanitize the user's name into a C identifier: alnum and underscore survive, everything else
   becomes an underscore, and a leading digit gets a prefix. */
static void
stx_sanitize( const char* in, char* out, u32 out_size )
{
    u32 o = 0;
    if ( in[ 0 ] >= '0' && in[ 0 ] <= '9' && out_size > 1 )
        out[ o++ ] = 't';

    for ( u32 i = 0; in[ i ] && o < out_size - 1; ++i )
    {
        char c = in[ i ];
        bool ok = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) ||
                  ( c >= '0' && c <= '9' ) || c == '_';
        out[ o++ ] = ok ? c : '_';
    }
    if ( o == 0 && out_size > 1 )
        out[ o++ ] = '_';
    out[ o ] = '\0';
}

/*============================================================================================*/
/* Emit sections                                                                                */
/*============================================================================================*/

/* Seeds + ramp + the extended palette's reserved four -- the whole authored surface. */
static void
stx_emit_palette( const gui_style_t* s, const char* ind )
{
    stx_put( "%s.palette =\n%s{\n%s    .seed =\n%s    {\n", ind, ind, ind, ind );
    for ( u32 i = 0; i < GUI_SEED_COUNT; ++i )
        stx_put( "%s        [ %-18s ] = %s,\n", ind,
                 stx_id( k_seed_id, GUI_SEED_COUNT, i ), stx_color( s->palette.seed[ i ] ) );
    stx_put( "%s    },\n%s    .ramp =\n%s    {\n", ind, ind, ind );
    for ( u32 i = 0; i < GUI_RAMP_COUNT; ++i )
        stx_put( "%s        [ %-16s ] = %s,\n", ind,
                 stx_id( k_ramp_id, GUI_RAMP_COUNT, i ), stx_f32( s->palette.ramp[ i ] ) );
    stx_put( "%s    },\n%s    .ext =\n%s    {\n", ind, ind, ind );
    for ( u32 i = 0; i < GUI_EXT_RESERVED_COUNT; ++i )
        stx_put( "%s        [ %-14s ] = %s,\n", ind,
                 stx_id( k_ext_id, GUI_EXT_RESERVED_COUNT, i ), stx_color( s->palette.ext[ i ] ) );
    stx_put( "%s    },\n%s},\n", ind, ind );
}

/* Every var, grouped by the class the engine reports, shapes emitted as their enum value. */
static void
stx_emit_vars( const gui_style_t* s, const char* ind )
{
    stx_put( "%s.var =\n%s{\n", ind, ind );

    for ( u32 c = 0; c < GUI_CLASS_COUNT; ++c )
    {
        bool head = false;
        for ( u32 v = 0; v < GUI_VAR_COUNT; ++v )
        {
            if ( (u32)gui()->style_var_class( ( gui_style_var_t )v ) != c )
                continue;

            if ( !head )
            {
                stx_put( "%s    /* %s */\n", ind,
                         gui()->style_class_name( ( gui_style_class_t )c ) );
                head = true;
            }

            i32                n_vals = 0;
            const char* const* vals   = stx_shape_values( ( gui_style_var_t )v, &n_vals );
            i32                pick   = (i32)s->var[ v ];
            const char*        value  = ( vals && pick >= 0 && pick < n_vals )
                                            ? vals[ pick ]
                                            : stx_f32( s->var[ v ] );

            /* Terminator folded into the value token so the trailing comment stays aligned
               past it -- a comma parked after the padding would be a comment, not syntax. */
            char cell[ 64 ];
            snprintf( cell, sizeof cell, "%s,", value );

            stx_put( "%s    [ %-24s ] = %-25s   /* %s */\n", ind,
                     stx_id( k_var_id, GUI_VAR_COUNT, v ), cell,
                     gui()->style_var_name( ( gui_style_var_t )v ) );
        }
    }
    stx_put( "%s},\n", ind );
}

/* The density ramp -- (row, pad, gap) per scale_push step. */
static void
stx_emit_scales( const gui_style_t* s, const char* ind )
{
    stx_put( "%s.scales =\n%s{\n", ind, ind );
    for ( u32 i = 0; i < GUI_SCALE_COUNT; ++i )
        stx_put( "%s    [ %-16s ] = { %s, %s, %s },\n", ind,
                 stx_id( k_scale_id, GUI_SCALE_COUNT, i ),
                 stx_f32( s->scales[ i ].row ),
                 stx_f32( s->scales[ i ].pad ),
                 stx_f32( s->scales[ i ].gap ) );
    stx_put( "%s},\n", ind );
}

/* The cells that do NOT come back from a re-bake of this palette -- the hand edits, and the only
   part of the 48-cell grid worth writing down.  `target` names the thing being assigned into
   ("s->col" for the runtime door, "style->col" inside a theme installer). */
static u32
stx_emit_overrides( const gui_style_t* s, const char* target, const char* ind )
{
    /* Re-derive a reference grid from this very palette; anything that matches is derived. */
    gui_style_t ref = *s;
    gui()->style_bake( &ref );

    u32 n = 0;
    for ( u32 r = 0; r < GUI_ROLE_COUNT; ++r )
        for ( u32 p = 0; p < GUI_PHASE_COUNT; ++p )
        {
            if ( s->col[ r ][ p ] == ref.col[ r ][ p ] )
                continue;

            stx_put( "%s%s[ %s ][ %s ] = %s;\n", ind, target,
                     stx_id( k_role_id,  GUI_ROLE_COUNT,  r ),
                     stx_id( k_phase_id, GUI_PHASE_COUNT, p ),
                     stx_color( s->col[ r ][ p ] ) );
            ++n;
        }
    return n;
}

/* Any face slot in use means the look leans on brushes this emitter cannot serialize (a brush is
   registered at runtime against a texture); say so rather than emit a silently incomplete theme. */
static u32
stx_count_faces( const gui_style_t* s )
{
    u32 n = 0;
    for ( u32 r = 0; r < GUI_ROLE_COUNT; ++r )
        for ( u32 p = 0; p < GUI_PHASE_COUNT; ++p )
            if ( s->face[ r ][ p ] != 0 )
                ++n;
    return n;
}

/*============================================================================================*/
/* Emit                                                                                         */
/*============================================================================================*/

static void
stx_build( void )
{
    const gui_style_t* s = gui()->style_peek();

    char id[ 64 ];
    stx_sanitize( s_stx.name[ 0 ] ? s_stx.name : "custom", id, sizeof id );

    s_stx.len       = 0;
    s_stx.text[ 0 ] = '\0';
    s_stx.cells     = 0;

    const char* base = gui()->theme_get();

    stx_put( "/*  Generated by sb_gui_style -- Style Export.\n"
             "    Base: %s.  Font (em) is NOT part of a theme: metrics below are authored at em=12\n"
             "    and gui_style_apply rescales them to whatever face is active.  */\n\n",
             base ? base : "edited from a built-in theme" );

    if ( s_stx.mode == STX_MODE_TABLE )
    {
        stx_put( "/*  A k_themes[] entry (gui_theme.c), or a project's own theme table.  The colour\n"
                 "    grid is deliberately absent: theme_install() bakes it from the palette below.  */\n\n" );

        stx_put( "static const gui_theme_t k_theme_%s =\n{\n    \"%s\",\n    {\n", id, id );
        stx_emit_palette( s, "        " );
        stx_emit_vars   ( s, "        " );
        stx_emit_scales ( s, "        " );
        stx_put( "    },\n};\n" );

        /* Overrides go in a companion installer -- a table entry cannot carry post-bake code. */
        u32 n = 0;
        {
            u32 mark = s_stx.len;
            stx_put( "\n/*  Hand-edited cells: they disagree with the bake, so they must be applied AFTER it\n"
                     "    (see theme_install in gui_theme.c -- it bakes, then any override belongs here).  */\n\n"
                     "static void\ntheme_%s_overrides( gui_style_t* style )\n{\n", id );
            n = stx_emit_overrides( s, "style->col", "    " );
            if ( n == 0 )
            {
                s_stx.len          = mark;        /* nothing to override -- drop the whole block */
                s_stx.text[ mark ] = '\0';
            }
            else
            {
                stx_put( "}\n" );
            }
        }
        s_stx.cells = n;
    }
    else
    {
        stx_put( "/*  Runtime door: restyle at boot without owning a theme entry.  Call once after\n"
                 "    gui init (and after any theme_set), then style_apply to rescale the metrics.  */\n\n" );

        stx_put( "static void\nstyle_%s_apply( void )\n{\n"
                 "    gui_style_t* s = gui()->style_get();   /* the base style -- theme goes anonymous */\n\n", id );

        stx_put( "    /* Seeds + ramp: the authored colour. */\n" );
        for ( u32 i = 0; i < GUI_SEED_COUNT; ++i )
            stx_put( "    s->palette.seed[ %-18s ] = %s;\n",
                     stx_id( k_seed_id, GUI_SEED_COUNT, i ), stx_color( s->palette.seed[ i ] ) );
        stx_put( "\n" );
        for ( u32 i = 0; i < GUI_RAMP_COUNT; ++i )
            stx_put( "    s->palette.ramp[ %-16s ] = %s;\n",
                     stx_id( k_ramp_id, GUI_RAMP_COUNT, i ), stx_f32( s->palette.ramp[ i ] ) );

        stx_put( "\n    /* Derive all 48 cells from the palette above. */\n"
                 "    gui()->style_bake( s );\n" );

        u32 mark = s_stx.len;
        stx_put( "\n    /* ...then disagree with the ramp on individual cells.  Order matters: a cell\n"
                 "       written before the bake would simply be overwritten by it. */\n" );
        u32 n = stx_emit_overrides( s, "    s->col", "" );
        if ( n == 0 )
        {
            s_stx.len          = mark;
            s_stx.text[ mark ] = '\0';
        }
        s_stx.cells = n;

        stx_put( "\n    /* Metrics + skin scalars, authored at em=12. */\n" );
        for ( u32 v = 0; v < GUI_VAR_COUNT; ++v )
        {
            i32                n_vals = 0;
            const char* const* vals   = stx_shape_values( ( gui_style_var_t )v, &n_vals );
            i32                pick   = (i32)s->var[ v ];
            const char*        value  = ( vals && pick >= 0 && pick < n_vals )
                                            ? vals[ pick ]
                                            : stx_f32( s->var[ v ] );

            char cell[ 64 ];
            snprintf( cell, sizeof cell, "%s;", value );

            stx_put( "    s->var[ %-24s ] = %-25s   /* %s */\n",
                     stx_id( k_var_id, GUI_VAR_COUNT, v ), cell,
                     gui()->style_var_name( ( gui_style_var_t )v ) );
        }

        stx_put( "\n    /* Density ramp -- what scale_push pushes onto ROW / PAD / GAP. */\n" );
        for ( u32 i = 0; i < GUI_SCALE_COUNT; ++i )
            stx_put( "    s->scales[ %-16s ] = ( gui_scale_metrics_t ){ %s, %s, %s };\n",
                     stx_id( k_scale_id, GUI_SCALE_COUNT, i ),
                     stx_f32( s->scales[ i ].row ),
                     stx_f32( s->scales[ i ].pad ),
                     stx_f32( s->scales[ i ].gap ) );

        stx_put( "\n    gui()->style_apply();   /* rescale the active metrics from the new base */\n}\n" );
    }

    /* The one thing the emit cannot carry. */
    u32 faces = stx_count_faces( s );
    if ( faces )
        stx_put( "\n/*  NOTE -- %u face slot%s in use.  A face names a BRUSH registered at runtime\n"
                 "    (gui_style_brush_add) against a texture, so it cannot be emitted as data here;\n"
                 "    register the brushes in code and assign the handles after this style lands.  */\n",
                 faces, faces == 1 ? " is" : "s are" );

    if ( s_stx.len >= STX_CAP - 1 )
    {
        snprintf( s_stx.status, sizeof s_stx.status,
                  "Emit truncated at %u bytes -- raise STX_CAP.", (unsigned)STX_CAP );
        s_stx.status_ok = false;
    }
    else
    {
        snprintf( s_stx.status, sizeof s_stx.status,
                  "Emitted %u bytes, %u hand-edited cell%s.",
                  s_stx.len, s_stx.cells, s_stx.cells == 1 ? "" : "s" );
        s_stx.status_ok = true;
    }
}

/* Write the emit to temp/sb_gui_style/<name>.c under the engine root (scratch, not an asset:
   the destination for a kept theme is project source, which is the user's call to make). */
static void
stx_write_file( void )
{
    if ( !s_stx.len )
        stx_build();

    char id[ 64 ];
    stx_sanitize( s_stx.name[ 0 ] ? s_stx.name : "custom", id, sizeof id );

    char dir[ 512 ];
    snprintf( dir, sizeof dir, "%s" PATH_SEP "temp" PATH_SEP "sb_gui_style", sys_root_dir() );
    if ( !sys_dir_make( dir ) )
    {
        snprintf( s_stx.status, sizeof s_stx.status, "Could not create %s", dir );
        s_stx.status_ok = false;
        return;
    }

    char path[ 640 ];
    snprintf( path, sizeof path, "%s" PATH_SEP "%s_theme.c", dir, id );

    if ( !sys_file_write_entire( path, s_stx.text, s_stx.len ) )
    {
        snprintf( s_stx.status, sizeof s_stx.status, "Write failed: %s", path );
        s_stx.status_ok = false;
        return;
    }

    snprintf( s_stx.status, sizeof s_stx.status, "Wrote %u bytes -> %s", s_stx.len, path );
    s_stx.status_ok = true;
}

/*============================================================================================*/
/* Headless emit -- the `-emit` command line path (see main).                                   */
/*                                                                                              */
/* Writes BOTH modes for the live style and returns how many files landed.  The point is that    */
/* the emitter can be exercised without a window and without a click: the files it writes are C, */
/* so compiling them is a real assertion about the output, not a screenshot of it.               */
/*============================================================================================*/

static int
st_export_emit_files( void )
{
    const char* theme = gui()->theme_get();
    int         n     = 0;

    for ( int m = 0; m < 2; ++m )
    {
        s_stx.mode = ( m == 0 ) ? STX_MODE_TABLE : STX_MODE_RUNTIME;
        snprintf( s_stx.name, sizeof s_stx.name, "%s_%s",
                  theme ? theme : "custom", ( m == 0 ) ? "table" : "runtime" );

        stx_build();
        stx_write_file();

        printf( "[sb_gui_style] %s\n", s_stx.status );
        n += s_stx.status_ok ? 1 : 0;
    }
    return n;
}

/*============================================================================================*/
/* Window                                                                                       */
/*============================================================================================*/

static void
st_export_window( void )
{
    if ( !s_stx.inited )
    {
        snprintf( s_stx.name, sizeof s_stx.name, "%s", "custom" );
        s_stx.show_preview = true;
        s_stx.inited       = true;
        stx_build();
    }

    if ( !st_begin( "Style Export", 640.0f, 620.0f ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    /* --- What is being exported ------------------------------------------------------------- */
    const char* base = gui()->theme_get();
    gui()->textf( "Live style: %s", base ? base : "(custom -- edited)" );
    gui()->same_line( -1 );
    gui()->help_marker( "Style Export always emits the LIVE base style (style_peek), whatever the\n"
                        "Style Editor last committed -- a named theme untouched, or your edits on top." );

    gui()->separator_text( "Emit" );

    gui()->form( GUI_LABEL_RIGHT, gui()->text_size( "Theme name" ).x );
    if ( gui()->input_text( "Theme name", s_stx.name, sizeof s_stx.name ) )
        stx_build();
    gui()->form( GUI_LABEL_RIGHT, 0.0f );

    /* Two doors, two shapes of output -- see the file header. */
    i32 mode = (i32)s_stx.mode;
    gui()->row2( 0.5f, 0.5f );
    bool picked  = gui()->radio_button( "gui_theme_t table entry",   &mode, STX_MODE_TABLE );
         picked |= gui()->radio_button( "runtime style_get() setup", &mode, STX_MODE_RUNTIME );
    if ( picked )
    {
        s_stx.mode = ( stx_mode_t )mode;
        stx_build();
    }

    gui()->stack();

    /* --- Actions ---------------------------------------------------------------------------- */
    gui()->row_cols( 0.0f, (f32[]){ 1.0f, 1.0f, 1.0f, GUI_END } );

    gui()->next_item_fit( 1.0f );
    if ( gui()->button( "Rebuild" ) )
        stx_build();

    gui()->next_item_fit( 1.0f );
    if ( gui()->button( "Copy to Clipboard" ) )
    {
        stx_build();
        app()->clipboard_set( s_stx.text );
        snprintf( s_stx.status, sizeof s_stx.status, "Copied %u bytes to the clipboard.", s_stx.len );
        s_stx.status_ok = true;
    }

    gui()->next_item_fit( 1.0f );
    if ( gui()->button( "Write File" ) )
    {
        stx_build();
        stx_write_file();
    }

    gui()->stack();

    if ( s_stx.status[ 0 ] )
    {
        if ( s_stx.status_ok ) gui()->text_wrapped( s_stx.status );
        else                   gui()->text_colored( GUI_COLOR( 0xFF, 0x60, 0x60, 0xFF ), s_stx.status );
    }

    /* --- Preview ----------------------------------------------------------------------------
       Read-only on purpose: the buffer is regenerated from the live style on every action, so an
       edit made here would be thrown away by the next knob turn.  The clipboard is the door out. */
    gui()->checkbox( "Show source", &s_stx.show_preview );

    if ( s_stx.show_preview )
    {
        gui()->separator_text( "Source" );

        if ( gui()->child_begin( "##src", 0.0f, gui()->content_avail().y, GUI_WIN_NONE ) )
        {
            gui()->stack();
            gui()->scale_push( GUI_SCALE_DENSE );

            /* Walk the buffer line by line -- one text() per line, so the child scrolls and clips
               like any other content and no copy of the whole emit is made. */
            const char* line = s_stx.text;
            while ( *line )
            {
                const char* nl = strchr( line, '\n' );
                u32         n  = nl ? (u32)( nl - line ) : (u32)strlen( line );

                char buf[ 256 ];
                if ( n >= sizeof buf ) n = sizeof buf - 1;
                memcpy( buf, line, n );
                buf[ n ] = '\0';

                if ( buf[ 0 ] )
                    gui()->text( buf );
                else
                    gui()->new_line( -1.0f );

                if ( !nl ) break;
                line = nl + 1;
            }

            gui()->scale_pop();
        }
        gui()->child_end();
    }

    gui()->window_end();
}

// clang-format on
