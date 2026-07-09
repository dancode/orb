/*==============================================================================================

    runtime_service/gui/0_foundation/gui_theme.c -- Theme registry + base style state + layout metrics.

    Owns the three pieces of style STATE that everything else in gui reads or scales from:
        k_themes     -- the built-in named presets (gui_theme_t), each a complete gui_style_t
                        authored for em=12.
        s_style_base -- the mutable user base style: a copy of the active theme, or freely edited
                        via gui_style_get() (theme_name then goes anonymous / NULL).
        s_style      -- s_style_base scaled to the active font's type size (em) by layout_compute;
                        every other file's WIDGET_ / WIN_ metrics and default colors ultimately
                        read this (through gui_style.c's push-stack resolver, gui_widget_core.c's
                        macros, gui_symbol.c's check/bullet/arrow style setters, ...).

    The theme API (theme_list/set/get/reset) and gui_style_get() are the public surface over that
    state; layout_compute is the font-driven rescale, called from gui_frame.c whenever a font
    loads or activates.  style_new_frame (gui_style.c) reseeds the push-stacks' base layer from
    s_style each frame -- forward-declared here since gui_theme_reset() calls it and gui_style.c
    is included right after this file.

    Included by gui.c FIRST among the Tier 0 files -- s_style must already be declared (file-scope
    static, no header) before 0_foundation/gui_style.c's style_var_base/style_col_base resolvers and every
    later tier's widget code can read it in the same TU.

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
static u32 s_font_size = 0;

/* Built-in theme registry.  Each entry is a complete gui_style_t authored for em=12;
   layout_compute scales the metrics to the active font.  Add more here; the array is const
   so its name pointers remain stable for the lifetime of the process. */
static const gui_theme_t k_themes[] =
{
    {
        "dark",
        {
            .colors = {
                [ GUI_COL_TEXT         ] = GUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF ),
                [ GUI_COL_TEXT_DIM     ] = GUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF ),
                [ GUI_COL_WINDOW_BG    ] = GUI_COLOR( 0x24, 0x24, 0x24, 0xE8 ),
                [ GUI_COL_CHILD_BG     ] = GUI_COLOR( 0x1C, 0x1C, 0x1C, 0xFF ),
                [ GUI_COL_TITLE_BG     ] = GUI_COLOR( 0x10, 0x60, 0xA0, 0xFF ),
                [ GUI_COL_BORDER       ] = GUI_COLOR( 0x80, 0x80, 0x80, 0xFF ),
                [ GUI_COL_WIDGET_BG    ] = GUI_COLOR( 0x40, 0x40, 0x40, 0xFF ),
                [ GUI_COL_WIDGET_HOT   ] = GUI_COLOR( 0x50, 0x80, 0xB0, 0xFF ),
                [ GUI_COL_WIDGET_ACT   ] = GUI_COLOR( 0x30, 0x60, 0x90, 0xFF ),
                [ GUI_COL_WIDGET_FG    ] = GUI_COLOR( 0x20, 0x90, 0xD0, 0xFF ),
                [ GUI_COL_CHECK_MARK   ] = GUI_COLOR( 0x18, 0xE6, 0x48, 0xFF ),
                [ GUI_COL_SLIDER_TRACK ] = GUI_COLOR( 0x30, 0x30, 0x30, 0xFF ),
                [ GUI_COL_RESIZE_HOT   ] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ),
                [ GUI_COL_INPUT_BG     ] = GUI_COLOR( 0x38, 0x38, 0x38, 0xFF ),
                [ GUI_COL_INPUT_FOCUS  ] = GUI_COLOR( 0x20, 0x50, 0x70, 0xFF ),
                [ GUI_COL_CURSOR       ] = GUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF ),
                [ GUI_COL_NAV_HIGHLIGHT] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ),
            },
            /* 1. LAYOUT CONTROLLER */
            .line_size          = 20,
            .widget_gap         = 4,
            .widget_pad         = 8,
            .min_cell_w         = 40,
            .grid_quantum       = 4,

            /* 2. STYLE */
            .win_border         = 1,
            .win_rounding       = 0,       /* geometric default: hard square corners everywhere */
            .widget_rounding    = 0,
            .grab_rounding      = 0,
            .check_style        = GUI_CHECK_TICK,
            .bullet_style       = GUI_BULLET_DISC,
            .arrow_style        = GUI_ARROW_FILLED,
            .separator_style    = GUI_SEPARATOR_SOLID,
            .progress_style     = GUI_PROGRESS_SOLID,
            .slider_knob        = GUI_SLIDER_KNOB_BAR,
            .menu_check         = GUI_MENU_CHECK_BOX,

            /* 3. WIDGET DRAWING STYLE */
            .win_title_h        = 24,
            .checkbox_sz        = 16,
            .slider_knob_w      = 12,
            .checkmark_pad      = 4,
            .cursor_w           = 1,
            .cursor_inset       = 3,

            .scales = {
                [ GUI_SCALE_DENSE ] = { .row = 16, .pad = 4,  .gap = 4 },
                [ GUI_SCALE_STD   ] = { .row = 20, .pad = 8,  .gap = 4 },   /* == the base metrics */
                [ GUI_SCALE_ROOMY ] = { .row = 24, .pad = 8,  .gap = 4 },
                [ GUI_SCALE_BAR   ] = { .row = 32, .pad = 12, .gap = 4 },
            },
        },
    },
    {
        /* "dark" with the soft-corner treatment: the same palette, rounding dialed on and a
           circular slider knob.  Not the default -- switch with gui()->theme_set( "rounded" )
           to compare the two looks. */
        "rounded",
        {
            .colors = {
                [ GUI_COL_TEXT         ] = GUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF ),
                [ GUI_COL_TEXT_DIM     ] = GUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF ),
                [ GUI_COL_WINDOW_BG    ] = GUI_COLOR( 0x24, 0x24, 0x24, 0xE8 ),
                [ GUI_COL_CHILD_BG     ] = GUI_COLOR( 0x1C, 0x1C, 0x1C, 0xFF ),
                [ GUI_COL_TITLE_BG     ] = GUI_COLOR( 0x10, 0x60, 0xA0, 0xFF ),
                [ GUI_COL_BORDER       ] = GUI_COLOR( 0x80, 0x80, 0x80, 0xFF ),
                [ GUI_COL_WIDGET_BG    ] = GUI_COLOR( 0x40, 0x40, 0x40, 0xFF ),
                [ GUI_COL_WIDGET_HOT   ] = GUI_COLOR( 0x50, 0x80, 0xB0, 0xFF ),
                [ GUI_COL_WIDGET_ACT   ] = GUI_COLOR( 0x30, 0x60, 0x90, 0xFF ),
                [ GUI_COL_WIDGET_FG    ] = GUI_COLOR( 0x20, 0x90, 0xD0, 0xFF ),
                [ GUI_COL_CHECK_MARK   ] = GUI_COLOR( 0x18, 0xE6, 0x48, 0xFF ),
                [ GUI_COL_SLIDER_TRACK ] = GUI_COLOR( 0x30, 0x30, 0x30, 0xFF ),
                [ GUI_COL_RESIZE_HOT   ] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ),
                [ GUI_COL_INPUT_BG     ] = GUI_COLOR( 0x38, 0x38, 0x38, 0xFF ),
                [ GUI_COL_INPUT_FOCUS  ] = GUI_COLOR( 0x20, 0x50, 0x70, 0xFF ),
                [ GUI_COL_CURSOR       ] = GUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF ),
                [ GUI_COL_NAV_HIGHLIGHT] = GUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF ),
            },
            /* 1. LAYOUT CONTROLLER */
            .line_size          = 20,
            .widget_gap         = 4,
            .widget_pad         = 8,
            .min_cell_w         = 40,
            .grid_quantum       = 4,

            /* 2. STYLE */
            .win_border         = 1,
            .win_rounding       = 8,
            .widget_rounding    = 5,
            .grab_rounding      = 6,
            .check_style        = GUI_CHECK_TICK,
            .bullet_style       = GUI_BULLET_DISC,
            .arrow_style        = GUI_ARROW_FILLED,
            .separator_style    = GUI_SEPARATOR_SOLID,
            .progress_style     = GUI_PROGRESS_SOLID,
            .slider_knob        = GUI_SLIDER_KNOB_CIRCLE,
            .menu_check         = GUI_MENU_CHECK_BOX,

            /* 3. WIDGET DRAWING STYLE */
            .win_title_h        = 24,
            .checkbox_sz        = 16,
            .slider_knob_w      = 12,
            .checkmark_pad      = 4,
            .cursor_w           = 1,
            .cursor_inset       = 3,

            .scales = {
                [ GUI_SCALE_DENSE ] = { .row = 16, .pad = 4,  .gap = 4 },
                [ GUI_SCALE_STD   ] = { .row = 20, .pad = 8,  .gap = 4 },   /* == the base metrics */
                [ GUI_SCALE_ROOMY ] = { .row = 24, .pad = 8,  .gap = 4 },
                [ GUI_SCALE_BAR   ] = { .row = 32, .pad = 12, .gap = 4 },
            },
        },
    },
    {
        "light",
        {
            .colors = {
                [ GUI_COL_TEXT         ] = GUI_COLOR( 0x10, 0x10, 0x10, 0xFF ),
                [ GUI_COL_TEXT_DIM     ] = GUI_COLOR( 0x60, 0x60, 0x60, 0xFF ),
                [ GUI_COL_WINDOW_BG    ] = GUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF ),
                [ GUI_COL_CHILD_BG     ] = GUI_COLOR( 0xE4, 0xE4, 0xE4, 0xFF ),
                [ GUI_COL_TITLE_BG     ] = GUI_COLOR( 0x20, 0x80, 0xC0, 0xFF ),
                [ GUI_COL_BORDER       ] = GUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF ),
                [ GUI_COL_WIDGET_BG    ] = GUI_COLOR( 0xD0, 0xD0, 0xD0, 0xFF ),
                [ GUI_COL_WIDGET_HOT   ] = GUI_COLOR( 0x60, 0xA0, 0xD0, 0xFF ),
                [ GUI_COL_WIDGET_ACT   ] = GUI_COLOR( 0x40, 0x80, 0xB0, 0xFF ),
                [ GUI_COL_WIDGET_FG    ] = GUI_COLOR( 0x20, 0x80, 0xC0, 0xFF ),
                [ GUI_COL_CHECK_MARK   ] = GUI_COLOR( 0x10, 0xA0, 0x30, 0xFF ),
                [ GUI_COL_SLIDER_TRACK ] = GUI_COLOR( 0xC0, 0xC0, 0xC0, 0xFF ),
                [ GUI_COL_RESIZE_HOT   ] = GUI_COLOR( 0x30, 0x90, 0xE0, 0xFF ),
                [ GUI_COL_INPUT_BG     ] = GUI_COLOR( 0xE8, 0xE8, 0xE8, 0xFF ),
                [ GUI_COL_INPUT_FOCUS  ] = GUI_COLOR( 0xC0, 0xD8, 0xF0, 0xFF ),
                [ GUI_COL_CURSOR       ] = GUI_COLOR( 0x10, 0x10, 0x60, 0xFF ),
                [ GUI_COL_NAV_HIGHLIGHT] = GUI_COLOR( 0x30, 0x90, 0xE0, 0xFF ),
            },
            /* 1. LAYOUT CONTROLLER */
            .line_size          = 20,
            .widget_gap         = 4,
            .widget_pad         = 8,
            .min_cell_w         = 40,
            .grid_quantum       = 4,

            /* 2. STYLE */
            .win_border         = 1,
            .win_rounding       = 6,
            .widget_rounding    = 4,
            .grab_rounding      = 4,
            .check_style        = GUI_CHECK_TICK,
            .bullet_style       = GUI_BULLET_DISC,
            .arrow_style        = GUI_ARROW_FILLED,
            .separator_style    = GUI_SEPARATOR_SOLID,
            .progress_style     = GUI_PROGRESS_SOLID,
            .slider_knob        = GUI_SLIDER_KNOB_BAR,
            .menu_check         = GUI_MENU_CHECK_BOX,

            /* 3. WIDGET DRAWING STYLE */
            .win_title_h        = 24,
            .checkbox_sz        = 16,
            .slider_knob_w      = 12,
            .checkmark_pad      = 4,
            .cursor_w           = 1,
            .cursor_inset       = 3,

            .scales = {
                [ GUI_SCALE_DENSE ] = { .row = 16, .pad = 4,  .gap = 4 },
                [ GUI_SCALE_STD   ] = { .row = 20, .pad = 8,  .gap = 4 },   /* == the base metrics */
                [ GUI_SCALE_ROOMY ] = { .row = 24, .pad = 8,  .gap = 4 },
                [ GUI_SCALE_BAR   ] = { .row = 32, .pad = 12, .gap = 4 },
            },
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
static gui_style_t s_style;

gui_style_t*
gui_style_get( void )
{
    /* Direct edits via this pointer are anonymous; the caller is responsible for calling
       gui_style_apply() and is advised to call gui_theme_reset() to clear push stacks. */
    s_theme_name = NULL;
    return &s_style_base;
}

/*----------------------------------------------------------------------------------------------
    Theme API -- named style snapshots that form the root of the push/pop stack.

    gui_theme_reset() is the "large style change" escape hatch: it restores s_style_base from
    the active named theme (if any), rescales the metrics, and immediately clears both the color
    and var push stacks -- so callers never need to issue paired pop calls just to get back to a
    clean base state.

    style_new_frame() is static and lives in gui_style.c (included right after this file);
    declare it here so the call in gui_theme_reset() resolves within the same TU.
----------------------------------------------------------------------------------------------*/

static void style_new_frame( void );  /* forward -- defined in gui_style.c */

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

/* Snap a scaled metric onto the grid lattice: nearest multiple of q, floored at one quantum so
   a nonzero authored metric never vanishes.  Zero stays zero (an authored "none" is preserved). */
static u8
metric_quantize( u32 v, u32 q )
{
    if ( v == 0 ) return 0;
    u32 r = ( ( v + q / 2 ) / q ) * q;
    return (u8)( r < q ? q : r );
}

/* Recompute the active layout metrics by scaling the user's base style profile to the
   active font's type size (em).  The base style is authored assuming em=12. */
static void
layout_compute( u32 em, u32 char_h, u32 line_h )
{
    if ( em < 8u ) em = 8u;
    s_font_size = em;

    f32 scale = (f32)em / 12.0f;

    /* Copy colors and enums */
    s_style = s_style_base;

    /* Scale pixel metrics proportionally -- grouped by the three gui_style_t categories.
       (grid_quantum and the enum-valued STYLE knobs are not pixel metrics; the struct copy
       above carries them.) */

    /* 1. LAYOUT CONTROLLER */
    s_style.line_size       = (u8)( (f32)s_style_base.line_size       * scale );
    s_style.widget_gap      = (u8)( (f32)s_style_base.widget_gap      * scale );
    s_style.widget_pad      = (u8)( (f32)s_style_base.widget_pad      * scale );
    s_style.min_cell_w      = (u8)( (f32)s_style_base.min_cell_w      * scale );

    /* 2. STYLE */
    s_style.win_border      = (u8)( (f32)s_style_base.win_border      * scale );
    s_style.win_rounding    = (u8)( (f32)s_style_base.win_rounding    * scale );
    s_style.widget_rounding = (u8)( (f32)s_style_base.widget_rounding * scale );
    s_style.grab_rounding   = (u8)( (f32)s_style_base.grab_rounding   * scale );

    /* 3. WIDGET DRAWING STYLE */
    s_style.win_title_h     = (u8)( (f32)s_style_base.win_title_h     * scale );
    s_style.checkbox_sz     = (u8)( (f32)s_style_base.checkbox_sz     * scale );
    s_style.slider_knob_w   = (u8)( (f32)s_style_base.slider_knob_w   * scale );
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
       cursor_*, checkmark_pad, rounding) stay free -- they are drawing weights, not layout. */
    u32 q = s_style_base.grid_quantum;
    if ( q > 1 )
    {
        u32 row = ( ( (u32)s_style.line_size + q - 1 ) / q ) * q;   /* ceil: keep the font floor */
        s_style.line_size     = (u8)row;
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
            u32 sr = ( ( (u32)s_style.scales[ i ].row + q - 1 ) / q ) * q;
            s_style.scales[ i ].row = (u8)sr;
            s_style.scales[ i ].pad = metric_quantize( s_style.scales[ i ].pad, q );
            s_style.scales[ i ].gap = metric_quantize( s_style.scales[ i ].gap, q );
        }
    }
}

// clang-format on
/*============================================================================================*/
