/*==============================================================================================

    runtime_service/gui/style/gui_bake.c -- The bake: a seed palette -> the 36-cell colour grid.

    The one step between what a theme AUTHORS (gui_palette_t) and what a render READS
    (gui_style_t.col), and the only writer of col[][] in the engine.  Pure: a function of the
    palette alone -- no ambient state, no interact query, no draw call, not even a read of the
    active style -- so the same palette always bakes the same grid, and a caller can bake a
    scratch gui_style_t that is not installed anywhere.

    Included by gui_style.c FIRST: gui_theme_set bakes on the way in, and the seed push in
    gui_style_core.c re-bakes into the working run, so both files below need it.

    There is no SELECT plane to bake: a selected read washes a resolved cell toward the accent
    LIVE (style_wash_selected, gui_style_core.c), spending bake_wash below -- the same formula
    this file derives every ramp step with, so a bake-time colour and a selected-time wash of it
    never disagree.

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
    The colour operations -- six verbs, and the whole ramp is written in them.

    Alpha is never blended, only carried: every op keeps the alpha of its FIRST argument.  That
    is what lets a translucent seed (a HUD panel at 0xF0) yield a translucent cell in all four
    phases without the author restating the alpha four times -- and it is why these are not
    col_lerp, which lerps alpha like any other channel.
==============================================================================================*/

#define BAKE_WHITE 0x00FFFFFFu   /* rgb only -- alpha comes from the blend's first argument */
#define BAKE_BLACK 0x00000000u

/* Perceptual weight of a colour, 0..255 -- Rec.601 luma.  Three callers: the pole decides a
   plane's polarity from it, MUTE uses it as the grey a hue drains toward, and the ink guard
   measures separation with it.  Integer: the bake runs at a landing, but there is no reason to
   pay for float here and the weights are exact in 8 bits. */
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

/* A plane's POLE -- the end of the greyscale a dark ground reaches toward and a light ground
   reaches away from, decided once from the ground.  Everything that has to "come forward" moves
   toward it: over a dark ground forward is lighter, over a light one forward is darker.  Derived
   rather than authored, because a theme that had to declare its own polarity could declare it
   wrongly, and nothing else in the palette would make sense afterwards -- and because the SELECT
   plane's ground is itself derived, so there is nobody to ask.

   An earlier cut asked the question per colour -- move away from whatever the ground's luma is --
   which reads well until the colour IS the ground (the title band) and the comparison has no
   direction left to give: the tie-break fired one way for both polarities and light themes grew a
   title bar you could not see. */
static u32 bake_pole( u32 ground ) { return ( bake_lum( ground ) < 128u ) ? BAKE_WHITE : BAKE_BLACK; }

/* LIFT -- bring c forward, toward the pole.  The one direction verb, and the reason no role
   needs a light variant and a dark variant of its ramp. */
static u32 bake_lift( u32 c, f32 t, u32 pole )      { return bake_mix( c, pole, t ); }

/* RECESS -- sink c into the page.  Always toward black, in BOTH polarities, because a recessed
   well is a SHADOW and shadows do not invert with the theme: the old hand-authored light palette
   darkened its recessed panel and its empty track exactly as the dark one did.  What differs
   between the two is only HOW FAR, which is why recess is a ramp field the theme owns rather
   than a constant here. */
static u32 bake_recess( u32 c, f32 t )              { return bake_mix( c, BAKE_BLACK, t ); }

/* FADE -- retire c toward the ground it sits on.  The DIM phase for anything drawn ON a
   container: secondary text, a subdued frame, an inert mark, a status banner. */
static u32 bake_fade( u32 c, f32 t, u32 ground )    { return bake_mix( c, ground, t ); }

/* WASH -- carry c toward the accent.  Interaction feedback (the hot and pressed steps of every
   surface that reacts) and, at the SELECT ramp, the chosen ground itself. */
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

/* The ink GUARD -- the one invariant in the bake, and the only op that can decline to act.

   Ink is the single colour in the palette with a hard legibility floor, and it is the only one
   that has to survive faces it never met: a theme author picks an ink by looking at it on the
   SURFACE, and the bake then puts it on a hovered control, a pressed one, a selection fill and
   a pressed selection -- six derived grounds, none of which anyone eyeballed.  A light theme is
   where it bites: near-black ink is fine on 0xE2E2E6 and marginal on a pressed blue.

   So: measure the luma separation the ink actually has from the ground it is about to sit on,
   and if it falls short, push it FURTHER ONTO THE SIDE IT IS ALREADY ON by precisely enough to
   reach the floor.  Precisely, not by a guessed nudge -- luma is linear in the channels and
   bake_mix is linear in t, and each end of the greyscale has luma 255 or 0, so the t that lands
   exactly on the floor solves in one divide.

   The direction rule is "stay on your side", NOT bake_pole, and the difference is load-bearing.
   A pole-based guard asks whether the GROUND is dark, which flips at luma 128 -- so a face that
   drifts across the boundary under a hover animation would invert its label mid-transition, and
   a light theme's pressed button (which lands at luma 123) would take WHITE text while the same
   button one shade lighter took black.  Extending the separation the ink already has is
   continuous in the ink/ground relationship and keeps a dark-ink theme dark-inked.  The pole is
   only the tie-break for the degenerate case where the two luminances are exactly equal, which
   is the one time "your side" does not exist.

   Note what it does NOT do: it never touches ink that is already legible.  Both built-in themes
   clear the floor on almost every cell, so this is very nearly a no-op for them and stays out of
   the way of a hand-tuned palette; it exists to rescue the cells nobody looked at.

   And note what it deliberately does not COVER: the status hues are left exactly as authored.
   Ink is guarded because it composites against six derived faces the author never sees; a status
   seed composites against one ground the author is looking at while picking it.  Guarding those
   too would be the bake overruling a deliberate editorial choice on the strength of a number. */

#define BAKE_INK_DELTA 110   /* minimum ink-vs-face luma separation, of 255                    */
#define BAKE_DIM_DELTA  70   /* the same floor for SECONDARY ink, which is meant to be quieter */

static u32
bake_ink_on( u32 ink, u32 ground, i32 want )
{
    const i32 g = (i32)bake_lum( ground );
    const i32 i = (i32)bake_lum( ink    );

    if ( ( ( i > g ) ? i - g : g - i ) >= want ) return ink;   /* already legible -- leave it */

    /* Which way is "further from the ground"?  The side the ink is already on; if it is on
       neither (equal luma), fall back to the ground's own polarity. */
    const bool up = ( i > g ) || ( i == g && g < 128 );

    /* Solve bake_mix's t for the exact floor.  Toward white: L(t) = i + (255 - i) * t.
       Toward black: L(t) = i * (1 - t).  Out-of-reach targets solve to t > 1 and bake_mix
       clamps, which lands on the end of the greyscale -- the best available answer. */
    f32 t;
    if ( up ) t = ( i >= 255 ) ? 1.0f : (f32)( g + want - i ) / (f32)( 255 - i );
    else      t = ( i <=   0 ) ? 1.0f : 1.0f - (f32)( g - want ) / (f32)i;

    return bake_mix( ink, up ? BAKE_WHITE : BAKE_BLACK, t );
}

/*==============================================================================================
    The derivation -- nine roles, each a sentence in the verbs above.

    Written out role by role rather than driven from a data table, because each row is an
    editorial claim about what that role MEANS across the phases, and a table of opcodes would
    bury exactly the thing a theme author needs to read.  The four status rows ARE a loop, and
    for the opposite reason: a severity ladder is one claim instanced four times, so writing it
    out four times would only invite the four copies to drift apart.

    Everything here is stated relative to the GROUND and the INK it is handed, never to the
    surface seed -- that is what makes the same nine sentences correct for a selection.  The
    claims:

      PANEL   a container reacts WEAKLY -- it is background.  A fifth of the wash on hover with
              no lift behind it, the full wash when pressed, and it sinks when recessed.
      TITLE   a caption band is a LIFTED ground, which is why it needs no seed of its own.  Its
              ACTIVE cell is the bare ground, so a live tab merges into the body it owns -- the
              one invariant the old literals had to be trusted to preserve by hand.
      BG      a control reacts FULLY: wash on hover then separate from the ground, wash on press
              then sink.  Hot rises, active deepens -- that is the whole pressed-button read.
      BORDER  a frame line is structural at rest and pure signal when live: hot and focused are
              both the lifted accent, which is why they were one literal twice.
      TEXT_PRIMARY    ink does not react.  IDLE/HOT/ACTIVE being near-identical is not redundancy
              here, it is the claim: text on a hot face is the same ink, the FACE moved.  Only
              DIM differs, and DIM means disabled.
      TEXT_SECONDARY  the same non-reaction claim, at a permanently quieter step -- not a phase
              response, a standing choice for hints, captions and inactive labels.
      ACCENT  the value a control holds -- a straight three-step lift, the one honest ramp in the
              palette.  Its DIM is the EMPTY track, so it comes off the control face, not off the
              accent: an empty slider well is a well, not a faded fill.
      MARK    the indicator a control shows.  IDLE and ACTIVE are the same mark (a check does not
              change colour when you press it); HOT is the nav ring, which is accent business.
      GRAB    the contrast anchor -- authored opposite the theme's polarity, so its ramp is a
              lift away from the ground and it travels FURTHEST, since a knob has to stay legible
              against both a hovering track and a filled bar at once.

    The severity ladder (INFO / OK / WARN / ERROR) used to close this list as a tenth-through-
    thirteenth claim -- a signal at three lift steps and then the FIELD, DIM dropping the hue
    nearly into the ground for a banner rather than a mark.  It is not derived here any more: the
    extended palette (gui_style_ext_t, gui.h) carries those four as flat colours, copied straight
    from the palette rather than walked through bake_plane's verbs.
==============================================================================================*/

static void
bake_plane( u32 ( *col )[ GUI_PHASE_COUNT ], const gui_palette_t* p,
            u32 ground, u32 control, u32 ink )
{
    const f32 hover  = p->ramp[ GUI_RAMP_HOVER  ];
    const f32 press  = p->ramp[ GUI_RAMP_PRESS  ];
    const f32 fade   = p->ramp[ GUI_RAMP_FADE   ];
    const f32 recess = p->ramp[ GUI_RAMP_RECESS ];
    const f32 step   = p->ramp[ GUI_RAMP_STEP   ];

    const u32 line   = p->seed[ GUI_SEED_LINE   ];
    const u32 accent = p->seed[ GUI_SEED_ACCENT ];
    const u32 mark   = p->seed[ GUI_SEED_MARK   ];
    const u32 grab   = p->seed[ GUI_SEED_GRAB   ];

    const u32 pole = bake_pole( ground );

    /* The band a title sits in, derived once: both its IDLE cell and the base its DIM fades from
       are this lifted, faintly accented ground.  It lifts by a ramp STEP rather than by the
       recess, because the recess is authored per polarity (a light theme sinks far less than a
       dark one) and a caption band has to stay equally visible in both. */
    const u32 band = bake_wash( bake_lift( ground, step * 0.60f, pole ), hover * 0.12f, accent );

    /* PANEL -- the container.  A fifth of the hover and no lift at all: background should tint,
       not move.  Pressed is the full wash, sunk half a step so a held surface reads as pressed
       rather than merely coloured. */
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_IDLE   ] = ground;
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_HOT    ] = bake_wash( ground, hover * 0.20f, accent );
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_ACTIVE ] = bake_recess( bake_wash( ground, press, accent ), step * 0.50f );
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_DIM    ] = bake_recess( ground, recess );

    /* TITLE -- the lifted band.  ACTIVE is the bare ground: a live tab IS its panel. */
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_IDLE   ] = band;
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_HOT    ] = bake_lift( bake_wash( ground, hover, accent ), step, pole );
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_ACTIVE ] = ground;
    col[ GUI_ROLE_TITLE ][ GUI_PHASE_DIM    ] = bake_fade( band, fade, ground );

    /* BG -- the control face.  Hot comes forward, active sinks back: that pair IS the pressed
       read, and it is the one place the two direction verbs are deliberately opposed. */
    col[ GUI_ROLE_BG ][ GUI_PHASE_IDLE   ] = control;
    col[ GUI_ROLE_BG ][ GUI_PHASE_HOT    ] = bake_lift( bake_wash( control, hover, accent ), step, pole );
    col[ GUI_ROLE_BG ][ GUI_PHASE_ACTIVE ] = bake_recess( bake_wash( control, press, accent ), step );
    col[ GUI_ROLE_BG ][ GUI_PHASE_DIM    ] = bake_recess( control, recess );

    /* BORDER -- structure at rest, signal when live.  ACTIVE takes a second lift step so the
       focus ring reads brighter than a hovered edge -- the HOT -> ACTIVE spacing the ACCENT row
       keeps.  When the two cells were equal, "hovered edge" and "focused ring" were the same
       colour by construction, which quietly voided the border-carries-focus rule. */
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_IDLE   ] = line;
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_HOT    ] = bake_lift( accent, step,        pole );
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_ACTIVE ] = bake_lift( accent, step * 2.0f, pole );
    col[ GUI_ROLE_BORDER ][ GUI_PHASE_DIM    ] = bake_fade( line, fade, ground );

    /* TEXT_PRIMARY -- ink does not react BY CHOICE: IDLE/HOT/ACTIVE all start from the one ink,
       because text on a hot face is the same ink and it is the FACE that moved.  Each is then
       guarded against the face it will actually sit on (bake_ink_on), which is the only thing
       that can make them differ -- and only by as much as legibility demands.  Written after BG
       deliberately: the guard reads the very cells above.

       HOT and ACTIVE guard against BG, not PANEL or TITLE, because BG is the face that travels
       furthest -- it takes the full wash plus a lift or a sink, where a container takes a fifth
       of the wash.  Clear BG and the quieter surfaces are clear by construction.

       DIM is the disabled cell, like every other role's DIM -- not a "secondary" reading, that
       is TEXT_SECONDARY's job below.  It still has a floor of its own, and a lower one than the
       enabled cells: a disabled label is MEANT to recede, so holding it to the body-text
       separation would defeat the fade that makes it read as disabled.  It is still a floor --
       receding is not the same as disappearing. */
    col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_IDLE   ] = bake_ink_on( ink, ground, BAKE_INK_DELTA );
    col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_HOT    ] = bake_ink_on( ink, col[ GUI_ROLE_BG ][ GUI_PHASE_HOT ],
                                                                    BAKE_INK_DELTA );
    col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_ACTIVE ] = bake_ink_on( ink, col[ GUI_ROLE_BG ][ GUI_PHASE_ACTIVE ],
                                                                    BAKE_INK_DELTA );
    col[ GUI_ROLE_TEXT_PRIMARY ][ GUI_PHASE_DIM    ] = bake_ink_on( bake_fade( ink, fade, ground ), ground,
                                                                    BAKE_DIM_DELTA );

    /* TEXT_SECONDARY -- a permanently quieter ink, not a reaction to interaction: hints,
       captions, shortcuts, inactive labels.  IDLE is the same faded-and-guarded formula
       TEXT_PRIMARY's DIM carries above -- the "secondary" look this role now owns outright.
       HOT/ACTIVE stay the same ink, guarded against BG, for the identical reason PRIMARY's do: a
       secondary label can still sit on a hot or pressed face.  DIM fades once further, for a
       secondary label inside a disabled control (a hint in a disabled field), at the same floor
       -- receding further is the point, not a bug to guard away. */
    const u32 sec_idle = bake_ink_on( bake_fade( ink, fade, ground ), ground, BAKE_DIM_DELTA );
    col[ GUI_ROLE_TEXT_SECONDARY ][ GUI_PHASE_IDLE   ] = sec_idle;
    col[ GUI_ROLE_TEXT_SECONDARY ][ GUI_PHASE_HOT    ] = bake_ink_on( sec_idle, col[ GUI_ROLE_BG ][ GUI_PHASE_HOT ],
                                                                      BAKE_DIM_DELTA );
    col[ GUI_ROLE_TEXT_SECONDARY ][ GUI_PHASE_ACTIVE ] = bake_ink_on( sec_idle, col[ GUI_ROLE_BG ][ GUI_PHASE_ACTIVE ],
                                                                      BAKE_DIM_DELTA );
    col[ GUI_ROLE_TEXT_SECONDARY ][ GUI_PHASE_DIM    ] = bake_fade( sec_idle, fade + ( 1.0f - fade ) * 0.50f,
                                                                    ground );

    /* ACCENT -- the value held.  DIM is the empty track, so it comes off the CONTROL face. */
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_IDLE   ] = accent;
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_HOT    ] = bake_lift( accent, step,        pole );
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_ACTIVE ] = bake_lift( accent, step * 2.0f, pole );
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_DIM    ] = bake_recess( control, recess );

    /* MARK -- the indicator shown.  HOT is the nav ring (accent business, not the mark's). */
    col[ GUI_ROLE_MARK ][ GUI_PHASE_IDLE   ] = mark;
    col[ GUI_ROLE_MARK ][ GUI_PHASE_HOT    ] = bake_lift( accent, step, pole );
    col[ GUI_ROLE_MARK ][ GUI_PHASE_ACTIVE ] = mark;
    col[ GUI_ROLE_MARK ][ GUI_PHASE_DIM    ] = bake_fade( bake_mute( mark, 0.80f ), fade, ground );

    /* GRAB -- the contrast anchor, and the longest ramp in the palette.  Its DIM fades further
       than any other role's: the anchor is the one colour authored to sit as far from the ground
       as the palette goes, so the ordinary fade leaves an inert knob still shouting. */
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_IDLE   ] = grab;
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_HOT    ] = bake_lift( grab, step * 3.0f, pole );
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_ACTIVE ] = bake_lift( grab, step * 6.0f, pole );
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_DIM    ] = bake_fade( grab, fade + ( 1.0f - fade ) * 0.40f,
                                                          ground );
}

void
gui_style_bake( gui_style_t* s )
{
    if ( !s ) return;

    const gui_palette_t* p = &s->palette;

    const u32 surface = p->seed[ GUI_SEED_SURFACE ];
    const u32 control = p->seed[ GUI_SEED_CONTROL ];
    const u32 ink     = p->seed[ GUI_SEED_INK     ];

    bake_plane( s->col, p, surface, control, ink );

    /* GUI_RAMP_SELECT is not spent here: it washes a resolved cell toward the accent LIVE, at
       read time (style_wash_selected, gui_style_core.c), rather than baking a second plane. */

    /* The extended palette's reserved slots are a straight copy, not a derivation -- there is no
       ramp to walk, so "baking" them is just carrying the authored colour through to where a
       read (style_ext) expects to find it. */
    for ( u32 i = 0; i < GUI_EXT_RESERVED_COUNT; ++i )
        s->ext[ i ] = p->ext[ i ];
}

// clang-format on
/*============================================================================================*/
