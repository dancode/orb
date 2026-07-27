/*==============================================================================================

    runtime_service/gui/style/gui_theme.c -- Theme registry + base style state + layout metrics.

    Owns the three pieces of style STATE that everything else in gui reads or scales from:
        k_themes     -- the built-in named presets (gui_theme_t), each a complete gui_style_t
                        authored for em=12.
        s_style_base -- the mutable user base style: a copy of the active theme, or freely edited
                        via gui_style_get() (theme_name then goes anonymous / NULL).
        s_style      -- s_style_base scaled to the active font's type size (em) by metrics_compute;
                        every other file's WIDGET_ / WIN_ metrics and default colors ultimately
                        read this, through gui_style_core.c's push-stack resolver and the
                        vocabulary macros over it (style/gui_style.h).

    The theme API (theme_list/set/get/reset) and gui_style_get() are the public surface over
    that state; metrics_compute is the font-driven rescale, invoked across the unit seam by
    gui_style_apply (frame/gui_frame_font.c) whenever a font loads or activates -- the rescale
    needs font metrics this unit must not read itself.  style_new_frame (gui_style_core.c)
    reseeds the push-stacks' base layer from s_style each frame; gui_theme_reset() calls it
    through the style/gui_style.h declaration.

    Included by gui_style.c FIRST -- s_style must already be defined before
    gui_style_core.c's style_var_base resolver reads it in this TU.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Layout

    All dimensions are integer pixel counts derived from the active font's *type size* (em) --
    not its glyph-box height (char_h = ascent + descent), which runs ~1.3x the em and would
    inflate every padding.  The em is the design unit a typographer reasons in, so spacing,
    padding, and control heights all scale off it and stay proportional across fonts.
    Defaults match a 12px em.

    grid_quantum then snaps the scaled row-level metrics onto one px lattice (default 4) so
    row pitch, insets, and title bars share a common divisor and nested regions seam-align;
    set it to 1 (or 0) in a theme for free-pixel metrics, larger for a blockier feel.
==============================================================================================*/

/* Font type size (em) used by metrics_compute; updated by font_load(). */
u32 s_font_size = 0;

/*==============================================================================================
    The var schema -- ONE table describing every scalar the style has.

    Display name + class, designated by index so an entry cannot slide out of alignment.  This
    is the whole description of a var: metrics_compute reads the class to decide scaling and
    snapping, gui_style_var_name / _class publish both, and a style editor groups its sliders
    from them instead of keeping a parallel list.  Adding a var is one line HERE plus one in the
    enum -- there is no third place that has to be remembered.
==============================================================================================*/

typedef struct { const char* name; u8 cls; } style_var_info_t;

static const style_var_info_t k_var[ GUI_VAR_COUNT ] =
{
    [ GUI_VAR_ROW             ] = { "Row Height",      GUI_CLASS_METRIC },
    [ GUI_VAR_PAD             ] = { "Padding",         GUI_CLASS_METRIC },
    [ GUI_VAR_GAP             ] = { "Gap",             GUI_CLASS_METRIC },
    [ GUI_VAR_INDICATOR       ] = { "Indicator Size",  GUI_CLASS_METRIC },
    [ GUI_VAR_GUTTER          ] = { "Knob / Gutter",   GUI_CLASS_METRIC },
    [ GUI_VAR_MIN_CELL        ] = { "Min Cell Width",  GUI_CLASS_METRIC },
    [ GUI_VAR_TITLE_H         ] = { "Title Height",    GUI_CLASS_METRIC },

    [ GUI_VAR_BORDER          ] = { "Border Width",    GUI_CLASS_STROKE },

    [ GUI_VAR_ROUND           ] = { "Widget Rounding", GUI_CLASS_SKIN   },
    [ GUI_VAR_PANEL_ROUND     ] = { "Panel Rounding",  GUI_CLASS_SKIN   },

    [ GUI_VAR_GRID_Q          ] = { "Grid Quantum",    GUI_CLASS_PITCH  },

    [ GUI_VAR_CHECK_SHAPE     ] = { "Check Shape",     GUI_CLASS_SHAPE  },
    [ GUI_VAR_BULLET_SHAPE    ] = { "Bullet Shape",    GUI_CLASS_SHAPE  },
    [ GUI_VAR_ARROW_SHAPE     ] = { "Arrow Shape",     GUI_CLASS_SHAPE  },
    [ GUI_VAR_SEPARATOR_SHAPE ] = { "Separator Shape", GUI_CLASS_SHAPE  },
    [ GUI_VAR_PROGRESS_SHAPE  ] = { "Progress Shape",  GUI_CLASS_SHAPE  },
    [ GUI_VAR_KNOB_SHAPE      ] = { "Knob Shape",      GUI_CLASS_SHAPE  },
    [ GUI_VAR_MENU_CHECK      ] = { "Menu Check",      GUI_CLASS_SHAPE  },
};

/* Section labels for the classes above -- an editor's group headings. */
static const char* const k_class_name[ GUI_CLASS_COUNT ] =
{
    [ GUI_CLASS_METRIC ] = "Metrics",
    [ GUI_CLASS_STROKE ] = "Strokes",
    [ GUI_CLASS_SKIN   ] = "Skin",
    [ GUI_CLASS_PITCH  ] = "Lattice",
    [ GUI_CLASS_SHAPE  ] = "Shapes",
};

/* The three px classes: everything the em rescale multiplies.  A PITCH is a raw lattice count
   and a SHAPE is an enum -- scaling either would be meaningless, not merely wrong. */
static bool
var_is_pixels( u8 cls )
{
    return cls == GUI_CLASS_METRIC || cls == GUI_CLASS_STROKE || cls == GUI_CLASS_SKIN;
}

/* Shared authoring blocks -- the built-in themes repeat large identical spans (the 6x4 palette,
   the density ramp, the shape picks), so those live once here as designated-initializer fragments
   and each theme below reads as "this palette + these few deltas".  A theme is still a plain
   gui_style_t aggregate; these macros only save the copy-paste (and the silent-typo risk).

   A palette is authored as the color grid itself -- role by role, four phases across -- which
   is the same shape a style source writes and the same shape the block stores.  There is no
   projection step between the three any more. */

/* The dark family palette -- shared by "dark", "rounded", and "quantum" (they diverge only in
   metrics / skin, never in color). */
#define THEME_PALETTE_DARK \
    .col = { \
    /*                    IDLE                              HOT                               ACTIVE                            DIM                            */ \
    [ GUI_ROLE_PANEL  ] = { GUI_COLOR( 0x24,0x24,0x24,0xFF ), GUI_COLOR( 0x2E,0x2E,0x2E,0xFF ), GUI_COLOR( 0x10,0x60,0xA0,0xFF ), GUI_COLOR( 0x1C,0x1C,0x1C,0xFF ) }, \
    [ GUI_ROLE_TITLE  ] = { GUI_COLOR( 0x2A,0x30,0x38,0xFF ), GUI_COLOR( 0x50,0x80,0xB0,0xFF ), GUI_COLOR( 0x24,0x24,0x24,0xFF ), GUI_COLOR( 0x26,0x29,0x2C,0xFF ) }, \
    [ GUI_ROLE_BG     ] = { GUI_COLOR( 0x40,0x40,0x40,0xFF ), GUI_COLOR( 0x50,0x80,0xB0,0xFF ), GUI_COLOR( 0x30,0x60,0x90,0xFF ), GUI_COLOR( 0x30,0x30,0x30,0xFF ) }, \
    [ GUI_ROLE_BORDER ] = { GUI_COLOR( 0x80,0x80,0x80,0xFF ), GUI_COLOR( 0x40,0xA0,0xF0,0xFF ), GUI_COLOR( 0x40,0xA0,0xF0,0xFF ), GUI_COLOR( 0x50,0x50,0x50,0xFF ) }, \
    [ GUI_ROLE_TEXT   ] = { GUI_COLOR( 0xF0,0xF0,0xF0,0xFF ), GUI_COLOR( 0xF0,0xF0,0xF0,0xFF ), GUI_COLOR( 0xF0,0xF0,0xF0,0xFF ), GUI_COLOR( 0xA0,0xA0,0xA0,0xFF ) }, \
    [ GUI_ROLE_ACCENT ] = { GUI_COLOR( 0x20,0x90,0xD0,0xFF ), GUI_COLOR( 0x40,0xA0,0xF0,0xFF ), GUI_COLOR( 0x18,0xE6,0x48,0xFF ), GUI_COLOR( 0x30,0x30,0x30,0xFF ) }, \
    [ GUI_ROLE_GRAB   ] = { GUI_COLOR( 0xC8,0xCD,0xD4,0xFF ), GUI_COLOR( 0xE4,0xEA,0xF0,0xFF ), GUI_COLOR( 0xFF,0xFF,0xFF,0xFF ), GUI_COLOR( 0x60,0x64,0x68,0xFF ) }, \
    }

/* The light palette -- a soft neutral-grey desktop look (never a white glare): the window sits on
   a calm mid-grey, panels recess a shade under it, controls raise a shade over it, and the accent
   is a muted steel blue rather than a saturated primary so nothing vibrates against the grey. */
#define THEME_PALETTE_LIGHT \
    .col = { \
    /*                    IDLE                              HOT                               ACTIVE                            DIM                            */ \
    [ GUI_ROLE_PANEL  ] = { GUI_COLOR( 0xE2,0xE2,0xE6,0xFF ), GUI_COLOR( 0xED,0xED,0xF1,0xFF ), GUI_COLOR( 0x50,0x6C,0x94,0xFF ), GUI_COLOR( 0xD6,0xD7,0xDC,0xFF ) }, \
    [ GUI_ROLE_TITLE  ] = { GUI_COLOR( 0xAE,0xB4,0xC0,0xFF ), GUI_COLOR( 0x86,0xA6,0xD2,0xFF ), GUI_COLOR( 0xE2,0xE2,0xE6,0xFF ), GUI_COLOR( 0xCD,0xD0,0xD7,0xFF ) }, \
    [ GUI_ROLE_BG     ] = { GUI_COLOR( 0xEC,0xEC,0xF0,0xFF ), GUI_COLOR( 0x86,0xA6,0xD2,0xFF ), GUI_COLOR( 0x5C,0x82,0xB4,0xFF ), GUI_COLOR( 0xDC,0xDC,0xE2,0xFF ) }, \
    [ GUI_ROLE_BORDER ] = { GUI_COLOR( 0xB4,0xB5,0xBC,0xFF ), GUI_COLOR( 0x44,0x6C,0xA6,0xFF ), GUI_COLOR( 0x44,0x6C,0xA6,0xFF ), GUI_COLOR( 0xC8,0xC9,0xCF,0xFF ) }, \
    [ GUI_ROLE_TEXT   ] = { GUI_COLOR( 0x20,0x22,0x26,0xFF ), GUI_COLOR( 0x20,0x22,0x26,0xFF ), GUI_COLOR( 0x20,0x22,0x26,0xFF ), GUI_COLOR( 0x6C,0x6E,0x74,0xFF ) }, \
    [ GUI_ROLE_ACCENT ] = { GUI_COLOR( 0x44,0x6C,0xA6,0xFF ), GUI_COLOR( 0x44,0x6C,0xA6,0xFF ), GUI_COLOR( 0x2E,0x9E,0x54,0xFF ), GUI_COLOR( 0xC7,0xC8,0xCE,0xFF ) }, \
    [ GUI_ROLE_GRAB   ] = { GUI_COLOR( 0x3A,0x40,0x4A,0xFF ), GUI_COLOR( 0x1E,0x22,0x2A,0xFF ), GUI_COLOR( 0x0A,0x0C,0x10,0xFF ), GUI_COLOR( 0xA8,0xAA,0xB2,0xFF ) }, \
    }

/* The density ramp is identical across every built-in theme (STD mirrors the base metrics). */
#define THEME_SCALES_DEFAULT \
    .scales = { \
        [ GUI_SCALE_DENSE ] = { .row = 16, .pad = 4,  .gap = 4 }, \
        [ GUI_SCALE_STD   ] = { .row = 20, .pad = 8,  .gap = 4 },   /* == the base metrics */ \
        [ GUI_SCALE_ROOMY ] = { .row = 24, .pad = 8,  .gap = 4 }, \
        [ GUI_SCALE_BAR   ] = { .row = 32, .pad = 12, .gap = 4 }, \
    }

/* The var block, parameterized by the five numbers the built-ins actually differ on.  Spelled as
   one macro with arguments rather than a shared default plus per-theme overrides, because a
   second designated initializer for the same array element is exactly the kind of thing that
   compiles differently depending on how strictly a compiler reads C11 6.7.9 -- an argument list
   is unambiguous everywhere.  Everything not an argument is the same in every built-in theme;
   promote one to an argument the day a theme needs to differ on it. */
#define THEME_VARS( GAP, ROUND, PANEL_ROUND, KNOB, GRID_Q ) \
    .var = { \
        /* 1. METRICS -- px at em=12 */ \
        [ GUI_VAR_ROW             ] = 20, \
        [ GUI_VAR_PAD             ] = 8, \
        [ GUI_VAR_GAP             ] = ( GAP ), \
        [ GUI_VAR_BORDER          ] = 1, \
        [ GUI_VAR_INDICATOR       ] = 16, \
        [ GUI_VAR_GUTTER          ] = 12, \
        [ GUI_VAR_MIN_CELL        ] = 40, \
        [ GUI_VAR_TITLE_H         ] = 24, \
        /* 2. SKIN */ \
        [ GUI_VAR_ROUND           ] = ( ROUND ), \
        [ GUI_VAR_PANEL_ROUND     ] = ( PANEL_ROUND ), \
        [ GUI_VAR_GRID_Q          ] = ( GRID_Q ), \
        /* 3. SHAPE PICKS */ \
        [ GUI_VAR_CHECK_SHAPE     ] = GUI_CHECK_TICK, \
        [ GUI_VAR_BULLET_SHAPE    ] = GUI_BULLET_DISC, \
        [ GUI_VAR_ARROW_SHAPE     ] = GUI_ARROW_FILLED, \
        [ GUI_VAR_SEPARATOR_SHAPE ] = GUI_SEPARATOR_SOLID, \
        [ GUI_VAR_PROGRESS_SHAPE  ] = GUI_PROGRESS_SOLID, \
        [ GUI_VAR_KNOB_SHAPE      ] = ( KNOB ), \
        [ GUI_VAR_MENU_CHECK      ] = GUI_MENU_CHECK_BOX, \
    }

/* Built-in theme registry.  Each entry is a complete gui_style_t authored for em=12;
   metrics_compute scales the metrics to the active font.  Add more here; the array is const
   so its name pointers remain stable for the lifetime of the process. */
static const gui_theme_t k_themes[] =
{
    {
        /* Hard square corners, bar knob, free-pixel lattice. */
        "dark",
        {
            THEME_PALETTE_DARK,
            THEME_VARS( /*gap*/ 8, /*round*/ 0, /*panel_round*/ 0, GUI_SLIDER_KNOB_BAR, /*grid_q*/ 1 ),
            THEME_SCALES_DEFAULT,
        },
    },
    {
        /* "dark" with the soft-corner treatment: the same palette, rounding dialed on and a
           circular slider knob.  Not the default -- switch with gui()->theme_set( "rounded" )
           to compare the two looks. */
        "rounded",
        {
            THEME_PALETTE_DARK,
            THEME_VARS( /*gap*/ 4, /*round*/ 4, /*panel_round*/ 8, GUI_SLIDER_KNOB_CIRCLE, /*grid_q*/ 1 ),
            THEME_SCALES_DEFAULT,
        },
    },
    {
        "light",
        {
            THEME_PALETTE_LIGHT,
            THEME_VARS( /*gap*/ 4, /*round*/ 0, /*panel_round*/ 0, GUI_SLIDER_KNOB_BAR, /*grid_q*/ 1 ),
            THEME_SCALES_DEFAULT,
        },
    },
    {
        /* "dark" on a coarse 16px layout lattice -- identical palette and skin, only GUI_VAR_GRID_Q
           differs, so nested regions seam-align on a chunky grid. */
        "quantum",
        {
            THEME_PALETTE_DARK,
            THEME_VARS( /*gap*/ 8, /*round*/ 0, /*panel_round*/ 0, GUI_SLIDER_KNOB_BAR, /*grid_q*/ 16 ),
            THEME_SCALES_DEFAULT,
        },
    },
};

static const u32 k_theme_count = sizeof( k_themes ) / sizeof( k_themes[ 0 ] );

/* The mutable user base style -- edited directly via gui_style_get(), or overwritten by
   gui_theme_set().  Initialized to the first built-in ("dark") so the engine is styled from
   the first frame without an explicit theme_set call. */
static gui_style_t s_style_base;

/* Active theme name -- pointer into k_themes[i].name, NULL if the user has made anonymous
   edits via gui_style_get() without subsequently calling gui_theme_set(). */
static const char* s_theme_name = NULL;

/* The active style: s_style_base scaled to the current font size.  Private -- style_active() is
   the read door, metrics_compute the only writer.  A poke here would not survive the next
   rescale, which rebuilds the whole struct from s_style_base. */
static gui_style_t s_style;

gui_style_t*
gui_style_get( void )
{
    /* Direct edits via this pointer are anonymous; the caller is responsible for calling
       gui_style_apply() and is advised to call gui_theme_reset() to clear push stacks. */
    s_theme_name = NULL;
    return &s_style_base;
}

/* The ACTIVE style -- s_style_base rescaled for the current font by gui_style_apply.  The
   internal read every style landing re-derives from (see style/gui_style.h). */
const gui_style_t*
style_active( void )
{
    return &s_style;
}

const gui_style_t*
gui_style_peek( void )
{
    /* Read-only view of the base style -- unlike gui_style_get() this does NOT mark the theme
       anonymous, so a live style editor can display current values (and label the active theme)
       and only reach for gui_style_get() on the frame an edit actually lands. */
    return &s_style_base;
}

/*==============================================================================================
    Theme API -- named style snapshots that form the root of the push/pop stack.

    gui_theme_reset() is the "large style change" escape hatch: it restores s_style_base from
    the active named theme (if any), rescales the metrics, and immediately clears both the color
    and var push stacks -- so callers never need to issue paired pop calls just to get back to a
    clean base state.
==============================================================================================*/

const gui_theme_t*
gui_theme_list( u32* count_out )
{
    if ( count_out ) *count_out = k_theme_count;
    return k_themes;
}

bool
gui_theme_set( const char* name )
{
    if ( !name ) return false;
    for ( u32 i = 0; i < k_theme_count; ++i )
    {
        if ( strcmp( name, k_themes[ i ].name ) == 0 )
        {
            s_style_base = k_themes[ i ].style;
            s_theme_name = k_themes[ i ].name;
            gui_theme_reset();
            return true;
        }
    }
    return false;
}

const char*
gui_theme_get( void )
{
    return s_theme_name;
}

void
gui_theme_reset( void )
{
    /* Restore s_style_base from the active named theme so anonymous style_get edits are
       discarded.  If no theme is set (anonymous), s_style_base is left as-is and only
       the push stacks are cleared. */
    if ( s_theme_name )
    {
        for ( u32 i = 0; i < k_theme_count; ++i )
        {
            if ( strcmp( s_theme_name, k_themes[ i ].name ) == 0 )
            {
                s_style_base = k_themes[ i ].style;
                break;
            }
        }
    }
    /* gui_style_apply() no-ops safely if no font has activated yet (font_valid() gate lives there
       now) -- s_style just stays at its pre-font zero value, which style_new_frame seeds the push
       stacks from harmlessly (nothing renders pre-font; gui_ctx_begin asserts font_valid()).
       Whichever call activates the first font (gui_init's built-in preset, or the caller's own
       font_load) triggers gui_style_apply() again and scales s_style_base for real at that point. */

    gui_style_apply();  /* rescale s_style from s_style_base */
    style_new_frame();  /* reseed the working slot set from s_style, clear all push stacks */
}

/*==============================================================================================
    Grid lattice -- the ONE home of the quantum-snapping arithmetic.  Every place that snaps a size
    or a cumulative edge onto the theme's grid_quantum px lattice (theme metric quantize below,
    layout track resolve, natural widths, pack pens, scroll spill tolerance) routes through these
    four primitives; no other code divides or multiplies by grid_quantum to snap.  Each takes the
    lattice pitch `q` explicitly so it is phase-agnostic -- theme compute passes the base quantum,
    the layout engine passes the live one.  q <= 1 is "grid off" and returns the value verbatim.

    The GUI_GRID_LATTICE compile switch (gui.h) collapses all four to identity when 0, so the dead
    arithmetic folds out and grid_quantum has no runtime cost.
==============================================================================================*/

/* Largest lattice multiple <= v (0 allowed -- callers needing a nonzero floor use lat_floor_min). */

f32
lat_floor( f32 v, u32 q )
{
#if GUI_GRID_LATTICE
    if ( q <= 1 || v <= 0.0f ) return v;
    return (f32)( (u32)( v / (f32)q ) * q );
#else
    (void)q;
    return v;
#endif
}

/* Largest lattice multiple <= v, but never below one quantum -- for a live size that must not
   collapse to nothing when it dips under a single cell. */

f32
lat_floor_min( f32 v, u32 q )
{
#if GUI_GRID_LATTICE
    if ( q <= 1 || v <= 0.0f ) return v;
    f32 r = (f32)( (u32)( v / (f32)q ) * q );
    return ( r < (f32)q ) ? (f32)q : r;
#else
    (void)q;
    return v;
#endif
}

/* Smallest lattice multiple >= v. */

f32
lat_ceil( f32 v, u32 q )
{
#if GUI_GRID_LATTICE
    if ( q <= 1 || v <= 0.0f ) return v;
    f32 m = v / (f32)q;
    u32 n = (u32)m;
    if ( (f32)n < m ) ++n;
    return (f32)( n * q );
#else
    (void)q;
    return v;
#endif
}

/* Nearest lattice multiple (round half away from zero).  Signed-safe across the whole real line --
   unlike lat_floor / lat_ceil / lat_floor_min (which snap non-negative sizes and cumulative edges),
   this also snaps window POSITIONS, which go negative when a window slides or resizes past the top /
   left of its viewport.  A v <= 0 guard here would silently stop snapping outside those edges. */

f32
lat_round( f32 v, u32 q )
{
#if GUI_GRID_LATTICE
    if ( q <= 1 ) return v;
    f32 n = v / (f32)q;
    i32 k = (i32)( n >= 0.0f ? n + 0.5f : n - 0.5f );   /* round half away from zero, sign-correct */
    return (f32)( k * (i32)q );
#else
    (void)q;
    return v;
#endif
}

/* Snap a scaled metric onto the grid lattice: nearest multiple of q, floored at one quantum so
   a nonzero authored metric never vanishes.  Zero stays zero (an authored "none" is preserved).
   Only reached from the GUI_GRID_LATTICE-gated block in metrics_compute, so it lives under the gate
   rather than lingering as an unused static when snapping is compiled out. */

#if GUI_GRID_LATTICE
static f32
metric_quantize( f32 v, u32 q )
{
    if ( v <= 0.0f ) return 0.0f;
    f32 r = lat_round( v, q );
    return ( r < (f32)q ) ? (f32)q : r;
}
#endif

/* Recompute the active layout metrics by scaling the user's base style profile to the
   active font's type size (em).  The base style is authored assuming em=12.  Invoked across
   the unit seam by gui_style_apply (frame/gui_frame_font.c), which reads the font metrics this
   unit must not touch and passes them in as parameters. */

void
metrics_compute( u32 em, u32 char_h, u32 line_h )
{
    if ( em < 8u ) em = 8u;
    s_font_size = em;

    /* Original numbers based on font size of em=12.
       Scale the base metrics proportionally to the active font's type size. */

    f32 scale = (f32)em / 12.0f;

    /* Colors carry over verbatim; so do the lattice pitch and the shape picks, which the loop
       below skips by class. */
    s_style = s_style_base;

    /* Scale every var that is a pixel count.  Driven by the class table, not by a line per field
       and not by an enum range: a new metric declares its class beside its name and is scaled
       from that moment, so the failure mode where a field was added but nobody remembered to
       rescale it cannot happen. */
    for ( u32 i = 0; i < GUI_VAR_COUNT; ++i )
        if ( var_is_pixels( k_var[ i ].cls ) )
            s_style.var[ i ] = s_style_base.var[ i ] * scale;

    /* The ramp steps are metrics like any other: same factor. */
    for ( u32 i = 0; i < GUI_SCALE_COUNT; ++i )
    {
        s_style.scales[ i ].row = s_style_base.scales[ i ].row * scale;
        s_style.scales[ i ].pad = s_style_base.scales[ i ].pad * scale;
        s_style.scales[ i ].gap = s_style_base.scales[ i ].gap * scale;
    }

    /* Prevent vanishing hairlines when scaling down: an authored nonzero never rounds to nothing.
       (The caret and the focus ring used to need their own clauses here; both now read
       GUI_VAR_BORDER, so one clause covers them.) */
    bool clamp_min_visible_metrics = true;
    if ( clamp_min_visible_metrics )
    {
        if ( s_style.var[ GUI_VAR_BORDER ] < 1.0f && s_style_base.var[ GUI_VAR_BORDER ] > 0.0f )
            s_style.var[ GUI_VAR_BORDER ] = 1.0f;
        if ( s_style.var[ GUI_VAR_GAP ] < 1.0f && s_style_base.var[ GUI_VAR_GAP ] > 0.0f )
            s_style.var[ GUI_VAR_GAP ] = 1.0f;
    }

    /* Floor the row height to the font's glyph box and line advance so a tall-boxed font
       (e.g. one with deep descenders) never clips and a single line of text always fits. */
    bool clamp_line_size_to_font = true;
    if ( clamp_line_size_to_font )
    {
        f32 floor_h = ( (f32)char_h > (f32)line_h ) ? (f32)char_h : (f32)line_h;

        if ( s_style.var[ GUI_VAR_ROW ] < floor_h ) s_style.var[ GUI_VAR_ROW ] = floor_h;

        /* Every ramp row owes the same guarantee: a row always holds one line of text, so a
           font too tall for DENSE simply lifts it (the ramp compresses rather than clips). */
        for ( u32 i = 0; i < GUI_SCALE_COUNT; ++i )
            if ( s_style.scales[ i ].row < floor_h ) s_style.scales[ i ].row = floor_h;
    }

    /* Grid theme: snap the row-level layout metrics back onto the quantum lattice -- the em
       scale and the font floor above land on arbitrary pixels, and the whole point of the grid
       is that row pitch (row + gap), insets, and title bars share one divisor so nested
       regions stay seam-aligned.  Row height rounds UP so the font floor is never undone (text
       must still fit); everything else rounds to nearest.  Strokes and fine details (win_border,
       rounding) stay free -- a hairline or a radius snapped to the lattice would quadruple, so
       they keep their scaled pixel value even when they shape geometry. */
#if GUI_GRID_LATTICE
    u32 q = (u32)s_style_base.var[ GUI_VAR_GRID_Q ];
    if ( q > 1 )
    {
        /* GUI_CLASS_METRIC snaps and nothing else does -- which is exactly why STROKE and SKIN
           are their own classes rather than metrics: a hairline or a corner radius snapped to a
           16px lattice would quadruple.  ROW rounds UP so the font floor above is never undone;
           every other metric goes to nearest. */
        s_style.var[ GUI_VAR_ROW ] = lat_ceil( s_style.var[ GUI_VAR_ROW ], q );

        for ( u32 i = 0; i < GUI_VAR_COUNT; ++i )
            if ( k_var[ i ].cls == GUI_CLASS_METRIC && i != GUI_VAR_ROW )
                s_style.var[ i ] = metric_quantize( s_style.var[ i ], q );

        /* Ramp steps land on the same lattice: rows ceil (keep the font floor), pads and gaps
           snap to nearest.  The whole ramp retunes together when the quantum or font changes. */
        for ( u32 i = 0; i < GUI_SCALE_COUNT; ++i )
        {
            s_style.scales[ i ].row = lat_ceil( s_style.scales[ i ].row, q );
            s_style.scales[ i ].pad = metric_quantize( s_style.scales[ i ].pad, q );
            s_style.scales[ i ].gap = metric_quantize( s_style.scales[ i ].gap, q );
        }
    }
#endif
}

// clang-format on
/*============================================================================================*/
