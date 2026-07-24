/*==============================================================================================

    runtime_service/gui/style/gui_theme.c -- Theme registry + base style state + layout metrics.

    Owns the three pieces of style STATE that everything else in gui reads or scales from:
        k_themes     -- the built-in named presets (gui_theme_t), each a complete gui_style_t
                        authored for em=12.
        s_style_base -- the mutable user base style: a copy of the active theme, or freely edited
                        via gui_style_get() (theme_name then goes anonymous / NULL).
        s_style      -- s_style_base scaled to the active font's type size (em) by layout_compute;
                        every other file's WIDGET_ / WIN_ metrics and default colors ultimately
                        read this (through gui_style.c's push-stack resolver + vocabulary macros,
                        gui_symbol.c's check/bullet/arrow style setters, ...).

    The theme API (theme_list/set/get/reset) and gui_style_get() are the public surface over
    that state; layout_compute is the font-driven rescale, invoked across the unit seam by
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

/* Font type size (em) used by layout_compute; updated by font_load(). */
u32 s_font_size = 0;

/* Shared authoring blocks -- the built-in themes repeat large identical spans (a 17-slot palette,
   the density ramp), so those live once here as designated-initializer fragments and each theme
   below reads as "this palette + these few deltas".  A theme is still a plain gui_style_t aggregate;
   these macros only save the copy-paste (and the silent-typo risk that comes with it). */

/* The dark family palette -- shared by "dark", "rounded", and "quantum" (they diverge only in
   metrics / skin, never in color). */
#define THEME_PALETTE_DARK \
    [ GUI_COL_TEXT         ] = GUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF ), \
    [ GUI_COL_TEXT_DIM     ] = GUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF ), \
    [ GUI_COL_WINDOW_BG    ] = GUI_COLOR( 0x24, 0x24, 0x24, 0xFF ), \
    [ GUI_COL_CHILD_BG     ] = GUI_COLOR( 0x1C, 0x1C, 0x1C, 0xFF ), \
    [ GUI_COL_TITLE_BG     ] = GUI_COLOR( 0x10, 0x60, 0xA0, 0xFF ), \
    [ GUI_COL_BORDER       ] = GUI_COLOR( 0x80, 0x80, 0x80, 0xFF ), \
    [ GUI_COL_WIDGET_BG    ] = GUI_COLOR( 0x40, 0x40, 0x40, 0xFF ), \
    [ GUI_COL_WIDGET_HOT   ] = GUI_COLOR( 0x50, 0x80, 0xB0, 0xFF ), \
    [ GUI_COL_WIDGET_ACT   ] = GUI_COLOR( 0x30, 0x60, 0x90, 0xFF ), \
    [ GUI_COL_WIDGET_FG    ] = GUI_COLOR( 0x20, 0x90, 0xD0, 0xFF ), \
    [ GUI_COL_CHECK_MARK   ] = GUI_COLOR( 0x18, 0xE6, 0x48, 0xFF ), \
    [ GUI_COL_SLIDER_TRACK ] = GUI_COLOR( 0x30, 0x30, 0x30, 0xFF ), \
    [ GUI_COL_RESIZE_HOT   ] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ), \
    [ GUI_COL_INPUT_BG     ] = GUI_COLOR( 0x38, 0x38, 0x38, 0xFF ), \
    [ GUI_COL_INPUT_FOCUS  ] = GUI_COLOR( 0x20, 0x50, 0x70, 0xFF ), \
    [ GUI_COL_CURSOR       ] = GUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF ), \
    [ GUI_COL_NAV_HIGHLIGHT] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ), \
    [ GUI_COL_NAV_CAPTURE  ] = GUI_COLOR( 0xF0, 0xA0, 0x20, 0xFF ), \
    [ GUI_COL_FOCUS_BORDER ] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF )

/* The light palette -- a soft neutral-grey desktop look (never a white glare): the window sits on
   a calm mid-grey, panels recess a shade under it, controls raise a shade over it, and the accent
   is a muted steel blue rather than a saturated primary so nothing vibrates against the grey. */
#define THEME_PALETTE_LIGHT \
    [ GUI_COL_TEXT         ] = GUI_COLOR( 0x20, 0x22, 0x26, 0xFF ), \
    [ GUI_COL_TEXT_DIM     ] = GUI_COLOR( 0x6C, 0x6E, 0x74, 0xFF ), \
    [ GUI_COL_WINDOW_BG    ] = GUI_COLOR( 0xE2, 0xE2, 0xE6, 0xFF ), \
    [ GUI_COL_CHILD_BG     ] = GUI_COLOR( 0xD6, 0xD7, 0xDC, 0xFF ), \
    [ GUI_COL_TITLE_BG     ] = GUI_COLOR( 0x50, 0x6C, 0x94, 0xFF ), \
    [ GUI_COL_BORDER       ] = GUI_COLOR( 0xB4, 0xB5, 0xBC, 0xFF ), \
    [ GUI_COL_WIDGET_BG    ] = GUI_COLOR( 0xEC, 0xEC, 0xF0, 0xFF ), \
    [ GUI_COL_WIDGET_HOT   ] = GUI_COLOR( 0x86, 0xA6, 0xD2, 0xFF ), \
    [ GUI_COL_WIDGET_ACT   ] = GUI_COLOR( 0x5C, 0x82, 0xB4, 0xFF ), \
    [ GUI_COL_WIDGET_FG    ] = GUI_COLOR( 0x44, 0x6C, 0xA6, 0xFF ), \
    [ GUI_COL_CHECK_MARK   ] = GUI_COLOR( 0x2E, 0x9E, 0x54, 0xFF ), \
    [ GUI_COL_SLIDER_TRACK ] = GUI_COLOR( 0xC7, 0xC8, 0xCE, 0xFF ), \
    [ GUI_COL_RESIZE_HOT   ] = GUI_COLOR( 0x44, 0x6C, 0xA6, 0xFF ), \
    [ GUI_COL_INPUT_BG     ] = GUI_COLOR( 0xF3, 0xF3, 0xF6, 0xFF ), \
    [ GUI_COL_INPUT_FOCUS  ] = GUI_COLOR( 0xCF, 0xDE, 0xF1, 0xFF ), \
    [ GUI_COL_CURSOR       ] = GUI_COLOR( 0x22, 0x28, 0x40, 0xFF ), \
    [ GUI_COL_NAV_HIGHLIGHT] = GUI_COLOR( 0x44, 0x6C, 0xA6, 0xFF ), \
    [ GUI_COL_NAV_CAPTURE  ] = GUI_COLOR( 0xC8, 0x8A, 0x20, 0xFF ), \
    [ GUI_COL_FOCUS_BORDER ] = GUI_COLOR( 0x44, 0x6C, 0xA6, 0xFF )

/* The density ramp is identical across every built-in theme (STD mirrors the base metrics). */
#define THEME_SCALES_DEFAULT \
    .scales = { \
        [ GUI_SCALE_DENSE ] = { .row = 16, .pad = 4,  .gap = 4 }, \
        [ GUI_SCALE_STD   ] = { .row = 20, .pad = 8,  .gap = 4 },   /* == the base metrics */ \
        [ GUI_SCALE_ROOMY ] = { .row = 24, .pad = 8,  .gap = 4 }, \
        [ GUI_SCALE_BAR   ] = { .row = 32, .pad = 12, .gap = 4 }, \
    }

/* The SKIN enum defaults shared by every theme -- the mark / arrow / knob shapes.  Rounding and
   the slider knob shape are authored per theme (they carry each theme's visual identity), so they
   stay inline; only the "never varies" enums live here. */
#define THEME_SKIN_SHAPES_DEFAULT \
    .check_style     = GUI_CHECK_TICK, \
    .bullet_style    = GUI_BULLET_DISC, \
    .arrow_style     = GUI_ARROW_FILLED, \
    .separator_style = GUI_SEPARATOR_SOLID, \
    .progress_style  = GUI_PROGRESS_SOLID, \
    .menu_check      = GUI_MENU_CHECK_BOX, \
    .checkmark_pad   = 4, \
    .cursor_w        = 1, \
    .cursor_inset    = 3, \
    .win_focus_border = 2

/* Built-in theme registry.  Each entry is a complete gui_style_t authored for em=12;
   layout_compute scales the metrics to the active font.  Add more here; the array is const
   so its name pointers remain stable for the lifetime of the process. */
static const gui_theme_t k_themes[] =
{
    {
        "dark",
        {
            .colors = { THEME_PALETTE_DARK },
            /* 1. METRICS */
            .line_size = 20, .widget_gap = 8, .widget_pad = 8, .min_cell_w = 40,
            .grid_quantum = 1, .win_border = 1, .win_title_h = 24,
            .checkbox_sz = 16, .slider_knob_w = 12,
            /* 2. SKIN -- hard square corners, bar knob */
            .win_rounding = 0, .widget_rounding = 0, .grab_rounding = 0,
            .slider_knob = GUI_SLIDER_KNOB_BAR,
            THEME_SKIN_SHAPES_DEFAULT,
            THEME_SCALES_DEFAULT,
        },
    },
    {
        /* "dark" with the soft-corner treatment: the same palette, rounding dialed on and a
           circular slider knob.  Not the default -- switch with gui()->theme_set( "rounded" )
           to compare the two looks. */
        "rounded",
        {
            .colors = { THEME_PALETTE_DARK },
            /* 1. METRICS */
            .line_size = 20, .widget_gap = 4, .widget_pad = 8, .min_cell_w = 40,
            .grid_quantum = 1, .win_border = 1, .win_title_h = 24,
            .checkbox_sz = 16, .slider_knob_w = 12,
            /* 2. SKIN -- rounded corners, circular knob */
            .win_rounding = 8, .widget_rounding = 4, .grab_rounding = 6,
            .slider_knob = GUI_SLIDER_KNOB_CIRCLE,
            THEME_SKIN_SHAPES_DEFAULT,
            THEME_SCALES_DEFAULT,
        },
    },
    {
        "light",
        {
            .colors = { THEME_PALETTE_LIGHT },
            /* 1. METRICS */
            .line_size = 20, .widget_gap = 4, .widget_pad = 8, .min_cell_w = 40,
            .grid_quantum = 1, .win_border = 1, .win_title_h = 24,
            .checkbox_sz = 16, .slider_knob_w = 12,
            /* 2. SKIN -- lightly rounded, bar knob */
            .win_rounding = 0, .widget_rounding = 0, .grab_rounding = 0,
            .slider_knob = GUI_SLIDER_KNOB_BAR,
            THEME_SKIN_SHAPES_DEFAULT,
            THEME_SCALES_DEFAULT,
        },
    },
    {
        /* "dark" on a coarse 16px layout lattice -- identical palette and skin, only grid_quantum
           differs, so nested regions seam-align on a chunky grid. */
        "quantum",
        {
            .colors = { THEME_PALETTE_DARK },
            /* 1. METRICS */
            .line_size = 20, .widget_gap = 8, .widget_pad = 8, .min_cell_w = 40,
            .grid_quantum = 16, .win_border = 1, .win_title_h = 24,
            .checkbox_sz = 16, .slider_knob_w = 12,
            /* 2. SKIN -- hard square corners, bar knob (as "dark") */
            .win_rounding = 0, .widget_rounding = 0, .grab_rounding = 0,
            .slider_knob = GUI_SLIDER_KNOB_BAR,
            THEME_SKIN_SHAPES_DEFAULT,
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

/* The active style, scaled from s_style_base for the current font size. */
gui_style_t s_style;

gui_style_t*
gui_style_get( void )
{
    /* Direct edits via this pointer are anonymous; the caller is responsible for calling
       gui_style_apply() and is advised to call gui_theme_reset() to clear push stacks. */
    s_theme_name = NULL;
    return &s_style_base;
}

/* The ACTIVE style -- s_style_base rescaled for the current font by gui_style_apply.  The
   internal read the element unit's el_style_derive compiles from (see style/gui_style.h). */
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
   Only reached from the GUI_GRID_LATTICE-gated block in layout_compute, so it lives under the gate
   rather than lingering as an unused static when snapping is compiled out. */

#if GUI_GRID_LATTICE
static u8
metric_quantize( u32 v, u32 q )
{
    if ( v == 0 ) return 0;
    u32 r = (u32)lat_round( (f32)v, q );
    return (u8)( r < q ? q : r );
}
#endif

/* Recompute the active layout metrics by scaling the user's base style profile to the
   active font's type size (em).  The base style is authored assuming em=12.  Invoked across
   the unit seam by gui_style_apply (frame/gui_frame_font.c), which reads the font metrics this
   unit must not touch and passes them in as parameters. */

void
layout_compute( u32 em, u32 char_h, u32 line_h )
{
    if ( em < 8u ) em = 8u;
    s_font_size = em;

    /* Original numbers based on font size of em=12.
       Scale the base metrics proportionally to the active font's type size. */

    f32 scale = (f32)em / 12.0f;

    /* Copy colors and enums */
    s_style = s_style_base;

    /* Scale pixel metrics proportionally -- grouped by the two gui_style_t categories.
       (grid_quantum and the enum-valued SKIN knobs are not pixel metrics; the struct copy
       above carries them.) */

    /* 1. METRICS */
    s_style.line_size       = (u8)( (f32)s_style_base.line_size       * scale );
    s_style.widget_gap      = (u8)( (f32)s_style_base.widget_gap      * scale );
    s_style.widget_pad      = (u8)( (f32)s_style_base.widget_pad      * scale );
    s_style.min_cell_w      = (u8)( (f32)s_style_base.min_cell_w      * scale );
    s_style.win_border      = (u8)( (f32)s_style_base.win_border      * scale );
    s_style.win_title_h     = (u8)( (f32)s_style_base.win_title_h     * scale );
    s_style.checkbox_sz     = (u8)( (f32)s_style_base.checkbox_sz     * scale );
    s_style.slider_knob_w   = (u8)( (f32)s_style_base.slider_knob_w   * scale );

    /* 2. SKIN */
    s_style.win_rounding     = (u8)( (f32)s_style_base.win_rounding     * scale );
    s_style.widget_rounding  = (u8)( (f32)s_style_base.widget_rounding  * scale );
    s_style.grab_rounding    = (u8)( (f32)s_style_base.grab_rounding    * scale );
    s_style.win_focus_border = (u8)( (f32)s_style_base.win_focus_border * scale );

    /* Widget knobs */
    s_style.checkmark_pad   = (u8)( (f32)s_style_base.checkmark_pad   * scale );
    s_style.cursor_w        = (u8)( (f32)s_style_base.cursor_w        * scale );
    s_style.cursor_inset    = (u8)( (f32)s_style_base.cursor_inset    * scale );

    /* The scale ramp steps are metrics like any other: em-scale each with the same factor. */
    for ( u32 i = 0; i < GUI_SCALE_COUNT; ++i )
    {
        s_style.scales[ i ].row = (u8)( (f32)s_style_base.scales[ i ].row * scale );
        s_style.scales[ i ].pad = (u8)( (f32)s_style_base.scales[ i ].pad * scale );
        s_style.scales[ i ].gap = (u8)( (f32)s_style_base.scales[ i ].gap * scale );
    }

    /* Prevent vanishing outlines or cursors when scaling down. */
    bool clamp_min_visible_metrics = true;
    if ( clamp_min_visible_metrics )
    {
        if ( s_style.win_border == 0 && s_style_base.win_border > 0 ) s_style.win_border = 1u;
        if ( s_style.cursor_w == 0   && s_style_base.cursor_w > 0 )   s_style.cursor_w = 1u;
        if ( s_style.widget_gap == 0 && s_style_base.widget_gap > 0 ) s_style.widget_gap = 1u;
        if ( s_style.win_focus_border == 0 && s_style_base.win_focus_border > 0 ) s_style.win_focus_border = 1u;
    }

    /* Floor the row height to the font's glyph box and line advance so a tall-boxed font
       (e.g. one with deep descenders) never clips and a single line of text always fits. */
    bool clamp_line_size_to_font = true;
    if ( clamp_line_size_to_font )
    {
        if ( s_style.line_size < char_h ) s_style.line_size = (u8)( char_h );
        if ( s_style.line_size < line_h ) s_style.line_size = (u8)( line_h );

        /* Every ramp row owes the same guarantee: a row always holds one line of text, so a
           font too tall for DENSE simply lifts it (the ramp compresses rather than clips). */
        for ( u32 i = 0; i < GUI_SCALE_COUNT; ++i )
        {
            if ( s_style.scales[ i ].row < char_h ) s_style.scales[ i ].row = (u8)( char_h );
            if ( s_style.scales[ i ].row < line_h ) s_style.scales[ i ].row = (u8)( line_h );
        }
    }

    /* Grid theme: snap the row-level layout metrics back onto the quantum lattice -- the em
       scale and the font floor above land on arbitrary pixels, and the whole point of the grid
       is that row pitch (line_size + gap), insets, and title bars share one divisor so nested
       regions stay seam-aligned.  Row height rounds UP so the font floor is never undone (text
       must still fit); everything else rounds to nearest.  Strokes and fine details (win_border,
       cursor_*, checkmark_pad, rounding) stay free -- a hairline or inset snapped to the lattice
       would quadruple, so they keep their scaled pixel value even when they shape geometry. */
#if GUI_GRID_LATTICE
    u32 q = s_style_base.grid_quantum;
    if ( q > 1 )
    {
        s_style.line_size     = (u8)lat_ceil( (f32)s_style.line_size, q );   /* ceil: keep font floor */
        s_style.widget_gap    = metric_quantize( s_style.widget_gap,    q );
        s_style.widget_pad    = metric_quantize( s_style.widget_pad,    q );
        s_style.min_cell_w    = metric_quantize( s_style.min_cell_w,    q );
        s_style.win_title_h   = metric_quantize( s_style.win_title_h,   q );
        s_style.checkbox_sz   = metric_quantize( s_style.checkbox_sz,   q );
        s_style.slider_knob_w = metric_quantize( s_style.slider_knob_w, q );

        /* Ramp steps land on the same lattice: rows ceil (keep the font floor), pads and gaps
           snap to nearest.  The whole ramp retunes together when the quantum or font changes. */
        for ( u32 i = 0; i < GUI_SCALE_COUNT; ++i )
        {
            s_style.scales[ i ].row = (u8)lat_ceil( (f32)s_style.scales[ i ].row, q );
            s_style.scales[ i ].pad = metric_quantize( s_style.scales[ i ].pad, q );
            s_style.scales[ i ].gap = metric_quantize( s_style.scales[ i ].gap, q );
        }
    }
#endif
}

// clang-format on
/*============================================================================================*/
