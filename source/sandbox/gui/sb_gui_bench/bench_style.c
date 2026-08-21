/*==============================================================================================

    sandbox/gui/sb_gui_bench/bench_style.c -- the style suite: what a look costs.

    One shared composite scene swept two ways.  The four built-in themes answer "do the shipped
    looks differ" (they differ only in gap / rounding / knob metrics, so near-identical gpu is
    the expected -- and now measured -- answer).  The synthesized variants are the real "does
    simpler save" axis: each pushes a handful of style overrides for the frame and emits the
    same scene, so shadows, rounding, borders and corner smoothing are priced one lever at a
    time against the stock look.

    Overrides are per-frame pushes rather than style_edit mutations: the push stacks reset at
    every ctx_begin, so a wrapper that pushes, emits, and pops leaves nothing to restore when
    the runner moves to the next case.

==============================================================================================*/
// clang-format off

static void
scene_style_stock( const bench_case_t* c, u32 frame )
{
    scene_composite( c, frame );
}

static void
scene_style_no_shadow( const bench_case_t* c, u32 frame )
{
    gui()->push_style_var( GUI_VAR_SHADOW, 0.0f );
    gui()->push_style_ext( GUI_EXT_SHADOW, 0x00000000u );
    scene_composite( c, frame );
    gui()->pop_style_var( 1 );
    gui()->pop_style_ext( 1 );
}

static void
scene_style_round0( const bench_case_t* c, u32 frame )
{
    gui()->push_style_var( GUI_VAR_ROUND,       0.0f );
    gui()->push_style_var( GUI_VAR_PANEL_ROUND, 0.0f );
    scene_composite( c, frame );
    gui()->pop_style_var( 2 );
}

static void
scene_style_round8( const bench_case_t* c, u32 frame )
{
    gui()->push_style_var( GUI_VAR_ROUND,       8.0f  );
    gui()->push_style_var( GUI_VAR_PANEL_ROUND, 12.0f );
    scene_composite( c, frame );
    gui()->pop_style_var( 2 );
}

static void
scene_style_no_border( const bench_case_t* c, u32 frame )
{
    gui()->push_style_var( GUI_VAR_BORDER,     0.0f );
    gui()->push_style_var( GUI_VAR_FOCUS_RING, 0.0f );
    scene_composite( c, frame );
    gui()->pop_style_var( 2 );
}

static void
scene_style_smooth( const bench_case_t* c, u32 frame )
{
    gui()->push_style_var( GUI_VAR_ROUND,         8.0f  );
    gui()->push_style_var( GUI_VAR_PANEL_ROUND,   12.0f );
    gui()->push_style_var( GUI_VAR_CORNER_SMOOTH, 1.0f  );
    scene_composite( c, frame );
    gui()->pop_style_var( 3 );
}

/* Everything off at once -- the floor a maximally plain look actually reaches. */
static void
scene_style_flat_min( const bench_case_t* c, u32 frame )
{
    gui()->push_style_var( GUI_VAR_SHADOW,        0.0f );
    gui()->push_style_var( GUI_VAR_ROUND,         0.0f );
    gui()->push_style_var( GUI_VAR_PANEL_ROUND,   0.0f );
    gui()->push_style_var( GUI_VAR_BORDER,        0.0f );
    gui()->push_style_var( GUI_VAR_FOCUS_RING,    0.0f );
    gui()->push_style_var( GUI_VAR_CORNER_SMOOTH, 0.0f );
    gui()->push_style_ext( GUI_EXT_SHADOW, 0x00000000u );
    scene_composite( c, frame );
    gui()->pop_style_var( 6 );
    gui()->pop_style_ext( 1 );
}

static const bench_case_t k_style_cases[] =
{
    { "style", "style_dark",    "built-in theme: dark",    false, "dark",    scene_style_stock, 0 },
    { "style", "style_rounded", "built-in theme: rounded", false, "rounded", scene_style_stock, 0 },
    { "style", "style_light",   "built-in theme: light",   false, "light",   scene_style_stock, 0 },
    { "style", "style_quantum", "built-in theme: quantum", false, "quantum", scene_style_stock, 0 },

    { "style", "style_no_shadow", "dark, elevation shadows off",     false, "dark",
      scene_style_no_shadow, 0 },
    { "style", "style_round0",    "dark, all rounding forced 0",     false, "dark",
      scene_style_round0, 0 },
    { "style", "style_round8",    "dark, rounding forced 8 / 12",    false, "dark",
      scene_style_round8, 0 },
    { "style", "style_no_border", "dark, borders + focus ring off",  false, "dark",
      scene_style_no_border, 0 },
    { "style", "style_smooth",    "dark, rounded + corner smoothing", false, "dark",
      scene_style_smooth, 0 },
    { "style", "style_flat_min",  "dark, every skin lever off",      false, "dark",
      scene_style_flat_min, 0 },
};

// clang-format on
/*============================================================================================*/
