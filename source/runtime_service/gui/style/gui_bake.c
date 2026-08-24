/*==============================================================================================

    gui/style/gui_bake.c -- the bake: a seed palette -> the 40-cell colour grid.

    A theme authors a gui_palette_t: seven seed colours plus a ramp (gui.h).  A render reads
    gui_style_t.col: a 10-role x 4-phase grid of resolved colours.  gui_style_bake is the only
    function that turns one into the other, and the only writer of col[][] anywhere.

    It is pure -- a function of the palette argument alone, with no ambient state, no interact
    query, no draw call -- so the same palette always bakes the same grid, and a caller can bake
    a scratch gui_style_t that is never installed.  Two call sites drive it: gui_theme_set bakes
    on theme load, and the seed push in gui_style_core.c re-bakes whenever a seed is overridden
    live.

    Selection has no baked plane of its own: a selected read washes a resolved cell toward the
    accent live, at read time (style_wash_selected, gui_style_core.c), using bake_wash below --
    so a baked colour and a selected-time wash of it are the same formula and never disagree.

    Every blend here is a byte lerp in gamma-encoded sRGB, not linear light. Linear light is the
    right space for PHYSICAL mixing (alpha compositing, filtering); sRGB is the better fit for a
    perceptual ramp, because a fixed t reads as roughly one step size wherever it lands, where the
    same t in linear light is a barely-visible nudge near white and a jump near black.  OKLab is
    the drop-in upgrade if chromatic mixes ever show visible hue drift -- it swaps in behind
    bake_mix alone.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    The colour operations -- one mixer, four directional verbs, and one guard; the whole ramp
    is written in them.

    Alpha is never blended, only carried: every op keeps the alpha of its FIRST argument.
    That is what lets a translucent seed (a HUD panel at 0xF0) yield a translucent cell in
    all four phases without the author restating the alpha four times -- and it is why these
    are not col_lerp, which lerps alpha like any other channel.
==============================================================================================*/

#define BAKE_WHITE 0x00FFFFFFu   /* rgb only -- alpha comes from the blend's first argument */
#define BAKE_BLACK 0x00000000u

/* A cell nothing in the current widget set reads (confirmed by a full call-site audit,
   2026-08-14).  The grid stays uniform -- every role keeps all four phases, so a theme author
   never hits a hole -- but a dead cell bakes to this instead of a plausible colour, so a future
   accidental read is an obvious visual bug (loud magenta) rather than a quiet wrong guess. */
#define BAKE_UNUSED GUI_COLOR( 0xFF, 0x00, 0xFF, 0xFF )

/* Perceived brightness of a packed colour, 0..255 -- Rec. 601 luma with integer weights:
   ( 77 R + 150 G + 29 B ) / 256.  The eye weighs green most and blue least, and every
   light-or-dark judgement here (the pole pick, the ink guard's separation test) is made in
   this space rather than on raw channel values. */
static u32
bake_lum( u32 c )
{
    u32 r = ( c       ) & 0xFFu;
    u32 g = ( c >>  8 ) & 0xFFu;
    u32 b = ( c >> 16 ) & 0xFFu;

    return ( 77u * r + 150u * g + 29u * b ) >> 8;
}

/* THE mixing primitive: blend a toward b by t (0 = all a, 1 = all b).  Every directional verb
   below is this function with a different target.  The result keeps a's alpha untouched, and
   each channel rounds (+0.5f) so the float -> int cast never biases the ramp darker. */
static u32
bake_mix( u32 a, u32 b, f32 t )
{
    if ( t <= 0.0f ) return a;
    if ( t >= 1.0f ) t = 1.0f;

    u32 out = a & 0xFF000000u;                       /* alpha carried from a, never blended */

    for ( u32 sh = 0; sh <= 16; sh += 8 )            /* r, g, b */
    {
        f32 c0 = (f32)( ( a >> sh ) & 0xFFu );
        f32 c1 = (f32)( ( b >> sh ) & 0xFFu );

        out |= ( (u32)( c0 + ( c1 - c0 ) * t + 0.5f ) & 0xFFu ) << sh;
    }
    return out;
}

/*==============================================================================================
    The pole -- which way "forward" is.

    "Come forward" means lighter on a dark ground and darker on a light one.  The pole answers
    that once, from the ground's luma, and every lift in the plane uses the same answer -- so
    the derivation stays consistent even for the cells whose base IS the ground, which a
    per-colour comparison could not decide.

        bake_lift    toward the pole     (elevated, prominent)
        bake_recess  toward black        (a HOLE -- an empty track, a surface behind a scrim)
        bake_nest    signed, either way  (one rung of the surface ladder; the theme picks which)
        bake_fade    toward the ground   (retired: secondary ink, inert frames)
        bake_wash    toward the accent   (engaged: hover, press, selected)
==============================================================================================*/

static u32
bake_pole( u32 ground )
{
    return ( bake_lum( ground ) < 128u ) ? BAKE_WHITE : BAKE_BLACK;
}

static u32
bake_lift( u32 c, f32 t, u32 pole )
{
    return bake_mix( c, pole, t );
}

/* A hole in a surface, not a rung of the ladder: the empty half of a slider track, the pushed-in
   face of a pressed control, a panel behind an active modal's fence.  Always toward black, on
   either polarity -- a well, a dent and a scrim all read as an absence of light, and a theme that
   inverted them would show a RAISED empty track and a modal that brightened the page it fences. */
static u32
bake_recess( u32 c, f32 t )
{
    return bake_mix( c, BAKE_BLACK, t );
}

/* One rung DOWN the surface ladder -- a region nested inside its parent.  Which way "down" points
   is the theme's call, and t carries both the answer and the distance: positive sinks toward
   black, negative lifts toward the pole.

   It is a per-theme choice because the ground's own luma decides how much room the sink direction
   even has.  The dark built-in grounds at 0x24, so everything below it must fit two ladder rungs
   into 36 levels, where the same ramp above the ground has 219 to spend; a near-white ground has
   the opposite problem.  Splitting this off bake_recess is what lets a theme spend that headroom
   on the side it actually has, without also turning its empty tracks inside out. */
static u32
bake_nest( u32 c, f32 t, u32 pole )
{
    return ( t < 0.0f ) ? bake_mix( c, pole, -t ) : bake_mix( c, BAKE_BLACK, t );
}

static u32
bake_fade( u32 c, f32 t, u32 ground )
{
    return bake_mix( c, ground, t );
}

static u32
bake_wash( u32 c, f32 t, u32 accent )
{
    return bake_mix( c, accent, t );
}

/*==============================================================================================
    The ink guard -- the one op that can refuse to act.

    A theme author picks ink by eye against one surface, but the bake then sets that ink on
    grounds it derived itself (hovered, pressed, faded) that nobody eyeballed.  bake_ink_on
    measures the luma separation and, only when it is under the floor, pushes the ink exactly
    far enough to clear it -- a barely-failing ink gets a nudge, a wildly-failing one may land
    on the end of the greyscale.
==============================================================================================*/

#define BAKE_INK_DELTA 110   /* minimum ink-vs-face luma separation, of 255                    */
#define BAKE_DIM_DELTA  70   /* the same floor for SECONDARY ink, which is meant to be quieter */

/* Fixed SIGNAL strengths, deliberately not ramp-scaled.  The ramps are theme personality and a
   theme is free to run them near zero (a hover that barely tinges); these two are cues that must
   stay visible under any personality, so they do not dim with it. */
#define BAKE_DROP_WASH 0.20f   /* drop-target cue: how far PANEL / CHILD / TITLE wash toward DROP */
#define BAKE_BAND_WASH 0.15f   /* the title band's accent tinge over its lift                     */

static u32
bake_ink_on( u32 ink, u32 ground, i32 want )
{
    const i32 g = (i32)bake_lum( ground );
    const i32 i = (i32)bake_lum( ink    );

    if ( ( ( i > g ) ? i - g : g - i ) >= want ) return ink;   /* already legible -- leave it */

    /* Push along the side the ink already sits on, not the side the ground is on (a tie breaks
       by polarity: dark ground pushes up). */
    const bool up = ( i > g ) || ( i == g && g < 128 );

    /* Solve t directly for the blend that lands on the floor, no iteration:
           toward white: L(t) = i + ( 255 - i ) * t
           toward black: L(t) = i * ( 1 - t )
       An out-of-reach target solves to t > 1 and bake_mix clamps, so the answer degrades to
       the end of the greyscale -- the best separation available. */
    f32 t;
    if ( up ) t = ( i >= 255 ) ? 1.0f : (f32)( g + want - i ) / (f32)( 255 - i );
    else      t = ( i <=   0 ) ? 1.0f : 1.0f - (f32)( g - want ) / (f32)i;

    return bake_mix( ink, up ? BAKE_WHITE : BAKE_BLACK, t );
}

/*==============================================================================================
    The derivation -- ten roles, each a sentence in the verbs above.

    Written out role by role rather than driven from a data table, each block below is an
    editorial claim about what a phase MEANS for that role, and a table of opcodes would bury
    exactly the thing a theme author needs to read. Each claim is stated relative to the GROUND
    and INK bake_plane is handed, never to the raw palette seed -- see each role's comment below
    for what it claims and why.

    The severity ladder (INFO / OK / WARN / ERROR) is not one of the ten roles here: it lives
    in the extended palette (gui_style_ext_t, gui.h) as four flat colours, copied straight
    through rather than walked through these verbs -- see the loop at the end of gui_style_bake.
==============================================================================================*/

static void
bake_plane( u32 ( *col )[ GUI_PHASE_COUNT ], const gui_palette_t* p,
            u32 ground, u32 control, u32 ink )
{
    /* get the ramp and seed values from the palette for easy reference */

    const f32 hover  = p->ramp[ GUI_RAMP_HOVER  ];
    const f32 press  = p->ramp[ GUI_RAMP_PRESS  ];
    const f32 fade   = p->ramp[ GUI_RAMP_FADE   ];
    const f32 recess = p->ramp[ GUI_RAMP_RECESS ];
    const f32 nest   = p->ramp[ GUI_RAMP_NEST   ];
    const f32 step   = p->ramp[ GUI_RAMP_STEP   ];

    const u32 line   = p->seed[ GUI_SEED_LINE   ];
    const u32 accent = p->seed[ GUI_SEED_ACCENT ];
    const u32 mark   = p->seed[ GUI_SEED_MARK   ];
    const u32 grab   = p->seed[ GUI_SEED_GRAB   ];

    const u32 pole = bake_pole( ground );

    /* The BAND a caption sits on: the ground lifted half a step, then faintly accent-washed --
       kin to the panel under it rather than a clone of it.  TITLE's ACTIVE cell, and the base
       its HOT and INERT cells derive from. */

    const u32 band = bake_wash( bake_lift( ground, step * 0.50f, pole ), BAKE_BAND_WASH, accent );

    /* PANEL -- the window body.  Phase here does not track the cursor at all: a window's body
       covers too much screen for a per-pixel hover to tint without reading as noise.  Instead it
       tracks the window's OWN standing -- IDLE is any open, unfocused window; ACTIVE is the
       focused/foreground one, a faint lift so the eye can tell which surface owns input without
       the fill competing with content; HOT is freed from hover and repurposed as the drag-and-drop
       landing cue, either kind -- a window title-dragged over a dockspace (the dock's own
       gesture: the window genuinely IS the target, so no opt-in needed) or a generic
       drag_source_begin payload hovering a window opened with GUI_WIN_DRAG_TARGET (gui.h) --
       explicit, because most windows are scenery a drag passes over on its way to a widget target
       inside them, and lighting up every window under the cursor would claim "drop anywhere in
       me" for windows that mean nothing of the kind.  Either way it is a frame-level state, not a
       cursor-over-pixel one, so a wash toward the DROP hue reuses the same signal the dock's own
       drop overlay already carries.  INERT is a window sitting behind an active modal fence -- a
       scrim rather than a ladder rung, the same hole gui_stock_panel's framed backdrop and an
       empty dock-leaf placeholder already read; a nested child region has its own role below. */

    col[ GUI_ROLE_PANEL ][ GUI_PHASE_IDLE   ] = ground;
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_HOT    ] = bake_wash( ground, BAKE_DROP_WASH, p->ext[ GUI_EXT_DROP ] );
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_ACTIVE ] = bake_lift( ground, step * 0.1f, pole );
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_INERT  ] = bake_recess( ground, recess );

    /* PANEL_CHILD -- a nested container: scroll region, embedded child panel.  Same shape and the
       same standing-based reading as PANEL (see above), seeded from a NESTED ground instead of
       the bare one so a child reads one rung off its parent AT REST: IDLE is open; HOT is the same
       drop-target wash, gated the same way PANEL's generic path is -- only a child opened with
       GUI_WIN_DRAG_TARGET lights up while a payload hovers it (a reorderable list body, say); a
       plain child holding its own individually-targetable widgets (colour swatches, tree rows)
       stays flat and lets THEM ring instead (child_standing_phase, flow/gui_layout_child.c).
       There is no dock equivalent for a child -- it is not part of the dock tree -- so the flag is
       its only HOT source.  ACTIVE is a faint lift while the keyboard cursor is scoped inside THIS
       child, not the window; INERT is the same modal-fence scrim PANEL's INERT carries, cut into
       the child's own ground rather than the window's -- so a fenced child darkens whichever way
       the theme nests.  The resize edge has its own signal (BORDER, draw_resize_highlight) and
       does not touch this role at all. */

    const u32 child_ground = bake_nest( ground, nest, pole );

    col[ GUI_ROLE_PANEL_CHILD ][ GUI_PHASE_IDLE   ] = child_ground;
    col[ GUI_ROLE_PANEL_CHILD ][ GUI_PHASE_HOT    ] = bake_wash( child_ground, BAKE_DROP_WASH, p->ext[ GUI_EXT_DROP ] );
    col[ GUI_ROLE_PANEL_CHILD ][ GUI_PHASE_ACTIVE ] = bake_lift( child_ground, step * 0.1f, pole );
    col[ GUI_ROLE_PANEL_CHILD ][ GUI_PHASE_INERT  ] = bake_recess( child_ground, recess );

    /* TITLE -- a caption band is a LIFTED ground, which is why it needs no seed of its own.  Phase
       mirrors PANEL's standing-based reading, not the cursor: ACTIVE is the full band -- the
       focused window's bar, or a live tab, is the vivid one, the eye's first landing point; IDLE
       is the bare ground -- an inactive bar or a background tab sits flush and recedes, rather
       than outshining the window that actually has focus.  HOT is freed from hover for the same
       drop-target cue PANEL's HOT carries -- a title bar is as valid a landing zone to highlight
       as the body under it.  INERT is read only for a maximized window's titlebar
       (gui_window_end.c). */

    col[ GUI_ROLE_TITLE ][ GUI_PHASE_IDLE   ] = ground;
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_HOT    ] = bake_wash( band, BAKE_DROP_WASH, p->ext[ GUI_EXT_DROP ] );
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_ACTIVE ] = band;
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_INERT  ] = bake_fade( band, fade, ground );

    /* BG -- the control face.  Hot comes forward, active sinks back: that pair IS the pressed
       read, and it is the one place the two direction verbs are deliberately opposed.  Hover is
       a LUMINANCE event -- the lift does the talking and the hover ramp adds only a whisper of
       accent -- so the accent hue stays rationed for things that are chosen or engaged: the
       press (a deeper wash), selection, value fills, focus.  INERT is read only by gui_plot's own
       backdrop -- a plot has no id, so it never lifts.  That backdrop is a WELL, the unpainted
       area inside one widget's own footprint, so it sinks with the empty track ACCENT's INERT cell
       bakes to the same expression for -- it is not a container, and does not ride the nest
       ladder a child region does. */

    col[ GUI_ROLE_BG ][ GUI_PHASE_IDLE   ] = control;
    col[ GUI_ROLE_BG ][ GUI_PHASE_HOT    ] = bake_lift( bake_wash( control, hover, accent ), step, pole );
    col[ GUI_ROLE_BG ][ GUI_PHASE_ACTIVE ] = bake_recess( bake_wash( control, press, accent ), step );
    col[ GUI_ROLE_BG ][ GUI_PHASE_INERT  ] = bake_recess( control, recess );

    /* BORDER -- structural at rest, pure signal when live.  ACTIVE takes a second lift step so
       the focus ring reads brighter than a hovered edge -- the border carries focus, so the two
       cells must stay visibly distinct, the same HOT -> ACTIVE spacing the ACCENT row keeps.
       INERT frames the same permanently non-interactive surfaces PANEL's INERT cell does. */

    col[ GUI_ROLE_BORDER ][ GUI_PHASE_IDLE   ] = line;
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_HOT    ] = bake_lift( accent, step,        pole );
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_ACTIVE ] = bake_lift( accent, step * 2.0f, pole );
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_INERT  ] = bake_fade( line, fade, ground );

    /* TEXT_PRIMARY -- ink does not react BY CHOICE: IDLE/HOT/ACTIVE all start from the one ink,
       because text on a hot face is the same ink and it is the FACE that moved.  In practice
       nothing ever reads the HOT/ACTIVE cells -- every real "ink on a live face" site (button
       glyphs, tab ink) swaps to TEXT_SECONDARY's IDLE cell instead of asking this role for a
       different phase, so they bake to BAKE_UNUSED rather than a plausible-looking guard nobody
       exercises.  INERT is real: it is gui_text_disabled's hand-picked ink, a caller convention
       with no tie to GUI_ITEM_DISABLED (see GUI_STYLE -- PHASE in gui.h). */

    col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_IDLE   ] = bake_ink_on( ink, ground, BAKE_INK_DELTA );
    col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_HOT    ] = BAKE_UNUSED;
    col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_ACTIVE ] = BAKE_UNUSED;
    col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_INERT  ] = bake_ink_on( bake_fade( ink, fade, ground ), ground, BAKE_DIM_DELTA );

    /* TEXT_SECONDARY -- a permanently quieter ink, not a reaction to interaction: hints,
       captions, shortcuts, inactive labels.  Only IDLE is ever read (input hints, non-current tab
       ink) -- nothing sits secondary text on a hot or pressed face today, and nothing asks for a
       doubly-quiet secondary ink either, so HOT/ACTIVE/INERT all bake to BAKE_UNUSED. */

    col[ GUI_ROLE_TEXT_SECONDARY ][ GUI_PHASE_IDLE   ] = bake_ink_on( bake_fade( ink, fade, ground ), ground, BAKE_DIM_DELTA );
    col[ GUI_ROLE_TEXT_SECONDARY ][ GUI_PHASE_HOT    ] = BAKE_UNUSED;
    col[ GUI_ROLE_TEXT_SECONDARY ][ GUI_PHASE_ACTIVE ] = BAKE_UNUSED;
    col[ GUI_ROLE_TEXT_SECONDARY ][ GUI_PHASE_INERT  ] = BAKE_UNUSED;

    /* ACCENT -- the value a control holds: a straight three-cell lift (IDLE, +step, +2*step).
       INERT is the EMPTY track, so it comes off the CONTROL face, not the accent -- an empty
       slider well is a well, not a faded fill. */

    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_IDLE   ] = accent;
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_HOT    ] = bake_lift( accent, step,        pole );
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_ACTIVE ] = bake_lift( accent, step * 2.0f, pole );
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_INERT  ] = bake_recess( control, recess );
    
    /* MARK -- the indicator a control shows.  IDLE and ACTIVE are the same colour: a check does
       not change colour when pressed.  HOT is the nav ring, which is accent business, not the
       mark's.  Nothing ever asks for an inert mark -- a disabled checkbox's tick just gets the
       ambient DISABLED_ALPHA dim, not a separate cell -- so INERT bakes to BAKE_UNUSED. */
    col[ GUI_ROLE_MARK ][ GUI_PHASE_IDLE   ] = mark;
    col[ GUI_ROLE_MARK ][ GUI_PHASE_HOT    ] = bake_lift( accent, step, pole );
    col[ GUI_ROLE_MARK ][ GUI_PHASE_ACTIVE ] = mark;
    col[ GUI_ROLE_MARK ][ GUI_PHASE_INERT  ] = BAKE_UNUSED;

    /* GRAB -- the contrast anchor, authored opposite the theme's polarity and lifted furthest of
       any role, since a knob must stay legible against both a hovering track and a filled bar.
       There is no live INERT reader (a knob is always part of a live control), so it bakes to
       BAKE_UNUSED rather than a colour nothing shows. */

    col[ GUI_ROLE_GRAB ][ GUI_PHASE_IDLE   ] = grab;
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_HOT    ] = bake_lift( grab, step * 3.0f, pole );
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_ACTIVE ] = bake_lift( grab, step * 6.0f, pole );
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_INERT  ] = BAKE_UNUSED;
}
/*==============================================================================================

    Derive the theme full colour grid from s->palette.

==============================================================================================*/

void
gui_style_bake( gui_style_t* s )
{
    if ( !s ) return;

    const gui_palette_t* p = &s->palette;

    const u32 surface  = p->seed[ GUI_SEED_SURFACE ];
    const u32 control  = p->seed[ GUI_SEED_CONTROL ];
    const u32 ink      = p->seed[ GUI_SEED_INK     ];

    bake_plane( s->col, p, surface, control, ink );

    /* GUI_RAMP_SELECT is not spent here -- see the file header for where it is spent. */

    /* The extended palette's reserved slots are a straight copy, not a derivation: 
       there is no ramp to walk, so this just carries the authored colour through to 
       where style_ext expects to find it. */

    for ( u32 i = 0; i < GUI_EXT_RESERVED_COUNT; ++i )
    {
        s->ext[ i ] = p->ext[ i ];
    }
}

// clang-format on
/*============================================================================================*/
