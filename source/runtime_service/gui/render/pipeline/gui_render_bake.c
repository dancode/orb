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

#define PAL_SLOTS      2048u                   /* >> GUI_PAL_MAX; power of two for the mask */
#define PAL_SLOT_MASK  ( PAL_SLOTS - 1u )

/*  The CANDIDATE set: hashes of records that missed the table once and have not earned an
    entry yet.  Interning on FIRST sight would be wrong -- a record whose lanes move every
    frame (an arc whose start angle animates, a gradient tracking a drag) is distinct every
    time it is built, and a table that never evicts would fill with single-use entries in
    seconds and then have no room for the styles that do repeat.

    The qualifying test is therefore "seen again in a LATER BUILD FRAME", not "seen twice".
    A repeat inside one frame proves only that several windows drew it at one instant, which
    two spinners at the same angle satisfy; a repeat across frames is what distinguishes a
    style from a value, and a style tessellated repeatedly is exactly the one an entry pays
    for.

    Hashes only, no records: a collision here interns something one frame early, which costs
    nothing -- the table itself is confirmed by full compare, so this set cannot cause a
    wrong answer, only a wrong moment.  It fills with the by-products of animation and is
    wiped whole when it does; anything mid-qualification simply re-qualifies. */

#define PAL_CAND_SLOTS  4096u
#define PAL_CAND_MASK   ( PAL_CAND_SLOTS - 1u )

/*  How many distinct style SCALES the table can cover at once.  Mixed-DPI puts more than one in a
    single frame -- each window lands its own surface's scale as it comes up (gui_frame_dpi.c) -- and
    a scale reaches the record as scaled lane values, so each one needs its own rows.  Two covers a
    laptop beside an external monitor, which is the case that exists; a third scale drops the
    least-recently-seen one and its shapes fall back to per-slot records. */

#define PAL_MAX_SCALES  2u

static struct
{
    gui_prim_t rec[ GUI_PAL_MAX ];   // rows harvested this bake, before publish
    u32        count;
    u32        digest;               // style inputs the last bake ran against (0 = never baked)

    /* One landed style per live scale, newest first.  Keyed by CONTENT: re-noting a scale already
       held refreshes it in place, so the repeated landings a mixed-DPI build performs cost one hash
       each and the set converges on exactly the scales in play. */
    struct
    {
        f32 var[ GUI_VAR_COUNT ];
        u32 key;                     // hash of var[]; 0 = slot empty

    } scale[ PAL_MAX_SCALES ];

    u32        scale_count;
    u16        slot[ PAL_SLOTS ];    // entry + 1 per slot; 0 = empty
    u32        memo;                 // last entry pal_find answered with (~0u = none)
    u32        hits, misses;         // probe outcome since the last bake, for the dump
    u32        memo_hits;            // of hits, the share the memo served without hashing

    /* Entries the bake rows produced, so the dump can say which half of the table is the
       authored chrome vocabulary and which the frame interned for itself.  Everything below
       `baked` came from pal_rows; everything above it from pal_intern. */
    u32        baked;

    /* Candidate hashes and the build frame each was last seen in.  frame 0 = empty slot,
       which is why the counter starts at 1. */
    struct
    {
        u32 hash;
        u32 frame;

    } cand[ PAL_CAND_SLOTS ];

    u32        cand_count;
    u32        frame;                // build frames since start; bumped by pal_style_reset
    u32        interned;             // entries pal_intern added since the last bake
    u32        full_drops;           // records that qualified with no room left
    bool       baking;               // pal_rows is running: its own records must not intern
    bool       pub_dirty;            // interned entries not folded into the published table

} s_bake;

/*  FNV-1a over the record's LIVE ROWS -- the probe key for the table below, and the hottest
    hash in the backend: it runs once per style-record memo miss in tess_prim_local.

    A record leaves every lane its field and ops do not read at zero (gui_prim.h), the same
    invariant the dedup memo already depends on, so an all-zero 16-byte row carries no
    identity.  Folding one anyway costs four links of a serial multiply chain and
    distinguishes nothing: a flat fill is one live row out of eight, and the full fold spent
    32 links to say what four say.  The row mask is folded first, so two records whose live
    rows differ still separate before their lanes are compared.

    The hash is a PROBE ACCELERATOR, never an answer.  pal_find confirms every candidate with
    a full 128-byte compare, so a weaker key can only lengthen a probe chain -- it can never
    resolve to the wrong entry, and therefore never draw the wrong shape.  That is what makes
    narrowing it safe.

    NOT the fold the census hashes with (render/gui_prim_census.c).  The census prints an
    identity over the whole record and pal_dump prints the table through that same formatter,
    so the two dumps still join on equal hashes; this one never leaves the probe.

    Read through a byte pointer and reassembled, like fnv1a itself (gui_emit_state.c): a u32
    view over a struct of mixed u32/f32 lanes is an aliasing violation the POSIX toolchains do
    optimize against, and a 4-byte memcpy per lane is a CRT call under /Od. */

static u32
pal_hash( const gui_prim_t* rec )
{
    const u8* b = (const u8*)rec;
    u32       w[ GUI_PRIM_ROWS * 4u ];
    u32       mask = 0u;

    for ( u32 r = 0; r < GUI_PRIM_ROWS; ++r )
    {
        u32 any = 0u;
        for ( u32 c = 0; c < 4u; ++c, b += 4 )
        {
            u32 v = (u32)b[ 0 ] | ( (u32)b[ 1 ] << 8 )
                  | ( (u32)b[ 2 ] << 16 ) | ( (u32)b[ 3 ] << 24 );
            w[ r * 4u + c ] = v;
            any            |= v;
        }
        if ( any )
            mask |= 1u << r;
    }

    u32 h = fnv1a_u32( 2166136261u, mask );
    for ( u32 r = 0; r < GUI_PRIM_ROWS; ++r )
    {
        if ( !( mask & ( 1u << r ) ) )
            continue;
        for ( u32 c = 0; c < 4u; ++c )
            h = fnv1a_u32( h, w[ r * 4u + c ] );
    }
    return h;
}

/*  Which palette entry holds this record, or GUI_PAL_NONE.  The hot path of the whole campaign:
    called once per style-record miss in tess_prim_local, so it tries a one-deep memo, then walks a
    couple of slots and does one full compare on the candidate.  The compare is not optional -- a
    hash collision that returned the wrong entry would draw the wrong shape.

    Every path but the memo folds the record's live rows, which is a serial chain and still
    the dearest thing on the per-quad path; the memo exists to keep a repeat off it. */

/*  The A/B switch.  Off, pal_find answers nothing and every style falls back to the per-slot memo
    and the arena -- the exact behaviour that existed before the palette, style bloat and all.  It
    is a lookup switch, NOT a teardown: the table stays baked and uploaded, so flipping it costs one
    re-tessellation in each direction and nothing else.  That is what makes it usable for A/B'ing a
    suspected palette artefact against the same frame without it. */

static bool s_pal_on = true;

bool pal_enabled( void ) { return s_pal_on; }

void
pal_set_enabled( bool on )
{
    if ( s_pal_on == on )
        return;
    s_pal_on = on;

    /* Cached geometry carries the answers the old setting gave, so the switch has to reach the
       geometry, not just the next quad.  Clearing the digest makes the next pal_bake republish,
       and the placement pass already reads a republish as "re-place everything" -- the same route
       a theme change takes (gui_build_cache.c). */
    s_bake.digest = 0u;
}

u32
pal_find( const gui_prim_t* rec )
{
    if ( s_bake.count == 0u || !s_pal_on )
        return GUI_PAL_NONE;

    /* One-deep memo over the last ANSWERED entry.  A plain repeat never reaches it --
       tess_prim_local memoizes its own last answer whatever produced it -- so what is left
       for this one is the ALTERNATION: a palette style and an arena style traded turn by
       turn, where the caller's memo holds the arena record and every return to the palette
       one would otherwise re-fold and re-probe.
       The compare is against the ENTRY's own bytes -- no copy to keep in step, because the hit
       below is confirmed by this same full compare, so the entry IS what the caller asked for.
       Counted as a hit: the question the rate answers is what share of styles the palette served,
       and this served one.  Held by index rather than by pointer so pal_index / the harvest cannot
       leave it dangling; cleared by pal_bake alongside the lookup, and for the same reason -- a
       bake's own rows probe the palette as they tessellate, and one answered from the old table
       appends nothing, so it would be missing from the table it is being harvested into. */
    if ( s_bake.memo < s_bake.count
      && memcmp( &s_bake.rec[ s_bake.memo ], rec, sizeof( gui_prim_t ) ) == 0 )
    {
        ++s_bake.hits;
        ++s_bake.memo_hits;
        return s_bake.memo;
    }

    u32 i = pal_hash( rec ) & PAL_SLOT_MASK;
    for ( u32 probe = 0; probe < PAL_SLOTS; ++probe, i = ( i + 1u ) & PAL_SLOT_MASK )
    {
        u16 e = s_bake.slot[ i ];
        if ( e == 0u )
            break;                                     /* empty slot: the record is not in here */
        if ( memcmp( &s_bake.rec[ e - 1u ], rec, sizeof( gui_prim_t ) ) == 0 )
        {
            ++s_bake.hits;
            return s_bake.memo = (u32)( e - 1u );
        }
    }

    ++s_bake.misses;
    return GUI_PAL_NONE;
}

/*  Point the lookup at one entry.  The probe walks to the first empty slot: the table is
    append-only within a bake, so no slot is ever vacated and a walk can never step over the
    entry it is looking for. */

static void
pal_slot_insert( u32 entry, u32 hash )
{
    u32 i = hash & PAL_SLOT_MASK;
    while ( s_bake.slot[ i ] != 0u )
        i = ( i + 1u ) & PAL_SLOT_MASK;
    s_bake.slot[ i ] = (u16)( entry + 1u );
}

/*  Rebuild the lookup over the rows just harvested.  Runs once per bake, never per frame. */

static void
pal_index( void )
{
    memset( s_bake.slot, 0, sizeof( s_bake.slot ) );

    for ( u32 e = 0; e < s_bake.count; ++e )
        pal_slot_insert( e, pal_hash( &s_bake.rec[ e ] ) );
}

/*==============================================================================================
    pal_intern -- give a record the frame keeps drawing an entry of its own.

    The bake table states what CHROME draws.  This is the other half, and the half that
    reaches a UI layer the engine has never seen: a record nothing predicted, seen again in a
    later frame, takes an entry on the spot.  A custom theme's inner-shadowed input box is
    covered by being drawn twice.  Nothing is registered, named, or declared -- the qualifying
    evidence is the drawing itself.

    Called from tess_prim_local on a FULL miss, the coldest path it has: the answer memo, the
    arena memo, the palette and the arena walk have all failed, and the alternative is an
    arena append.  So the hash is folded here rather than threaded out of pal_find -- once per
    never-before-seen record per slot, against a fold that is now a handful of words.

    APPEND-ONLY, and that is what makes it safe against the retained geometry cache.  A cached
    window's quads carry palette indices from earlier frames; growing the table cannot move
    them, so an intern needs no re-place and no generation bump -- unlike a re-bake, which
    re-derives every entry and does force one (pal_bake).  It also means no eviction and no
    freelist: an id handed out is an id for the life of the epoch.  Recycling one would need
    the liveness of every RETAINED slot, which a per-frame counter cannot see (a steady UI
    re-tessellates nothing), and the wrong answer there is a cached window silently drawing
    another style's shape.

    A full table simply stops interning.  The record takes a per-slot arena entry exactly as
    it did before any of this existed -- the same "a miss costs nothing" the bake relies on.
==============================================================================================*/

/* Interning A/B switch, separate from pal_enabled: off, the palette holds the bake table
   alone -- the Stage 6 behaviour -- so a run can attribute coverage to the authored rows or
   to what the frame taught itself.  Like pal_enabled it is a lookup-time switch and not a
   teardown, but unlike it there is nothing to invalidate: entries already interned stay valid
   and keep answering. */

static bool s_pal_intern_on = true;

bool pal_intern_enabled( void ) { return s_pal_intern_on; }

void
pal_set_intern( bool on )
{
    s_pal_intern_on = on;
}

/*  Has this record been seen in an EARLIER build frame?  Records the sighting either way, so
    the first call for a record returns false and arms the second. */

static bool
pal_cand_qualifies( u32 hash )
{
    if ( !hash ) hash = 1u;                      /* 0 would read as an empty slot */

    u32 i = hash & PAL_CAND_MASK;
    for ( u32 probe = 0; probe < PAL_CAND_SLOTS; ++probe, i = ( i + 1u ) & PAL_CAND_MASK )
    {
        if ( s_bake.cand[ i ].frame == 0u )
            break;
        if ( s_bake.cand[ i ].hash == hash )
        {
            if ( s_bake.cand[ i ].frame == s_bake.frame )
                return false;              /* same frame: another window, not another build */
            s_bake.cand[ i ].frame = s_bake.frame;
            return true;
        }
    }

    /* Not held.  Wipe rather than probe forever when the set is full -- what fills it is the
       by-product of animation, and anything real re-qualifies within two frames. */
    if ( s_bake.cand_count >= PAL_CAND_SLOTS / 2u )
    {
        memset( s_bake.cand, 0, sizeof( s_bake.cand ) );
        s_bake.cand_count = 0;
        i = hash & PAL_CAND_MASK;
    }

    while ( s_bake.cand[ i ].frame != 0u )
        i = ( i + 1u ) & PAL_CAND_MASK;

    s_bake.cand[ i ].hash  = hash;
    s_bake.cand[ i ].frame = s_bake.frame;
    ++s_bake.cand_count;
    return false;
}

u32
pal_intern( const gui_prim_t* rec )
{
    if ( !s_pal_on || !s_pal_intern_on || s_bake.baking )
        return GUI_PAL_NONE;

    u32 hash = pal_hash( rec );
    if ( !pal_cand_qualifies( hash ) )
        return GUI_PAL_NONE;

    if ( s_bake.count >= (u32)GUI_PAL_MAX )
    {
        ++s_bake.full_drops;
        GUI_WARN_ONCE( "style palette full (%u entries) -- styles that qualify from here "
                       "take per-slot records instead; raise GUI_PAL_MAX if this UI's "
                       "working set is genuinely this wide.\n", (u32)GUI_PAL_MAX );
        return GUI_PAL_NONE;
    }

    u32 entry = s_bake.count++;
    s_bake.rec[ entry ] = *rec;
    pal_slot_insert( entry, hash );

    ++s_bake.interned;
    s_bake.pub_dirty = true;
    return entry;
}

/*  Fold this frame's interned entries into the published table.  Called from the flush,
    between the build that may have added them and the upload that has to carry them
    (gui_render_submit.c).  Extends rather than republishes: entries below `count` are
    byte-identical to what is already there, and an in-flight frame only ever names an index
    below the count IT was uploaded with, so the bytes being written are ones no draw can be
    reading. */

void
pal_publish_pending( void )
{
    if ( !s_bake.pub_dirty )
        return;
    s_bake.pub_dirty = false;
    render_pal_extend( s_bake.rec, s_bake.count );
}

/*  Forget every scale.  The build re-notes each one as its windows land, so this runs at the top of
    a frame that will emit and NOT on an idle-skipped one -- a clean frame lands nothing, and a set
    that emptied itself there would re-bake against the primary alone and discard the geometry the
    skip exists to keep. */

void
pal_style_reset( void )
{
    s_bake.scale_count = 0;

    /* The BUILD FRAME counter, and this is the one call that ticks once per frame that will
       tessellate -- which is exactly the unit pal_cand_qualifies measures in.  An
       idle-skipped frame builds nothing, so it must not count: a record would otherwise
       qualify against a frame in which nothing was drawn at all. */
    ++s_bake.frame;
}

/*  Note a landed style as one of the scales the table must cover.  Called once at the top of a
    building frame for the primary surface and again from each mixed-DPI landing; the content key
    makes the repeats free. */

void
pal_style_set( const f32* vars, u32 count )
{
    if ( count > (u32)GUI_VAR_COUNT ) count = (u32)GUI_VAR_COUNT;

    u32 key = 2166136261u;
    for ( u32 v = 0; v < count; ++v )
    {
        u32 b; memcpy( &b, &vars[ v ], sizeof( b ) );
        key = ( key ^ b ) * 16777619u;
    }
    if ( !key ) key = 1u;            /* 0 is the empty-slot sentinel */

    for ( u32 s = 0; s < s_bake.scale_count; ++s )
        if ( s_bake.scale[ s ].key == key )
            return;                  /* already covered -- the common case, every landing after the first */

    /* A third scale evicts the oldest.  Losing one costs coverage, never correctness: its shapes
       miss the palette and take per-slot records exactly as they did before any of this existed. */
    u32 at = s_bake.scale_count;
    if ( at >= PAL_MAX_SCALES )
    {
        memmove( &s_bake.scale[ 0 ], &s_bake.scale[ 1 ],
                 ( PAL_MAX_SCALES - 1u ) * sizeof( s_bake.scale[ 0 ] ) );
        at = PAL_MAX_SCALES - 1u;
    }
    else
    {
        s_bake.scale_count++;
    }

    memset( s_bake.scale[ at ].var, 0, sizeof( s_bake.scale[ at ].var ) );
    memcpy( s_bake.scale[ at ].var, vars, count * sizeof( f32 ) );
    s_bake.scale[ at ].key = key;
}

/*  What the last bake ran against: every noted scale, in order, plus the atlas slot.  Folding the
    WHOLE var block rather than the vars the rows happen to read is what keeps a new row from
    inheriting a stale table -- there is no list to forget to update. */

static u32
pal_digest( void )
{
    u32 h = 2166136261u;
    for ( u32 s = 0; s < s_bake.scale_count; ++s )
        h = ( h ^ s_bake.scale[ s ].key ) * 16777619u;
    h = ( h ^ s_bake.scale_count ) * 16777619u;
    h = ( h ^ res_atlas_idx()     ) * 16777619u;
    return h ? h : 1u;               /* 0 is the never-baked sentinel */
}

/*==============================================================================================
    Row plumbing.

    A row emits one shape into the tessellator's scratch arena and keeps whatever record that shape
    minted.  pal_row_open mirrors the per-command reset tess_dispatch does (gui_build_tess_dispatch.c): the
    op word and the record are ambient over ONE shape, and a row that inherited the previous row's
    ops would bake a record no widget will ever ask for.
==============================================================================================*/

static void
pal_row_open( u32 ops )
{
    s_tess.cur_ops        = ops;
    s_tess.cur_corner_pow = 0.0f;
    s_tess.cur_fx_field   = 0u;
    s_tess.cur_col_border = 0u;
    s_tess.cur_rot_c      = 1.0f;
    s_tess.cur_rot_s      = 0.0f;
    s_tess.cur_phase      = 0.0f;
    s_tess.cur_swell      = 0.0f;
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
   than restated (draw_clamp_round_of, gui_emit_state.c). */
static f32
pal_round( f32 src, f32 h )
{
    return draw_clamp_round_of( src, PAL_WIDE, h );
}

/*==============================================================================================
    pal_rows -- the TABLE, run once against one landed style.

    Every row states a path and lets the emitter produce the value.  Under mixed DPI this runs once
    per live scale and the two vocabularies coexist in the table: the same row at 1x and at 2x is
    two different records by its lane bytes, and the content-addressed lookup separates them with no
    idea that DPI exists.
==============================================================================================*/

static void
pal_rows( const f32* var )
{
    /* The style metrics, read once.  Each is already through the em scale -- a var is landed at
       ( boot_font_px / 12 ) * dpi_scale when the theme is applied -- so a row states the var and
       the scale is carried for it. */

    const f32 round_widget = var[ GUI_VAR_ROUND       ];  /* control frames, knobs, grabs */
    const f32 round_win    = var[ GUI_VAR_PANEL_ROUND ];  /* windows, children, popups    */
    const f32 border       = var[ GUI_VAR_BORDER     ];
    const f32 ring         = var[ GUI_VAR_FOCUS_RING ];
    const f32 shadow       = var[ GUI_VAR_SHADOW     ];

    /* The heights a radius gets fitted against.  These are the rects chrome actually rounds: a
       row-tall control, an indicator box, a gutter-wide grab, a titlebar, and everything taller
       than twice the radius, where the clamp does not bite at all. */

    const f32 h_fit[] = { PAL_TALL,
                          var[ GUI_VAR_ROW       ],
                          var[ GUI_VAR_INDICATOR ],
                          var[ GUI_VAR_GUTTER    ],
                          var[ GUI_VAR_TITLE_H   ] };

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

    /* from 1: the all-square case is already baked */
    for ( u32 s = 1; s < n_src; ++s )
    {
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
    {
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
    }

    /*--------------------------------------------------------------------------------------
        7. Glows -- the same surfaces lit rather than shadowed, at the one reach every
        attention state is sized against.  Their dropoff is derived from that reach, so the
        vocabulary is as narrow as the shadow's and lands in the same handful of entries.
    --------------------------------------------------------------------------------------*/

    if ( shadow > 0.0f )
        for ( u32 s = 0; s < n_src; ++s )
            pal_box( GUI_OP_GLOW | GUI_OP_SELF, PAL_WIDE, PAL_TALL,
                     pal_round( r_src[ s ], PAL_TALL ), shadow * 2.0f, 0.0f );
}

/*==============================================================================================
    pal_bake -- run the table against every live style scale and publish the result.

    Called from the placement pass with the tessellation arena idle (gui_build_cache.c, just past
    tess_reset).  Rows write into the head of that arena and the counters are rewound afterwards, so
    the pass that follows sees the arena exactly as tess_reset left it.

    Returns true only when it PUBLISHED, which the caller reads as "every baked palette index in
    cached geometry is now stale" and answers with a full re-place.  That is what separates
    a bake from an intern: this REPLACES the table, so entry 12 means a different record
    afterwards, while pal_intern only ever appends and owes nothing.
==============================================================================================*/

bool
pal_bake( void )
{
    u32 digest = pal_digest();
    if ( digest == s_bake.digest || s_bake.scale_count == 0u )
        return false;
    s_bake.digest = digest;
    s_bake.count  = 0;

    /* Drop the lookup BEFORE the rows run.  The rows tessellate through tess_prim_local like any
       other shape, so they probe the palette themselves -- and a stale slot still pointing into the
       table being overwritten would answer a row, leaving it with nothing to harvest.  The row
       would then be missing from the very table it belongs in. */

    memset( s_bake.slot, 0, sizeof( s_bake.slot ) );
    s_bake.memo = ~0u;   /* same reason, and count rising as rows harvest would revive it */
    s_bake.hits = s_bake.misses = s_bake.memo_hits = 0;
    s_bake.interned = s_bake.full_drops = 0;

    /* The candidates go with the table: they are hashes of records built at the OLD style,
       and a theme or DPI change means nothing that qualified against them will be drawn
       again. */
    memset( s_bake.cand, 0, sizeof( s_bake.cand ) );
    s_bake.cand_count = 0;

    /* Interning off for the duration.  A row that interned itself would leave prim_count
       where it was and the harvest below would find nothing to keep -- the same trap the slot
       clear above exists for, arriving by a different door. */
    s_bake.baking = true;

    /* Newest scale first, so if the table ever fills it is the least recently seen surface that
       loses its tail rather than the one the user is looking at. */
    for ( u32 s = s_bake.scale_count; s-- > 0; )
        pal_rows( s_bake.scale[ s ].var );

    s_bake.baking = false;
    s_bake.baked  = s_bake.count;

    /* Leave the arena as we found it. */
    s_tess.prim_count = 0;
    s_tess.quad_count = 0;
    s_tess.cur_ops    = 0u;
    s_tess.cur_prim   = ( gui_prim_t ){ 0 };

    pal_index();
    render_pal_publish( s_bake.rec, s_bake.count );
    s_bake.pub_dirty = false;   /* the replace above carried everything the table holds */
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
    /* memo share is of the HITS, not of all probes: it says how much of what the palette answered
       came back without folding the record, which is the only part of a hit that costs anything. */
    gui_log( GUI_LOG_INFO, "---- PALETTE BAKE (%u of %u entries over %u style scale(s); %u/%u "
                           "probes hit since the bake, %.1f%%; %u of those hits memoed, %.1f%%) ----",
             s_bake.count, (u32)GUI_PAL_MAX, s_bake.scale_count, s_bake.hits, probes,
             probes ? 100.0f * (f32)s_bake.hits / (f32)probes : 0.0f,
             s_bake.memo_hits,
             s_bake.hits ? 100.0f * (f32)s_bake.memo_hits / (f32)s_bake.hits : 0.0f );

    /* How the table filled: authored rows against what the frame taught itself.  A large
       interned share on a stock theme means the bake table is missing shapes chrome actually
       draws; on a custom layer it is the mechanism working as intended. */
    gui_log( GUI_LOG_INFO, "     %u baked + %u interned%s   (%u candidate hashes held, %s)",
             s_bake.baked, s_bake.count - s_bake.baked,
             s_bake.full_drops ? "  TABLE FULL" : "",
             s_bake.cand_count, s_pal_intern_on ? "interning on" : "interning OFF" );

    for ( u32 s = 0; s < s_bake.scale_count; ++s )
    {
        const f32* v = s_bake.scale[ s ].var;
        gui_log( GUI_LOG_INFO, "     scale %u: round %.4g panel_round %.4g border %.4g ring %.4g "
                               "shadow %.4g | row %.4g ind %.4g gutter %.4g title %.4g", s,
                 v[ GUI_VAR_ROUND ], v[ GUI_VAR_PANEL_ROUND ], v[ GUI_VAR_BORDER ],
                 v[ GUI_VAR_FOCUS_RING ], v[ GUI_VAR_SHADOW ], v[ GUI_VAR_ROW ],
                 v[ GUI_VAR_INDICATOR ], v[ GUI_VAR_GUTTER ], v[ GUI_VAR_TITLE_H ] );
    }

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
