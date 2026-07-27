/*==============================================================================================

    runtime_service/gui/style/gui_bake.c -- The bake: seven seeds and a ramp -> the 32-cell grid.

    The one step between what a theme AUTHORS (gui_palette_t) and what a render READS
    (gui_style_t.col), and the only writer of col[][] in the engine.  Pure: a function of the
    palette alone -- no ambient state, no interact query, no draw call, not even a read of the
    active style -- so the same palette always bakes the same grid, and a caller can bake a
    scratch gui_style_t that is not installed anywhere.

    Included by gui_style.c FIRST: gui_theme_set bakes on the way in, and the seed push in
    gui_style_core.c re-bakes into the working run, so both files below need it.

    WHY sRGB AND NOT LINEAR LIGHT.  Every blend here is a plain byte lerp in gamma-encoded sRGB,
    which is deliberate and is not the usual "we did not get around to it".  Linear light is the
    correct space for PHYSICAL mixing (alpha compositing, filtering); it is the wrong one for a
    perceptual RAMP.  Gamma-encoded sRGB is a rough approximation of perceptual lightness, so a
    fixed t reads as roughly the same size of step wherever it lands -- while the same t in
    linear light is a barely-visible nudge near white and a jump near black.  It is also the
    space the palettes were hand-tuned in, which is why the derived grid lands close to what
    those literals held.  If chromatic mixes ever show visible hue drift, OKLab is the upgrade
    and it swaps in behind bake_mix alone.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    The colour operations -- five verbs, and the whole ramp is written in them.

    Alpha is never blended, only carried: every op keeps the alpha of its FIRST argument.  That
    is what lets a translucent seed (a HUD panel at 0xF0) yield a translucent cell in all four
    phases without the author restating the alpha four times -- and it is why these are not
    col_lerp, which lerps alpha like any other channel.
==============================================================================================*/

#define BAKE_WHITE 0x00FFFFFFu   /* rgb only -- alpha comes from the blend's first argument */
#define BAKE_BLACK 0x00000000u

/* Perceptual weight of a colour, 0..255 -- Rec.601 luma.  Two callers: the pole decides the
   theme's polarity from it, and MUTE uses it as the grey a hue drains toward.  Integer: the bake
   runs at a landing, but there is no reason to pay for float here and the weights are exact in
   8 bits. */
static u32
bake_lum( u32 c )
{
    u32 r = ( c       ) & 0xFFu;
    u32 g = ( c >>  8 ) & 0xFFu;
    u32 b = ( c >> 16 ) & 0xFFu;
    return ( 77u * r + 150u * g + 29u * b ) >> 8;
}

/* Blend a toward b by t, keeping a's alpha.  THE primitive -- every op below is this plus a
   choice of b. */
static u32
bake_mix( u32 a, u32 b, f32 t )
{
    if ( t <= 0.0f ) return a;
    if ( t >= 1.0f ) t = 1.0f;

    u32 out = a & 0xFF000000u;   /* alpha rides through untouched */

    for ( u32 sh = 0; sh <= 16; sh += 8 )
    {
        f32 c0 = (f32)( ( a >> sh ) & 0xFFu );
        f32 c1 = (f32)( ( b >> sh ) & 0xFFu );
        out |= ( (u32)( c0 + ( c1 - c0 ) * t + 0.5f ) & 0xFFu ) << sh;
    }
    return out;
}

/* The theme's POLE -- the end of the greyscale a dark theme reaches toward and a light theme
   reaches away from, decided once from the surface.  Everything that has to "come forward" moves
   toward it: on a dark theme forward is lighter, on a light theme forward is darker.  Derived
   rather than authored, because a theme that had to declare its own polarity could declare it
   wrongly, and nothing else in the palette would make sense afterwards.

   An earlier cut asked the question per colour -- move away from whatever the surface's luma is
   -- which reads well until the colour IS the surface (the title band) and the comparison has no
   direction left to give: the tie-break fired one way for both polarities and light themes grew a
   title bar you could not see. */
static u32 bake_pole( u32 surface ) { return ( bake_lum( surface ) < 128u ) ? BAKE_WHITE : BAKE_BLACK; }

/* LIFT -- bring c forward, toward the pole.  The one direction verb, and the reason no role
   needs a light variant and a dark variant of its ramp. */
static u32 bake_lift( u32 c, f32 t, u32 pole )      { return bake_mix( c, pole, t ); }

/* RECESS -- sink c into the page.  Always toward black, in BOTH polarities, because a recessed
   well is a SHADOW and shadows do not invert with the theme: the old hand-authored light palette
   darkened its recessed panel and its empty track exactly as the dark one did.  What differs
   between the two is only HOW FAR, which is why recess is a ramp field the theme owns rather
   than a constant here. */
static u32 bake_recess( u32 c, f32 t )              { return bake_mix( c, BAKE_BLACK, t ); }

/* FADE -- retire c toward the surface it sits on.  The DIM phase for anything that is drawn ON a
   container: secondary text, a subdued frame, an inert mark. */
static u32 bake_fade( u32 c, f32 t, u32 surface )   { return bake_mix( c, surface, t ); }

/* WASH -- carry c toward the accent.  Interaction feedback: the hot and pressed steps of every
   surface that reacts. */
static u32 bake_wash( u32 c, f32 t, u32 accent )    { return bake_mix( c, accent, t ); }

/* MUTE -- drain the chroma out of c toward its own luma, leaving lightness alone.  Only the
   affirmative hues need it: a green check faded straight toward a grey surface stays a saturated
   green, which reads as "on but quiet" rather than "inert".  Draining first reads as off. */
static u32
bake_mute( u32 c, f32 t )
{
    u32 l = bake_lum( c );
    return bake_mix( c, l | ( l << 8 ) | ( l << 16 ), t );
}

/*==============================================================================================
    The derivation -- eight roles, each a sentence in the verbs above.

    Written out role by role rather than driven from a data table, because each row is an
    editorial claim about what that role MEANS across the phases, and a table of opcodes would
    bury exactly the thing a theme author needs to read.  The claims:

      PANEL   a container reacts WEAKLY -- it is background.  A fifth of the wash on hover with
              no lift behind it, the full wash when selected, and it sinks when recessed.
      TITLE   a caption band is a LIFTED surface, which is why it needs no seed of its own.  Its
              ACTIVE cell is the bare surface, so a live tab merges into the body it owns -- the
              one invariant the old literals had to be trusted to preserve by hand.
      BG      a control reacts FULLY: wash on hover then separate from the ground, wash on press
              then sink.  Hot rises, active deepens -- that is the whole pressed-button read.
      BORDER  a frame line is structural at rest and pure signal when live: hot and focused are
              both the lifted accent, which is why they were one literal twice.
      TEXT    ink does not react.  Three identical cells is not redundancy here, it is the claim:
              text on a hot face is the same ink, the FACE moved.  Only DIM differs.
      ACCENT  the value a control holds -- a straight three-step lift, the one honest ramp in the
              palette.  Its DIM is the EMPTY track, so it comes off the control seed, not off the
              accent: an empty slider well is a well, not a faded fill.
      MARK    the indicator a control shows.  IDLE and ACTIVE are the same mark (a check does not
              change colour when you press it); HOT is the nav ring, which is accent business.
      GRAB    the contrast anchor -- authored opposite the theme's polarity, so its ramp is a
              lift away from the ground and it travels FURTHEST, since a knob has to stay legible
              against both a hovering track and a filled bar at once.
==============================================================================================*/

void
gui_style_bake( gui_style_t* s )
{
    if ( !s ) return;

    const f32 hover   = s->palette.ramp[ GUI_RAMP_HOVER  ];
    const f32 press   = s->palette.ramp[ GUI_RAMP_PRESS  ];
    const f32 fade    = s->palette.ramp[ GUI_RAMP_FADE   ];
    const f32 recess  = s->palette.ramp[ GUI_RAMP_RECESS ];
    const f32 step    = s->palette.ramp[ GUI_RAMP_STEP   ];

    const u32 surface = s->palette.seed[ GUI_SEED_SURFACE ];
    const u32 control = s->palette.seed[ GUI_SEED_CONTROL ];
    const u32 ink     = s->palette.seed[ GUI_SEED_INK     ];
    const u32 line    = s->palette.seed[ GUI_SEED_LINE    ];
    const u32 accent  = s->palette.seed[ GUI_SEED_ACCENT  ];
    const u32 mark    = s->palette.seed[ GUI_SEED_MARK    ];
    const u32 grab    = s->palette.seed[ GUI_SEED_GRAB    ];

    const u32 pole = bake_pole( surface );

    /* The band a title sits in, derived once: both its IDLE cell and the base its DIM fades from
       are this lifted, faintly accented surface.  It lifts by a ramp STEP rather than by the
       recess, because the recess is authored per polarity (a light theme sinks far less than a
       dark one) and a caption band has to stay equally visible in both. */
    const u32 band = bake_wash( bake_lift( surface, step * 0.60f, pole ), hover * 0.12f, accent );

    u32 ( *col )[ GUI_PHASE_COUNT ] = s->col;

    /* PANEL -- the container.  A fifth of the hover and no lift at all: background should tint,
       not move.  Selection is the full wash, sunk half a step so a picked row reads as pressed
       rather than merely coloured. */
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_IDLE   ] = surface;
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_HOT    ] = bake_wash( surface, hover * 0.20f, accent );
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_ACTIVE ] = bake_recess( bake_wash( surface, press, accent ),
                                                             step * 0.50f );
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_DIM    ] = bake_recess( surface, recess );

    /* TITLE -- the lifted band.  ACTIVE is the bare surface: a live tab IS its panel. */
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_IDLE   ] = band;
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_HOT    ] = bake_lift( bake_wash( surface, hover, accent ),
                                                           step, pole );
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_ACTIVE ] = surface;
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_DIM    ] = bake_fade( band, fade, surface );

    /* BG -- the control face.  Hot comes forward, active sinks back: that pair IS the pressed
       read, and it is the one place the two direction verbs are deliberately opposed. */
    col[ GUI_ROLE_BG ][ GUI_PHASE_IDLE   ] = control;
    col[ GUI_ROLE_BG ][ GUI_PHASE_HOT    ] = bake_lift( bake_wash( control, hover, accent ),
                                                        step, pole );
    col[ GUI_ROLE_BG ][ GUI_PHASE_ACTIVE ] = bake_recess( bake_wash( control, press, accent ), step );
    col[ GUI_ROLE_BG ][ GUI_PHASE_DIM    ] = bake_recess( control, recess );

    /* BORDER -- structure at rest, signal when live. */
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_IDLE   ] = line;
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_HOT    ] = bake_lift( accent, step, pole );
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_ACTIVE ] = bake_lift( accent, step, pole );
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_DIM    ] = bake_fade( line, fade, surface );

    /* TEXT -- ink does not react; the face under it does. */
    col[ GUI_ROLE_TEXT ][ GUI_PHASE_IDLE   ] = ink;
    col[ GUI_ROLE_TEXT ][ GUI_PHASE_HOT    ] = ink;
    col[ GUI_ROLE_TEXT ][ GUI_PHASE_ACTIVE ] = ink;
    col[ GUI_ROLE_TEXT ][ GUI_PHASE_DIM    ] = bake_fade( ink, fade, surface );

    /* ACCENT -- the value held.  DIM is the empty track, so it comes off the CONTROL seed. */
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_IDLE   ] = accent;
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_HOT    ] = bake_lift( accent, step,        pole );
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_ACTIVE ] = bake_lift( accent, step * 2.0f, pole );
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_DIM    ] = bake_recess( control, recess );

    /* MARK -- the indicator shown.  HOT is the nav ring (accent business, not the mark's). */
    col[ GUI_ROLE_MARK ][ GUI_PHASE_IDLE   ] = mark;
    col[ GUI_ROLE_MARK ][ GUI_PHASE_HOT    ] = bake_lift( accent, step, pole );
    col[ GUI_ROLE_MARK ][ GUI_PHASE_ACTIVE ] = mark;
    col[ GUI_ROLE_MARK ][ GUI_PHASE_DIM    ] = bake_fade( bake_mute( mark, 0.80f ), fade, surface );

    /* GRAB -- the contrast anchor, and the longest ramp in the palette.  Its DIM fades further
       than any other role's: the anchor is the one colour authored to sit as far from the surface
       as the palette goes, so the ordinary fade leaves an inert knob still shouting. */
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_IDLE   ] = grab;
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_HOT    ] = bake_lift( grab, step * 3.0f, pole );
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_ACTIVE ] = bake_lift( grab, step * 6.0f, pole );
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_DIM    ] = bake_fade( grab, fade + ( 1.0f - fade ) * 0.40f,
                                                          surface );
}

// clang-format on
/*============================================================================================*/
