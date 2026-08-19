/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_render_bake.c -- the palette BAKE TABLE.

    What the palette holds is decided here: one table of the shapes chrome draws over and over, in
    every window, under whatever theme is landed.  gui_render_pal.c makes a finished set of records
    addressable; this file is where the set comes from.

    THE TABLE RECORDS A PATH, NOT A VALUE.  A row below does not state "radius 10.67" -- it states
    "the panel radius, fitted to a titlebar", and the value falls out when the row runs.  That
    matters because the palette has to hold what the widgets will actually emit, and a widget's
    style parameters are derived: a radius is clamped to the rect it lands on, a shadow's feather is
    a style var through the em scale, a stroke is quantized to whole pixels.  A table of numbers
    would be right for one theme at one DPI and quietly wrong everywhere else.

    THE ROWS RUN THE REAL EMITTERS.  Each one calls the same tess_fx_box / tess_fx_segment /
    tess_rect_filled that a widget's command reaches, with the same ambient op word, so the record
    that lands in the table is assembled by the code that assembles the real one.  Nothing here
    restates how a lane is filled, which is the only way the two can be guaranteed to agree -- a
    bake that computed its own lanes would drift the moment an emitter changed, and the symptom
    would be a widget silently drawing the wrong shape.

    A MISS COSTS NOTHING.  The palette is a cache: a record the table failed to predict takes a
    per-slot arena entry exactly as it does today, and a row that predicts a shape nothing draws
    wastes one entry.  Neither is a correctness problem, which is what makes it safe for the table
    to be an educated guess rather than a proof.  Rows are ordered by measured value so the tail is
    what gets dropped if the table ever overruns GUI_PAL_MAX.

    WHAT IS DELIBERATELY ABSENT:
      - Anything whose lanes come from a widget's own arithmetic over its box rather than from the
        style grid -- symbol stroke weights, mark disc radii, arrow vertices.  Those are a continuum
        quantized by floorf, not a vocabulary, and the table cannot name them without duplicating
        each widget's formula here.
      - Colour.  A fill's colour rides the quad and a frame's border colour rides the fx record, so
        no record in this table splits on a colour.
      - Textured entries beyond the atlas the solid-fill convention already resolves to.  An entry
        carrying a relocating bindless slot would need patching on every repack; res_atlas_idx is in
        the digest instead, so a move re-bakes the table.

==============================================================================================*/
// clang-format off

/* A rect wide enough that no radius in the vocabulary is clamped by its WIDTH -- the rows vary the
   height, which is what actually decides whether a corner is a corner or a pill end. */
#define PAL_WIDE   4096.0f

/* The height standing for "tall enough that nothing clamps": a panel, a window body, a menu. */
#define PAL_TALL   4096.0f

/* Line weights the stroke paths quantize onto.  tess_fx_segment floors thickness at 1 px and halves
   it into the capsule radius, so these are the r = 0.5 .. 4.0 family that dominates the census --
   about a third of all arena entries across every theme measured. */
static const f32 k_pal_stroke[] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };

/*==============================================================================================
    Bake state.

    The table is re-derived when its inputs move and not otherwise.  `digest` folds every style var
    plus the atlas slot: a theme switch, a DPI change, a push that outlives a frame and an atlas
    repack all change it, and nothing else can change what a row produces.  Folding the WHOLE var
    block rather than the handful of vars the rows read is what keeps a new row from silently
    inheriting a stale table -- there is no list to forget to update.
==============================================================================================*/

/*  The lookup is a power-of-two open-addressed map from a record's content hash to its entry, sized
    well past GUI_PAL_MAX so a probe walks one or two slots.  Content-addressed on purpose: the
    record is assembled in tess_quad_push out of ambient state, and a widget naming an entry id
    directly would have to re-derive that state and would desync silently -- drawing the wrong
    shape, which is the one failure mode this design has no defence against.  Matching on what the
    record IS cannot be wrong: a hit is a byte-for-byte equal record. */

#define PAL_SLOTS      256u                    /* >> GUI_PAL_MAX; power of two for the mask */
#define PAL_SLOT_MASK  ( PAL_SLOTS - 1u )

static struct
{
    gui_prim_t rec[ GUI_PAL_MAX ];   // rows harvested this bake, before publish
    u32        count;
    u32        digest;               // style inputs the last bake ran against (0 = never baked)
    f32        var[ GUI_VAR_COUNT ]; // the landed style metrics, handed down by pal_style_set

    u16        slot[ PAL_SLOTS ];    // entry + 1 per slot; 0 = empty
    u32        hits, misses;         // this frame's probe outcome, for the dump

} s_bake;

/*  FNV-1a over the whole record -- the same fold the census hashes with, over the same bytes, so a
    baked entry and the census row it covers agree by construction. */

static u32
pal_hash( const gui_prim_t* rec )
{
    const u8* p = (const u8*)rec;
    u32       h = 2166136261u;
    for ( u32 i = 0; i < sizeof( gui_prim_t ); ++i )
        h = ( h ^ p[ i ] ) * 16777619u;
    return h;
}

/*  Which palette entry holds this record, or GUI_PAL_NONE.  The hot path of the whole campaign:
    called once per style-record miss in tess_prim_local, so it walks a couple of slots and does one
    full compare on the candidate.  The compare is not optional -- a hash collision that returned the
    wrong entry would draw the wrong shape. */

u32
pal_find( const gui_prim_t* rec )
{
    if ( s_bake.count == 0u )
        return GUI_PAL_NONE;

    u32 i = pal_hash( rec ) & PAL_SLOT_MASK;
    for ( u32 probe = 0; probe < PAL_SLOTS; ++probe, i = ( i + 1u ) & PAL_SLOT_MASK )
    {
        u16 e = s_bake.slot[ i ];
        if ( e == 0u )
            break;                                     /* empty slot: the record is not in here */
        if ( memcmp( &s_bake.rec[ e - 1u ], rec, sizeof( gui_prim_t ) ) == 0 )
        {
            ++s_bake.hits;
            return (u32)( e - 1u );
        }
    }

    ++s_bake.misses;
    return GUI_PAL_NONE;
}

/*  Rebuild the lookup over the rows just harvested.  Runs once per bake, never per frame. */

static void
pal_index( void )
{
    memset( s_bake.slot, 0, sizeof( s_bake.slot ) );

    for ( u32 e = 0; e < s_bake.count; ++e )
    {
        u32 i = pal_hash( &s_bake.rec[ e ] ) & PAL_SLOT_MASK;
        while ( s_bake.slot[ i ] != 0u )
            i = ( i + 1u ) & PAL_SLOT_MASK;
        s_bake.slot[ i ] = (u16)( e + 1u );
    }
}

void
pal_style_set( const f32* vars, u32 count )
{
    if ( count > (u32)GUI_VAR_COUNT ) count = (u32)GUI_VAR_COUNT;
    memcpy( s_bake.var, vars, count * sizeof( f32 ) );
}

static u32
pal_digest( void )
{
    u32 h = 2166136261u;             /* FNV-1a over the landed style, as raw f32 bits */
    for ( u32 v = 0; v < (u32)GUI_VAR_COUNT; ++v )
    {
        u32 b; memcpy( &b, &s_bake.var[ v ], sizeof( b ) );
        h = ( h ^ b ) * 16777619u;
    }
    h = ( h ^ res_atlas_idx() ) * 16777619u;
    return h ? h : 1u;               /* 0 is the never-baked sentinel */
}

/*==============================================================================================
    Row plumbing.

    A row emits one shape into the tessellator's scratch arena and keeps whatever record that shape
    minted.  pal_row_open mirrors the per-command reset tess_dispatch does (gui_build_tess.c): the
    op word and the record are ambient over ONE shape, and a row that inherited the previous row's
    ops would bake a record no widget will ever ask for.
==============================================================================================*/

static void
pal_row_open( u32 ops )
{
    s_tess.cur_ops        = ops;
    s_tess.cur_corner_pow = 0.0f;
    s_tess.cur_col_border = 0u;
    s_tess.cur_rot_c      = 1.0f;
    s_tess.cur_rot_s      = 0.0f;
    s_tess.cur_phase      = 0.0f;
    s_tess.cur_prim       = ( gui_prim_t ){ 0 };
    s_tess.prim_count     = 0;       /* the emitter appends at 0 and we read it straight back */
    s_tess.quad_count     = 0;
}

/*  Keep the record the row just minted, unless the table already holds it.  The rows are written as
    a cross product and most of it collapses -- a theme with square corners folds every radius row
    onto one entry -- so deduping here is what lets the table be stated as the full product instead
    of as a per-theme special case. */

static void
pal_row_keep( void )
{
    if ( s_tess.prim_count == 0 )
        return;                      /* the emitter declined the shape (degenerate rect) */

    const gui_prim_t* r = &s_tess.prims[ 0 ];

    for ( u32 i = 0; i < s_bake.count; ++i )
        if ( memcmp( &s_bake.rec[ i ], r, sizeof( gui_prim_t ) ) == 0 )
            return;

    if ( s_bake.count < (u32)GUI_PAL_MAX )
        s_bake.rec[ s_bake.count++ ] = *r;
}

/* One surface row: the ambient ops, the rect, and the two lanes an op reads.  Every rounded shape
   in the vocabulary is this call -- a fill, a frame, a band and a shadow differ in the op word and
   in nothing else. */
static void
pal_box( u32 ops, f32 w, f32 h, f32 r, f32 feather, f32 border )
{
    pal_row_open( ops );
    tess_fx_box( 0.0f, 0.0f, w, h, r, feather, border, 0.0f, 0.0f, 0.0f,
                 0, 0, 1, 1, 0, 0xFFFFFFFFu, NULL );
    pal_row_keep();
}

/* The radius a source would land at over a rect this tall -- the widget's own clamp, called rather
   than restated (draw_clamp_round_of, gui_emit_draw.c). */
static f32
pal_round( f32 src, f32 h )
{
    return draw_clamp_round_of( src, PAL_WIDE, h );
}

/*==============================================================================================
    pal_bake -- run the table.

    Called from the placement pass with the tessellation arena idle (gui_build_cache.c, just past
    tess_reset).  Rows write into the head of that arena and the counters are rewound afterwards, so
    the pass that follows sees the arena exactly as tess_reset left it.
==============================================================================================*/

bool
pal_bake( void )
{
    u32 digest = pal_digest();
    if ( digest == s_bake.digest )
        return false;
    s_bake.digest = digest;
    s_bake.count  = 0;

    /* Drop the lookup BEFORE the rows run.  The rows tessellate through tess_prim_local like any
       other shape, so they probe the palette themselves -- and a stale slot still pointing into the
       table being overwritten would answer a row, leaving it with nothing to harvest.  The row
       would then be missing from the very table it belongs in. */
    memset( s_bake.slot, 0, sizeof( s_bake.slot ) );
    s_bake.hits = s_bake.misses = 0;

    /* The style metrics, read once.  Each is already through the em scale -- a var is landed at
       ( boot_font_px / 12 ) * dpi_scale when the theme is applied -- so a row states the var and
       the scale is carried for it. */
    const f32 round_widget = s_bake.var[ GUI_VAR_ROUND       ];  /* control frames, knobs, grabs */
    const f32 round_win    = s_bake.var[ GUI_VAR_PANEL_ROUND ];  /* windows, children, popups    */
    const f32 border       = s_bake.var[ GUI_VAR_BORDER      ];
    const f32 ring         = s_bake.var[ GUI_VAR_FOCUS_RING  ];
    const f32 shadow       = s_bake.var[ GUI_VAR_SHADOW      ];

    /* The heights a radius gets fitted against.  These are the rects chrome actually rounds: a
       row-tall control, an indicator box, a gutter-wide grab, a titlebar, and everything taller
       than twice the radius, where the clamp does not bite at all. */
    const f32 h_fit[] = { PAL_TALL,
                          s_bake.var[ GUI_VAR_ROW       ],
                          s_bake.var[ GUI_VAR_INDICATOR ],
                          s_bake.var[ GUI_VAR_GUTTER    ],
                          s_bake.var[ GUI_VAR_TITLE_H   ] };
    const u32 n_fit   = (u32)( sizeof( h_fit ) / sizeof( h_fit[ 0 ] ) );

    const f32 r_src[] = { 0.0f, round_widget, round_win };
    const u32 n_src   = (u32)( sizeof( r_src ) / sizeof( r_src[ 0 ] ) );

    /*--------------------------------------------------------------------------------------
        1. The two records that carry nothing.  Between them they are about a third of every
        run measured: the flat fill every panel, row and separator lands on, and the plain
        textured quad a glyph falls back to when it cannot take the glyph tag.
    --------------------------------------------------------------------------------------*/

    pal_row_open( 0u );
    tess_rect_filled( 0.0f, 0.0f, PAL_WIDE, PAL_TALL, 0, 0, 1, 1, 0, 0xFFFFFFFFu );
    pal_row_keep();                                       /* -> ops = SELF, every lane zero */

    pal_row_open( 0u );
    tess_rect_filled( 0.0f, 0.0f, PAL_WIDE, PAL_TALL, 0, 0, 1, 1, res_atlas_idx(), 0xFFFFFFFFu );
    pal_row_keep();                                       /* -> no ops at all, every lane zero */

    /*--------------------------------------------------------------------------------------
        2. Strokes.  The single largest family in every census run: rules, separators, grid
        lines, symbol strokes and every polyline segment resolve to a capsule whose only lane
        is half the quantized thickness.  Diagonal on purpose -- an axis-aligned segment is
        stroked as a rect at the emit site and never reaches this path.
    --------------------------------------------------------------------------------------*/

    for ( u32 i = 0; i < (u32)( sizeof( k_pal_stroke ) / sizeof( k_pal_stroke[ 0 ] ) ); ++i )
    {
        pal_row_open( 0u );
        tess_fx_segment( 0.0f, 0.0f, 64.0f, 64.0f, k_pal_stroke[ i ], 0.0f, 0xFFFFFFFFu );
        pal_row_keep();
    }

    /*--------------------------------------------------------------------------------------
        3. Rounded fills -- the widget surface itself.  The full cross product of "which
        radius" against "fitted to what", which is exactly the pair that decides the lane: a
        theme with square corners collapses the whole block onto the square entry already
        baked above, and a rounded theme spreads it across the pill ends its metrics produce.
    --------------------------------------------------------------------------------------*/

    for ( u32 s = 0; s < n_src; ++s )
        for ( u32 f = 0; f < n_fit; ++f )
            pal_box( GUI_OP_SELF, PAL_WIDE, h_fit[ f ],
                     pal_round( r_src[ s ], h_fit[ f ] ), TESS_FX_AA, 0.0f );

    /*--------------------------------------------------------------------------------------
        3b. Rounded on one SIDE only -- a tab in a strip, a pane welded to its neighbour, a
        segmented control's end caps.  Same radius sources; the shape differs in which pair of
        corners takes them, and the fragment picks per quadrant so all four ride the record.
    --------------------------------------------------------------------------------------*/

    for ( u32 s = 1; s < n_src; ++s )        /* from 1: the all-square case is already baked */
        for ( u32 f = 0; f < 2u; ++f )
        {
            f32 r = pal_round( r_src[ s ], h_fit[ f ] );
            if ( r <= 0.0f ) continue;

            pal_row_open( GUI_OP_SELF );     /* top edge -- a tab above its strip */
            tess_round_rect_ex( 0.0f, 0.0f, PAL_WIDE, h_fit[ f ], r, r, 0.0f, 0.0f,
                                TESS_FX_AA, 0xFFFFFFFFu, 0xFFFFFFFFu, 0.0f, 0u, 0.0f );
            pal_row_keep();

            pal_row_open( GUI_OP_SELF );     /* left edge -- a tab in a vertical strip */
            tess_round_rect_ex( 0.0f, 0.0f, PAL_WIDE, h_fit[ f ], r, 0.0f, 0.0f, r,
                                TESS_FX_AA, 0xFFFFFFFFu, 0xFFFFFFFFu, 0.0f, 0u, 0.0f );
            pal_row_keep();

            pal_row_open( GUI_OP_SELF );     /* bottom edge -- a dropdown under its field */
            tess_round_rect_ex( 0.0f, 0.0f, PAL_WIDE, h_fit[ f ], 0.0f, 0.0f, r, r,
                                TESS_FX_AA, 0xFFFFFFFFu, 0xFFFFFFFFu, 0.0f, 0u, 0.0f );
            pal_row_keep();
        }

    /*--------------------------------------------------------------------------------------
        4. Frames -- the same surface with a border band.  The border COLOUR rides the quad,
        so a frame splits on its width and never on its paint.
    --------------------------------------------------------------------------------------*/

    for ( u32 s = 0; s < n_src; ++s )
        for ( u32 f = 0; f < n_fit; ++f )
            pal_box( GUI_OP_SELF | GUI_OP_FRAME, PAL_WIDE, h_fit[ f ],
                     pal_round( r_src[ s ], h_fit[ f ] ), 0.0f, border );

    /*--------------------------------------------------------------------------------------
        5. Bands -- a rounded outline, which is the surface with its interior carved away.
        Two weights: the ordinary border and the keyboard focus ring.
    --------------------------------------------------------------------------------------*/

    const f32 t_band[] = { border, ring };

    for ( u32 t = 0; t < 2u; ++t )
        for ( u32 s = 0; s < n_src; ++s )
            for ( u32 f = 0; f < 2u; ++f )      /* tall and row-tall; the rest are content */
                pal_box( GUI_OP_BAND | GUI_OP_SELF, PAL_WIDE, h_fit[ f ],
                         pal_round( r_src[ s ], h_fit[ f ] ), TESS_FX_AA, t_band[ t ] );

    /*--------------------------------------------------------------------------------------
        6. The elevation shadow under floating chrome: the panel's own shape, cut against
        itself, feathered by the shadow var.  DITHER is derived from the feather width rather
        than asked for, so setting CUT is the whole of what a row states here.
    --------------------------------------------------------------------------------------*/

    /* The cast is directional -- dropped a fixed fraction of its own softness down the screen -- and
       the overlay band spreads wider than the window band.  Both fractions are authored at the
       emit site (WIN_SHADOW_DROP / WIN_SHADOW_OVERLAY_SPREAD, chrome/window/gui_window_free.c) and
       restated here, the one place this file states a number a widget owns.  Bounded: a value that
       drifts out of step costs a palette miss and a per-slot record, never a wrong shape. */
    const f32 k_shadow_spread[] = { 1.0f, 1.5f };
    const f32 k_shadow_drop     = -0.30f;

    if ( shadow > 0.0f )
        for ( u32 b = 0; b < 2u; ++b )
        {
            f32 f = shadow * k_shadow_spread[ b ];
            for ( u32 s = 0; s < n_src; ++s )
            {
                pal_row_open( GUI_OP_CUT | GUI_OP_SELF );
                tess_fx_box( 0.0f, 0.0f, PAL_WIDE, PAL_TALL,
                             pal_round( r_src[ s ], PAL_TALL ), f, 0.0f, 0.0f, 0.0f, 0.0f,
                             0, 0, 1, 1, 0, 0xFFFFFFFFu,
                             &( tess_fx_aux_t ){ .cut_dy = f * k_shadow_drop } );
                pal_row_keep();
            }
        }

    /*--------------------------------------------------------------------------------------
        Hand the table over and leave the arena as we found it.
    --------------------------------------------------------------------------------------*/

    s_tess.prim_count = 0;
    s_tess.quad_count = 0;
    s_tess.cur_ops    = 0u;
    s_tess.cur_prim   = ( gui_prim_t ){ 0 };

    pal_index();
    render_pal_publish( s_bake.rec, s_bake.count );
    return true;
}


/*==============================================================================================
    pal_dump -- the table, in the census's record spelling.

    Printed beside every census dump so the two can be read together: an entry that covers a census
    row is byte-identical to it, so the hashes match and the join is a text compare.  Rows the table
    missed are census rows with no line here; entries nothing draws are lines here with no census
    row.  Both are visible at a glance, which is the whole reason the two share a formatter.
==============================================================================================*/

void
pal_dump( void )
{
#ifdef GUI_PRIM_CENSUS
    u32 probes = s_bake.hits + s_bake.misses;

    gui_log( GUI_LOG_INFO, "" );
    gui_log( GUI_LOG_INFO, "---- PALETTE BAKE (%u of %u entries; %u/%u probes hit since the bake, "
                           "%.1f%%) ----", s_bake.count, (u32)GUI_PAL_MAX, s_bake.hits, probes,
             probes ? 100.0f * (f32)s_bake.hits / (f32)probes : 0.0f );
    gui_log( GUI_LOG_INFO, "     metrics: round %.4g panel_round %.4g border %.4g ring %.4g "
                           "shadow %.4g | row %.4g ind %.4g gutter %.4g title %.4g",
             s_bake.var[ GUI_VAR_ROUND ], s_bake.var[ GUI_VAR_PANEL_ROUND ],
             s_bake.var[ GUI_VAR_BORDER ], s_bake.var[ GUI_VAR_FOCUS_RING ],
             s_bake.var[ GUI_VAR_SHADOW ], s_bake.var[ GUI_VAR_ROW ],
             s_bake.var[ GUI_VAR_INDICATOR ], s_bake.var[ GUI_VAR_GUTTER ],
             s_bake.var[ GUI_VAR_TITLE_H ] );

    for ( u32 i = 0; i < s_bake.count; ++i )
    {
        /* Through the census's own normalization, or the join it exists for cannot happen: the
           census folds the relocating atlas slot out of `tex` before hashing, and a raw entry would
           differ from the row it covers in that one lane and nothing else. */
        gui_prim_t key;
        census_normalize( &key, &s_bake.rec[ i ] );

        char line[ 256 ];
        census_rec_line( line, sizeof( line ), i + 1u, &key );
        gui_log( GUI_LOG_INFO, "%s", line );
    }
    gui_log( GUI_LOG_INFO, "=======================================================================" );
#endif
}

// clang-format on
/*============================================================================================*/
