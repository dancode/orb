/*==============================================================================================

    gui/style/gui_bake.c -- the bake: a seed palette -> the 36-cell colour grid.

    A theme authors a gui_palette_t: seven seed colours plus a ramp (gui.h).  A render reads
    gui_style_t.col: an 8-role x 4-phase grid of resolved colours.  gui_style_bake is the only
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
    The colour operations -- six verbs, and the whole ramp is written in them.

    Alpha is never blended, only carried: every op keeps the alpha of its FIRST argument.  
    That is what lets a translucent seed (a HUD panel at 0xF0) yield a translucent cell in
    all four phases without the author restating the alpha four times -- and it is why these
    are not col_lerp, which lerps alpha like any other channel.
==============================================================================================*/

#define BAKE_WHITE 0x00FFFFFFu   /* rgb only -- alpha comes from the blend's first argument */
#define BAKE_BLACK 0x00000000u

/*==============================================================================================
    This function answers one question: "How bright does this colour appear to a human eye?"
    It takes a packed 32-bit colour (0xBBGGRR) and returns a single 0�255 brightness number.   
    Your eye does not see red, green, and blue as equally bright. The weights reflect that:
    
    Channel     | Weight         | Why
    ------------|----------------|-----------------------------------
    Green       | 150            | Eyes are most sensitive to green
    Red         | 77             | Medium sensitivity
    Blue        | 29             | Least sensitive
    
    Caller      | What it does with the brightness
    ------------|----------------------------------------------------
    Pole        | Decides if a colour plane is "light" or "dark" (polarity)
    MUTE        | Finds the grey a saturated hue should drain toward when muted
    Ink guard   | Checks that text/ink is far enough from the BG in perceived brightness
==============================================================================================*/

static u32
bake_lum( u32 c )
{
    // Step 1 � Unpack the channels
    u32 r = ( c       ) & 0xFFu;
    u32 g = ( c >>  8 ) & 0xFFu;
    u32 b = ( c >> 16 ) & 0xFFu;

    // Step 2 � Weighted sum (Rec. 601 luma)    
    // normalize to 0�255 range /w a divide by 256 (>>8)
    return ( 77u * r + 150u * g + 29u * b ) >> 8;     
}

/*==============================================================================================
    Blends two colours together by a fraction t, like fading from one to the other. 
    It's described as "THE primitive" because every other colour operation in the 
    system is just this function with a different choice of 'b'.

    Tint a colour        - A solid target hue
    Fade to grey         - The grey from bake_lum
    Fade to black/white  - 0x000000 or 0xFFFFFF
    Dim/brighten         - A darker/lighter version

    Param       | Meaning
    ------------|-----------------------------------------------------------
    a           | Starting colour (packed 0xAABBGGRR)
    b           | Target colour to blend toward
    t           | How far to blend � 0.0 = all 'a', 1.0 = all 'b'
==============================================================================================*/

static u32
bake_mix( u32 a, u32 b, f32 t )
{
    // Step 1 � Clamp t
    if ( t <= 0.0f ) return a;      // no blend needed, bail early
    if ( t >= 1.0f ) t = 1.0f;      // cap at full blend (don't overshoot)

    // Step 2 � Preserve a's alpha
    // The top 8 bits (alpha channel) are copied from 'a' immediately and never 
    // touched again. Whatever transparency 'a' had, the result keeps it.

    u32 out = a & 0xFF000000u; 
    
    // Step 3 � Blend R, G, B in a loop
    for ( u32 sh = 0; sh <= 16; sh += 8 )
    {
        // The loop runs three times: sh = 0 (red), sh = 8 (green), sh = 16 (blue).
        // Each loop unpacks one channel from both colours, blends it, and packs it back:
        f32 c0 = (f32)( ( a >> sh ) & 0xFFu );
        f32 c1 = (f32)( ( b >> sh ) & 0xFFu );

        // +0.5f rounding trick keeps the result accurate despite the float -> int cast
        // without it truncation would bias every channel slightly darker.
        out |= ( (u32)( c0 + ( c1 - c0 ) * t + 0.5f ) & 0xFFu ) << sh;
    }
    return out;
}

/*==============================================================================================
    The Pole System — What it does:

    This whole block is about one idea: "forward" means something different depending on 
    whether your background is dark or light. The pole is how the system answers that 
    question once, cleanly, for everything that follows.

    The decision point, it looks at the ground (background) colour, measures its brightness 
    with bake_lum, and picks a single anchor:

        Dark  (luma < 128) | White — "forward" means lighter
        Light (luma ≥ 128) | Black — "forward" means darker

    Why not compare per-colour?

    Deriving the pole once from the ground sidesteps the issue of the color being the ground.
    Every colour in the system uses the same pole, so the answer is always consistent and 
    never needs a per-colour fallback (idle cell is the literal ground color)

         ├── bake_lift    push toward pole         (prominent, elevated)
         ├── bake_recess  push toward black        (shadow, always)
         ├── bake_fade    push toward ground       (secondary, inert)
         ├── bake_wash    push toward accent       (hot, pressed, selected)
         └── bake_mute    push toward own grey     (off, disabled)

==============================================================================================*/

/* POLE -- The light or dark decision point */
static u32 
bake_pole( u32 ground ) 
{ 
    return ( bake_lum( ground ) < 128u ) ? BAKE_WHITE : BAKE_BLACK; 
}

/* LIFT -- bring c forward, toward the pole (makes things feel elevated) */
static u32 
bake_lift( u32 c, f32 t, u32 pole ) 
{ 
    return bake_mix( c, pole, t ); 
}

/* RECESS -- sink c into the page, always blend toward black */
static u32 
bake_recess( u32 c, f32 t ) 
{ 
    return bake_mix( c, BAKE_BLACK, t ); 
}

/* FADE -- retire 'c' toward the ground it sits on: the DIM phase for anything drawn 
   ON a container (secondary text, a subdued frame, an inert mark). */
static u32 
bake_fade( u32 c, f32 t, u32 ground )   
{ 
    return bake_mix( c, ground, t );
}

/* WASH -- Blends 'c' toward the accent colour. Used for hover and pressed states.
   When a button is hovered or held, it washes toward the theme's accent.
   At the SELECT ramp it's used to tint the chosen ground itself. */
static u32 
bake_wash( u32 c, f32 t, u32 accent )    
{ 
    return bake_mix( c, accent, t ); 
}

/* MUTE -- drain the chroma out of c toward its own luma, leaving lightness alone.  
   Only the affirmative hues need it: a green check faded straight toward a grey surface 
   stays a saturated green, which reads as "on but quiet" rather than "inert".  
   Draining first reads as off. */

static u32
bake_mute( u32 c, f32 t )
{
    /* Step 1 — Find the grey equivalent */
    u32 l = bake_lum( c );

    /* Step 2 — Build a grey at that same brightness. Sets R, G, and B all to 'l'. 
       This is the exact grey that has the same luminance as the original colour 
       — same lightness, zero saturation. */
    u32 new_grey = l | ( l << 8 ) | ( l << 16 );

    /* Step 3 — Blend toward that grey. Gradually drains the colour toward neutral. 
       The brightness stays the same throughout; only the hue bleeds away. */
    return bake_mix( c, new_grey, t );

}

/*==============================================================================================
    The ink GUARD -- The legibility guard
   
    This is the only function that can refuse to act. Every other op always transforms 
    its input. This one first asks "is there even a problem?" and returns unchanged if not.

    The problem it solves: 

    A theme author picks an ink colour by looking at it on one specific surface.
    But the bake then places that same ink on multiple derived grounds — hovered, pressed,
    selected, pressed-while-selected, etc.
    
    Nobody looked at those. Some of them may have drifted close enough to the ink colour
    that text becomes unreadable. The guard measures the gap and, only if it's too small,
    pushes the ink further away until it clears the floor.

     Constant           | Value         | Used for
    --------------------|---------------|--------------------------------------------
    BAKE_INK_DELTA      | 110           | Primary ink — must be clearly readable
    BAKE_DIM_DELTA      | 70            | Secondary ink — quieter, but still legible

==============================================================================================*/

#define BAKE_INK_DELTA 110   /* minimum ink-vs-face luma separation, of 255                    */
#define BAKE_DIM_DELTA  70   /* the same floor for SECONDARY ink, which is meant to be quieter */

static u32
bake_ink_on( u32 ink, u32 ground, i32 want )
{
    /* Step 1 — Measure the separation */
    const i32 g = (i32)bake_lum( ground );
    const i32 i = (i32)bake_lum( ink    );

    if ( ( ( i > g ) ? i - g : g - i ) >= want ) return ink;   /* already legible -- leave it */

    /* Step 2 — Decide which direction to push:
       The rule is "stay on the side the ink is already on" — not which side the ground is on. 

    Situation                        | Direction
    ---------------------------------|----------------------------------------------------------------
    Ink is brighter than ground      | Push up toward white
    Ink is darker than ground        | Push down toward black
    Ink and ground are equal         | Use the pole as tiebreak: dark ground → push up */

    const bool up = ( i > g ) || ( i == g && g < 128 );

    /* Step 3 — Solve for the exact blend amount.
       Instead of nudging and checking repeatedly, the function solves for t directly 
       — the exact blend fraction that will land ink right on the legibility floor.
       "How far do I need to travel from ink toward 0 to gain the missing separation?"
    
        (1) Toward white: L(t) = i + (255 - i) * t.     (up = true) 
        (2) Toward black: L(t) = i * (1 - t).           (up = false)

        Out-of-reach targets solve to t > 1 and bake_mix clamps, which lands on the 
        end of the greyscale -- the best available answer. 
    */

    f32 t;
    if ( up ) t = ( i >= 255 ) ? 1.0f : (f32)( g + want - i ) / (f32)( 255 - i );    
    else      t = ( i <=   0 ) ? 1.0f : 1.0f - (f32)( g - want ) / (f32)i;

    /* Step 4 — Apply the minimum necessary push 
       This is surgical: t was solved to land exactly at the legibility floor, no further. 
       A barely-failing ink gets the smallest correction needed. 
       A wildly-failing ink gets pushed hard — possibly all the way to white or black. */

    return bake_mix( ink, up ? BAKE_WHITE : BAKE_BLACK, t );
}

/*==============================================================================================
    The derivation -- nine roles, each a sentence in the verbs above.

    Written out role by role rather than driven from a data table, each block below is an
    editorial claim about what a phase MEANS for that role, and a table of opcodes would bury
    exactly the thing a theme author needs to read. Each claim is stated relative to the GROUND
    and INK bake_plane is handed, never to the raw palette seed -- see each role's comment below
    for what it claims and why.

    The severity ladder (INFO / OK / WARN / ERROR) is not one of the nine roles here: it lives
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
    const f32 step   = p->ramp[ GUI_RAMP_STEP   ];

    const u32 line   = p->seed[ GUI_SEED_LINE   ];
    const u32 accent = p->seed[ GUI_SEED_ACCENT ];
    const u32 mark   = p->seed[ GUI_SEED_MARK   ];
    const u32 grab   = p->seed[ GUI_SEED_GRAB   ];

    const u32 pole = bake_pole( ground );

    /*  The band a titlebar sits: elevate from pole, then wash (moves toward accent color).
        Blends ground 50% of one ramp step toward pole (white on a dark, black on a light)
        This is the "lifted" part of "lifted, faintly accented ground". Makes the
        titlebar feel a part of the theme rather than a clone of the surface panel
        Color is informed by the strength of main hover style 'color change' factor. */

    const u32 band = bake_wash( bake_lift( ground, step * 0.50f, pole ), hover * 0.25f, accent );

    /* PANEL -- the container.  A fifth of the hover and no lift at all: background should tint,
       not move.  Pressed is the full wash, sunk half a step so a held surface reads as pressed
       rather than merely coloured. */

    col[ GUI_ROLE_PANEL ][ GUI_PHASE_IDLE   ] = ground;
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_HOT    ] = ground; // bake_wash( ground, hover * 0.25f, accent );
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_ACTIVE ] = ground; // bake_recess( bake_wash( ground, press, accent ), step * 0.50f );
    col[ GUI_ROLE_PANEL ][ GUI_PHASE_DIM    ] = bake_recess( ground, recess );

    /* TITLE -- a caption band is a LIFTED ground, which is why it needs no seed of its own.
       ACTIVE is the bare ground: a live tab IS its panel, merging into the body it owns. */

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

    /* BORDER -- structural at rest, pure signal when live.  ACTIVE takes a second lift step so
       the focus ring reads brighter than a hovered edge -- the border carries focus, so the two
       cells must stay visibly distinct, the same HOT -> ACTIVE spacing the ACCENT row keeps. */

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

    /* ACCENT -- the value a control holds: a straight three-cell lift (IDLE, +step, +2*step).
       DIM is the EMPTY track, so it comes off the CONTROL face, not the accent -- an empty
       slider well is a well, not a faded fill. */

    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_IDLE   ] = accent;
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_HOT    ] = bake_lift( accent, step,        pole );
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_ACTIVE ] = bake_lift( accent, step * 2.0f, pole );
    col[ GUI_ROLE_ACCENT ][ GUI_PHASE_DIM    ] = bake_recess( control, recess );

    /* MARK -- the indicator a control shows.  IDLE and ACTIVE are the same colour: a check does
       not change colour when pressed.  HOT is the nav ring, which is accent business, not the
       mark's. */
    col[ GUI_ROLE_MARK ][ GUI_PHASE_IDLE   ] = mark;
    col[ GUI_ROLE_MARK ][ GUI_PHASE_HOT    ] = bake_lift( accent, step, pole );
    col[ GUI_ROLE_MARK ][ GUI_PHASE_ACTIVE ] = mark;
    col[ GUI_ROLE_MARK ][ GUI_PHASE_DIM    ] = bake_fade( bake_mute( mark, 0.80f ), fade, ground );

    /* GRAB -- the contrast anchor, authored opposite the theme's polarity and lifted furthest of
       any role, since a knob must stay legible against both a hovering track and a filled bar.
       Its DIM fades further too: the anchor sits as far from the ground as the palette goes, so
       an ordinary fade would still leave an inert knob shouting. */

    col[ GUI_ROLE_GRAB ][ GUI_PHASE_IDLE   ] = grab;
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_HOT    ] = bake_lift( grab, step * 3.0f, pole );
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_ACTIVE ] = bake_lift( grab, step * 6.0f, pole );
    col[ GUI_ROLE_GRAB ][ GUI_PHASE_DIM    ] = bake_fade( grab, fade + ( 1.0f - fade ) * 0.40f,
                                                          ground );
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
