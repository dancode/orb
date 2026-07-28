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

    [ GUI_VAR_DISABLED_ALPHA  ] = { "Disabled Alpha",  GUI_CLASS_RATIO  },

    [ GUI_VAR_ANIM_HOT        ] = { "Hover Rate",      GUI_CLASS_RATE   },
    [ GUI_VAR_ANIM_ACTIVE     ] = { "Press Rate",      GUI_CLASS_RATE   },
    [ GUI_VAR_ANIM_SELECT     ] = { "Select Rate",     GUI_CLASS_RATE   },
    [ GUI_VAR_ANIM_SIZE       ] = { "Size Rate",       GUI_CLASS_RATE   },

    [ GUI_VAR_CHECK_SHAPE     ] = { "Check Shape",     GUI_CLASS_SHAPE  },
    [ GUI_VAR_BULLET_SHAPE    ] = { "Bullet Shape",    GUI_CLASS_SHAPE  },
    [ GUI_VAR_ARROW_SHAPE     ] = { "Arrow Shape",     GUI_CLASS_SHAPE  },
    [ GUI_VAR_SEPARATOR_SHAPE ] = { "Separator Shape", GUI_CLASS_SHAPE  },
    [ GUI_VAR_PROGRESS_SHAPE  ] = { "Progress Shape",  GUI_CLASS_SHAPE  },
    [ GUI_VAR_KNOB_SHAPE      ] = { "Knob Shape",      GUI_CLASS_SHAPE  },
    [ GUI_VAR_MENU_CHECK      ] = { "Menu Check",      GUI_CLASS_SHAPE  },
};

/* The palette axes, named for the same reason the var axis is: a style editor walks the schema
   instead of keeping a table in step with enums it does not own.  Designated by index, so an
   entry cannot slide out of alignment; an unnamed addition reads "?" rather than misreporting a
   neighbour. */
static const char* const k_seed_name[ GUI_SEED_COUNT ] =
{
    [ GUI_SEED_SURFACE ] = "Surface",
    [ GUI_SEED_CONTROL ] = "Control",
    [ GUI_SEED_INK     ] = "Ink",
    [ GUI_SEED_LINE    ] = "Line",
    [ GUI_SEED_ACCENT  ] = "Accent",
    [ GUI_SEED_MARK    ] = "Mark",
    [ GUI_SEED_GRAB    ] = "Grab",
    [ GUI_SEED_INFO    ] = "Info",
    [ GUI_SEED_OK      ] = "OK",
    [ GUI_SEED_WARN    ] = "Warn",
    [ GUI_SEED_ERROR   ] = "Error",
};

static const char* const k_ramp_name[ GUI_RAMP_COUNT ] =
{
    [ GUI_RAMP_HOVER  ] = "Hover Wash",
    [ GUI_RAMP_PRESS  ] = "Press Wash",
    [ GUI_RAMP_FADE   ] = "Inert Fade",
    [ GUI_RAMP_RECESS ] = "Recess Sink",
    [ GUI_RAMP_STEP   ] = "Lift Step",
    [ GUI_RAMP_SELECT ] = "Select Wash",
};

/* Section labels for the classes above -- an editor's group headings. */
static const char* const k_class_name[ GUI_CLASS_COUNT ] =
{
    [ GUI_CLASS_METRIC ] = "Metrics",
    [ GUI_CLASS_STROKE ] = "Strokes",
    [ GUI_CLASS_SKIN   ] = "Skin",
    [ GUI_CLASS_PITCH  ] = "Lattice",
    [ GUI_CLASS_RATIO  ] = "Ratios",
    [ GUI_CLASS_RATE   ] = "Motion",
    [ GUI_CLASS_SHAPE  ] = "Shapes",
};

/* The three px classes: everything the em rescale multiplies.  A PITCH is a raw lattice count,
   a RATIO is a unitless fraction and a SHAPE is an enum -- scaling any of them would be
   meaningless, not merely wrong. */
static bool
var_is_pixels( u8 cls )
{
    return cls == GUI_CLASS_METRIC || cls == GUI_CLASS_STROKE || cls == GUI_CLASS_SKIN;
}

/*==============================================================================================
    The palettes -- seven seeds and a five-number ramp per family, and .col left EMPTY.

    Every built-in used to carry 32 colour literals, and the two families restated the same
    quarter of them: TEXT one colour in three phases, BORDER hot == active, MARK idle == active,
    BG dim == ACCENT dim, TITLE active == PANEL idle.  Those are derivations, and they now live
    once, in style/gui_bake.c, instead of once per theme in hex.  gui_theme_set bakes the grid on
    the way in (theme_bake below), so nothing here authors a cell.

    The ramps differ between the families, and that is the point rather than an oversight: a
    fixed fraction toward black is a gentle inset on a near-black surface and a bruise on a
    near-white one, so "light" recesses at 0.08 where "dark" recesses at 0.22.  Everything else
    they agree on, which is what says the two looks really are one system.
==============================================================================================*/

/* The dark family -- shared by "dark", "rounded", and "quantum" (they diverge only in metrics /
   skin, never in colour).  A near-black desktop, a steel-blue accent, a green affirmative, and a
   near-white anchor for the knobs. */
#define THEME_PALETTE_DARK \
    .palette = { \
        .seed = { \
            [ GUI_SEED_SURFACE ] = GUI_COLOR( 0x24,0x24,0x24,0xFF ), \
            [ GUI_SEED_CONTROL ] = GUI_COLOR( 0x40,0x40,0x40,0xFF ), \
            [ GUI_SEED_INK     ] = GUI_COLOR( 0xF0,0xF0,0xF0,0xFF ), \
            [ GUI_SEED_LINE    ] = GUI_COLOR( 0x80,0x80,0x80,0xFF ), \
            [ GUI_SEED_ACCENT  ] = GUI_COLOR( 0x20,0x90,0xD0,0xFF ), \
            [ GUI_SEED_MARK    ] = GUI_COLOR( 0x18,0xE6,0x48,0xFF ), \
            [ GUI_SEED_GRAB    ] = GUI_COLOR( 0xC8,0xCD,0xD4,0xFF ), \
            [ GUI_SEED_INFO    ] = GUI_COLOR( 0x58,0xA8,0xE8,0xFF ), \
            [ GUI_SEED_OK      ] = GUI_COLOR( 0x5C,0xC8,0x64,0xFF ), \
            [ GUI_SEED_WARN    ] = GUI_COLOR( 0xE8,0xB8,0x40,0xFF ), \
            [ GUI_SEED_ERROR   ] = GUI_COLOR( 0xE8,0x5C,0x4C,0xFF ), \
        }, \
        .ramp = { \
            [ GUI_RAMP_HOVER  ] = 0.60f, \
            [ GUI_RAMP_PRESS  ] = 0.75f, \
            [ GUI_RAMP_FADE   ] = 0.45f, \
            [ GUI_RAMP_RECESS ] = 0.22f, \
            [ GUI_RAMP_STEP   ] = 0.18f, \
            [ GUI_RAMP_SELECT ] = 0.55f, \
        }, \
    }

/* The light family -- a soft neutral-grey desktop look (never a white glare): the window sits on
   a calm mid-grey, controls raise a shade over it, and the accent is a muted steel blue rather
   than a saturated primary so nothing vibrates against the grey.  The anchor inverts, as the
   anchor always does -- near-black knobs on a light theme.

   Its ramp is the gentler of the two, and every difference is the same fact seen from a
   different angle: moves are LOUDER near white.  A 0.22 sink that reads as a subtle inset well
   at 0x24 reads as a dirty smear at 0xE2, so recess drops to 0.08 -- and since a wash toward
   this accent already darkens (the accent is darker than the control here, where on the dark
   theme it is brighter), the hover and the lift step both come down too or a hovered button
   lands halfway to navy. */
#define THEME_PALETTE_LIGHT \
    .palette = { \
        .seed = { \
            [ GUI_SEED_SURFACE ] = GUI_COLOR( 0xE2,0xE2,0xE6,0xFF ), \
            [ GUI_SEED_CONTROL ] = GUI_COLOR( 0xEC,0xEC,0xF0,0xFF ), \
            [ GUI_SEED_INK     ] = GUI_COLOR( 0x20,0x22,0x26,0xFF ), \
            [ GUI_SEED_LINE    ] = GUI_COLOR( 0xB4,0xB5,0xBC,0xFF ), \
            [ GUI_SEED_ACCENT  ] = GUI_COLOR( 0x44,0x6C,0xA6,0xFF ), \
            [ GUI_SEED_MARK    ] = GUI_COLOR( 0x2E,0x9E,0x54,0xFF ), \
            [ GUI_SEED_GRAB    ] = GUI_COLOR( 0x3A,0x40,0x4A,0xFF ), \
            [ GUI_SEED_INFO    ] = GUI_COLOR( 0x2C,0x6C,0xB0,0xFF ), \
            [ GUI_SEED_OK      ] = GUI_COLOR( 0x2A,0x84,0x40,0xFF ), \
            [ GUI_SEED_WARN    ] = GUI_COLOR( 0xA8,0x70,0x10,0xFF ), \
            [ GUI_SEED_ERROR   ] = GUI_COLOR( 0xC0,0x38,0x2C,0xFF ), \
        }, \
        .ramp = { \
            [ GUI_RAMP_HOVER  ] = 0.50f, \
            [ GUI_RAMP_PRESS  ] = 0.72f, \
            [ GUI_RAMP_FADE   ] = 0.40f, \
            [ GUI_RAMP_RECESS ] = 0.08f, \
            [ GUI_RAMP_STEP   ] = 0.12f, \
            [ GUI_RAMP_SELECT ] = 0.50f, \
        }, \
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
        /* 3. RATIOS -- unitless */ \
        [ GUI_VAR_DISABLED_ALPHA  ] = 0.5f, \
        /* 4. RATES -- Hz-like damper speeds; 0 snaps.  Press is quicker than hover on purpose: \
              a hover is an invitation and may drift, a press is an answer and must land. \
              SIZE is quicker still: it is not expressing anything, it is covering the frame of \
              lag a single-pass engine owes on any MEASURED extent.  A snap there reads as a \
              glitch; a fast settle reads as the layout deciding. */ \
        [ GUI_VAR_ANIM_HOT        ] = 10.0f, \
        [ GUI_VAR_ANIM_ACTIVE     ] = 20.0f, \
        [ GUI_VAR_ANIM_SELECT     ] = 12.0f, \
        [ GUI_VAR_ANIM_SIZE       ] = 25.0f, \
        /* 5. SHAPE PICKS */ \
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

/* Load a built-in into the base style: copy the authored half, then DERIVE the colour grid from
   its palette.  The one place a theme's cells come into being -- k_themes authors no colour, so
   without this every cell would be zero.  Any hand-authored override a theme ever wants belongs
   after the bake call here, never in the table. */
static void
theme_install( const gui_theme_t* t )
{
    s_style_base = t->style;
    gui_style_bake( &s_style_base );
}

bool
gui_theme_set( const char* name )
{
    if ( !name ) return false;
    for ( u32 i = 0; i < k_theme_count; ++i )
    {
        if ( strcmp( name, k_themes[ i ].name ) == 0 )
        {
            theme_install( &k_themes[ i ] );
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
    /* Restore s_style_base from the active named theme (palette copied, grid re-derived) so
       anonymous style_get edits are discarded.  If no theme is set (anonymous), s_style_base is
       left ALONE -- including its colour grid: an anonymous style may hold cells the caller
       hand-authored after their own bake, and re-baking here would silently eat them.  Baking
       is the caller's step for exactly that reason (see gui_style_bake). */
    if ( s_theme_name )
    {
        for ( u32 i = 0; i < k_theme_count; ++i )
        {
            if ( strcmp( s_theme_name, k_themes[ i ].name ) == 0 )
            {
                theme_install( &k_themes[ i ] );
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

    /* The palette and the baked grid carry over verbatim -- colour has no px in it, so an em
       rescale must not touch either.  So do the lattice pitch and the shape picks, which the
       loop below skips by class. */
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
