/*==============================================================================================

    runtime_service/gui/render/pipeline/gui_render_intern.c -- where palette entries come from.

    gui_render_pal.c makes a finished set of records addressable; this file decides what the set
    holds.  There is one way in: a record the frame draws again in a LATER build frame earns an
    entry on the spot (pal_intern).  Nothing is authored, registered or declared -- the qualifying
    evidence is the drawing itself, which is what lets the palette cover a UI layer the engine has
    never seen.  A custom theme's inner-shadowed input box is covered by being drawn twice.

    Three lookups sit in front of the table, cheapest first, and each exists because the one behind
    it structurally cannot serve the case:
      - pal_cmd_hint  -- the answer this command site gave last time, keyed on the command hash the
                         emit phase already folds.  One u32 compare, no record fold.
      - pal_find      -- content-addressed probe over the table, confirmed by a full compare.
      - pal_intern    -- the miss path: qualify the record, then give it an entry.

    A MISS COSTS NOTHING.  The palette is a cache: a record with no entry takes a per-slot arena
    record exactly as it did before any of this existed.  That is what makes every heuristic here
    safe -- the worst outcome of a wrong guess is a wasted entry or an extra arena slot, never a
    wrong shape.

    WHAT NEVER EARNS AN ENTRY:
      - Colour.  A fill's colour rides the quad and a frame's border colour rides the fx record, so
        no record splits on a colour.
      - Anything whose lanes are a continuum rather than a vocabulary -- symbol stroke weights, mark
        disc radii, arrow vertices, an arc whose angle animates.  Those are distinct every frame, so
        they never qualify; that is the whole job of the candidate set below.
==============================================================================================*/
// clang-format off

/*==============================================================================================
    Palette state.

    The table is dropped and rebuilt when its inputs move and not otherwise.  `digest` folds every
    landed style scale plus the atlas slot: a theme switch, a DPI change, a push that outlives a
    frame and an atlas repack all change it.  Without that, entries a dead theme interned would sit
    in the table forever and a handful of theme switches would fill it.
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

/*  How many distinct style SCALES the digest tracks at once.  Mixed-DPI puts more than one in a
    single frame -- each window lands its own surface's scale as it comes up (gui_frame_dpi.c) -- and
    a scale reaches the record as scaled lane values, so the two produce different records and both
    are legitimately live.  Two covers a laptop beside an external monitor, which is the case that
    exists; a third scale evicts the least-recently-seen one, which costs nothing but an extra
    epoch reset while both are in play. */

#define PAL_MAX_SCALES  2u

static struct
{
    /* No record storage here: the table's bytes live in gui_render_pal.c's s_pal.rec, and this
       unit writes entries straight into it (pal_intern).  `count` is the LIVE count -- it runs
       ahead of the published one between a build that interned and that frame's first flush,
       which folds the difference in via pal_publish_pending. */
    u32        count;
    u32        digest;               // style inputs the table was built against (0 = never)

    /* One key per live scale, newest last.  Keyed by CONTENT: re-noting a scale already held is
       recognised and ignored, so the repeated landings a mixed-DPI build performs cost one hash
       each and the set converges on exactly the scales in play. */
    u32        scale_key[ PAL_MAX_SCALES ];   // 0 = slot empty
    u32        scale_count;

    u16        slot[ PAL_SLOTS ];    // entry + 1 per slot; 0 = empty
    u32        hits, misses;         // probe outcome since the last reset, for the dump

    /* Candidate hashes and the build frame each was last seen in.  frame 0 = empty slot,
       which is why the counter starts at 1. */
    struct
    {
        u32 hash;
        u32 frame;

    } cand[ PAL_CAND_SLOTS ];

    u32        cand_count;
    u32        frame;                // build frames since start; bumped by pal_style_reset
    u32        relearn_frame;        // build frame a drop happened in; 0 = no re-learn pending
    u32        cmd_hits;             // of hits, the share a command's parked answer served
    u32        full_drops;           // records that qualified with no room left
    bool       pub_dirty;            // interned entries not folded into the published table

} s_intern;

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
    called once per style-record miss in tess_prim_local -- it folds the record's live rows, walks
    a couple of slots and does one full compare on the candidate.  The compare is not optional --
    a hash collision that returned the wrong entry would draw the wrong shape.

    No memo of its own: the caller's answer memo and the per-command parked answers ahead of it
    absorb the repeats (measured 98%+ of hits), so a memo here served under 1% and cost a
    128-byte compare on every call that reached it. */

/*==============================================================================================
    The A/B lever -- ONE axis, three states (gui_palette_mode_t, gui.h).

    OFF makes pal_find and pal_cmd_hint answer nothing and pal_intern decline, so every style
    falls back to the per-slot memo and the arena -- the exact behaviour that existed before the
    palette, style bloat and all.  That is what makes a suspected palette artefact A/B-able
    against the same frame without it.

    FROZEN keeps the lookup and stops the learning: the table answers with what it already holds
    while anything new takes a per-slot record.  It measures the coverage of the learned set, and
    it is only meaningful once a session has learned something -- from a cold start it is OFF with
    extra steps, which is exactly why this is one control and not two checkboxes.
==============================================================================================*/

static gui_palette_mode_t s_pal_mode = GUI_PALETTE_LEARNING;

gui_palette_mode_t pal_mode( void ) { return s_pal_mode; }

/* Entries the STORED pool holds right now -- the LIVE count, so a frame that just interned reads
   its own additions.  The table's SIZE, cumulative within an epoch: how many of them a given frame
   actually names is a separate number the build accumulates per slot (gui_build_place.c).
   A readout only; nothing resolves an index through it. */
u32 pal_stored_count( void ) { return s_intern.count; }

void
pal_set_mode( gui_palette_mode_t mode )
{
    if ( s_pal_mode == mode )
        return;
    gui_palette_mode_t prev = s_pal_mode;
    s_pal_mode = mode;

    /* Every transition but one needs an epoch.  Away from or back to OFF, because cached geometry
       carries the answers the old mode gave and the switch has to reach the geometry, not just the
       next quad.  FROZEN -> LEARNING, because interning only reaches records that TESSELLATE and a
       settled UI tessellates nothing -- without it the thaw would look like nothing happened.

       LEARNING -> FROZEN is the exception and owes nothing: entries already handed out stay valid
       and keep answering, which is the whole point of freezing.

       Clearing the digest makes the next pal_epoch drop and republish, and the placement pass
       already reads that as "re-place everything" -- the same route a theme change takes. */
    if ( !( prev == GUI_PALETTE_LEARNING && mode == GUI_PALETTE_FROZEN ) )
        s_intern.digest = 0u;
}

u32
pal_find( const gui_prim_t* rec )
{
    if ( s_intern.count == 0u || s_pal_mode == GUI_PALETTE_OFF )
        return GUI_PAL_NONE;

    u32 i = pal_hash( rec ) & PAL_SLOT_MASK;
    for ( u32 probe = 0; probe < PAL_SLOTS; ++probe, i = ( i + 1u ) & PAL_SLOT_MASK )
    {
        u16 e = s_intern.slot[ i ];
        if ( e == 0u )
            break;                                     /* empty slot: the record is not in here */
        if ( memcmp( &s_pal.rec[ e - 1u ], rec, sizeof( gui_prim_t ) ) == 0 )
        {
            ++s_intern.hits;
            return (u32)( e - 1u );
        }
    }

    ++s_intern.misses;
    return GUI_PAL_NONE;
}

/*  Point the lookup at one entry.  The probe walks to the first empty slot: the table is
    append-only within an epoch, so no slot is ever vacated and a walk can never step over the
    entry it is looking for. */

static void
pal_slot_insert( u32 entry, u32 hash )
{
    u32 i = hash & PAL_SLOT_MASK;
    while ( s_intern.slot[ i ] != 0u )
        i = ( i + 1u ) & PAL_SLOT_MASK;
    s_intern.slot[ i ] = (u16)( entry + 1u );
}

/*==============================================================================================
    pal_intern -- give a record the frame keeps drawing an entry of its own.

    The only route into the table.  Called from tess_prim_local on a FULL miss, the coldest
    path it has: the answer memo, the arena memo, the palette and the arena walk have all
    failed, and the alternative is an arena append.  So the hash is folded here rather than
    threaded out of pal_find -- once per never-before-seen record per slot, against a fold that
    is a handful of words.

    APPEND-ONLY, and that is what makes it safe against the retained geometry cache.  A cached
    window's quads carry palette indices from earlier frames; growing the table cannot move
    them, so an intern needs no re-place and no generation bump -- unlike an epoch reset, which
    empties the table and does force one (pal_epoch).  It also means no eviction and no
    freelist: an id handed out is an id for the life of the epoch.  Recycling one would need
    the liveness of every RETAINED slot, which a per-frame counter cannot see (a steady UI
    re-tessellates nothing), and the wrong answer there is a cached window silently drawing
    another style's shape.

    A full table simply stops interning.  The record takes a per-slot arena entry exactly as
    it did before any of this existed -- the same "a miss costs nothing" stated at the top.
==============================================================================================*/

/*  Has this record been seen in an EARLIER build frame?  Records the sighting either way, so
    the first call for a record returns false and arms the second. */

static bool
pal_cand_qualifies( u32 hash )
{
    if ( !hash ) hash = 1u;                      /* 0 would read as an empty slot */

    u32 i = hash & PAL_CAND_MASK;
    for ( u32 probe = 0; probe < PAL_CAND_SLOTS; ++probe, i = ( i + 1u ) & PAL_CAND_MASK )
    {
        if ( s_intern.cand[ i ].frame == 0u )
            break;
        if ( s_intern.cand[ i ].hash == hash )
        {
            if ( s_intern.cand[ i ].frame == s_intern.frame )
                return false;              /* same frame: another window, not another build */
            s_intern.cand[ i ].frame = s_intern.frame;
            return true;
        }
    }

    /* Not held.  Wipe rather than probe forever when the set is full -- what fills it is the
       by-product of animation, and anything real re-qualifies within two frames. */
    if ( s_intern.cand_count >= PAL_CAND_SLOTS / 2u )
    {
        memset( s_intern.cand, 0, sizeof( s_intern.cand ) );
        s_intern.cand_count = 0;
        i = hash & PAL_CAND_MASK;
    }

    while ( s_intern.cand[ i ].frame != 0u )
        i = ( i + 1u ) & PAL_CAND_MASK;

    s_intern.cand[ i ].hash  = hash;
    s_intern.cand[ i ].frame = s_intern.frame;
    ++s_intern.cand_count;
    return false;
}

/*==============================================================================================
    The PER-COMMAND memo -- a style answer parked at the command site that produced it.

    The record a semantic command builds is a function of that command's payload: the ambient
    draw state is folded in at PUSH time, not read at tessellation (gui_emit_state.c says so
    of text_edge, and it holds for the rest -- a retained window re-tessellates long after the
    ambient has moved on).  So if a command's bytes are what they were the last time it
    tessellated, the record it is about to build is the record it built then, and the answer
    can be reused without folding or probing anything.

    The KEY IS ALREADY PAID FOR.  draw_hash_cmd folds every command at emit time for the
    retained cache's diff (s_draw.cmd_hashes), so this costs one u32 compare per command.
    That hash folds strictly MORE than the record depends on -- the clip entry, for one, which
    no record reads -- which is the direction that is safe: it can be over-sensitive and cost a
    probe, it cannot be under-sensitive and hand back the wrong shape.

    ONLY PALETTE ANSWERS ARE PARKED.  An arena index means "record N of the slot being
    tessellated right now", which is meaningless a frame later; a palette index is absolute and
    outlives the frame.  Almost every answer is a palette one, so the restriction costs nothing
    and removes the whole class of stale-arena-index bugs.

    AND IT IS STILL CONFIRMED BY COMPARE.  tess_prim_local memcmps the entry before believing
    it, so an epoch reset that moved what index E holds, a command-list shift that put another
    command at this index, or a hash collision all resolve to a failed compare and the ordinary
    path -- never to a wrong shape.  That is why there is no epoch counter: the compare IS one.

    Indexed by the GLOBAL command index, which shifts when an earlier window emits a different
    number of commands.  A shift invalidates the tail for exactly one frame and re-converges on
    the next -- a self-healing hint, never an assumption.
==============================================================================================*/

/* entry + 1, so a zeroed table reads as "nothing parked" without an init pass. */
static u16 s_cmd_entry[ GUI_MAX_CMDS ];
static u32 s_cmd_hash [ GUI_MAX_CMDS ];

/*  What this command answered last time, if its bytes have not moved since. */

u32
pal_cmd_hint( u32 ci )
{
    if ( ci >= (u32)GUI_MAX_CMDS || s_pal_mode == GUI_PALETTE_OFF )
        return GUI_PAL_NONE;
    if ( s_cmd_entry[ ci ] == 0u || s_cmd_hash[ ci ] != s_draw.cmd_hashes[ ci ] )
        return GUI_PAL_NONE;

    u32 e = (u32)s_cmd_entry[ ci ] - 1u;
    return e < s_intern.count ? e : GUI_PAL_NONE;
}

/*  Park the answer this command just finished with.  A style that resolved into the arena
    parks nothing -- see the header above. */

void
pal_cmd_learn( u32 ci, u32 style )
{
    if ( ci >= (u32)GUI_MAX_CMDS )
        return;

    if ( !gui_style_is_pal( style ) )
    {
        s_cmd_entry[ ci ] = 0u;
        return;
    }
    s_cmd_entry[ ci ] = (u16)( ( style - GUI_PAL_FIRST ) + 1u );
    s_cmd_hash [ ci ] = s_draw.cmd_hashes[ ci ];
}

/*  One entry's bytes, for the compare that confirms a hint.  NULL past the published table.
    */

const gui_prim_t*
pal_entry( u32 entry )
{
    return entry < s_intern.count ? &s_pal.rec[ entry ] : NULL;
}

void
pal_cmd_hit( void )
{
    ++s_intern.hits;
    ++s_intern.cmd_hits;
}

u32
pal_intern( const gui_prim_t* rec )
{
    if ( s_pal_mode != GUI_PALETTE_LEARNING )
        return GUI_PAL_NONE;

    u32 hash = pal_hash( rec );
    if ( !pal_cand_qualifies( hash ) )
        return GUI_PAL_NONE;

    if ( s_intern.count >= (u32)GUI_PAL_MAX )
    {
        ++s_intern.full_drops;
        GUI_WARN_ONCE( "style palette full (%u entries) -- styles that qualify from here "
                       "take per-slot records instead; raise GUI_PAL_MAX if this UI's "
                       "working set is genuinely this wide.\n", (u32)GUI_PAL_MAX );
        return GUI_PAL_NONE;
    }

    u32 entry = s_intern.count++;
    s_pal.rec[ entry ] = *rec;   /* straight into the one table (gui_render_pal.c) */
    pal_slot_insert( entry, hash );

    s_intern.pub_dirty = true;
    return entry;
}

/*  Fold this frame's interned entries into the published table.  Called from the flush,
    between the build that may have added them and the upload that has to carry them
    (gui_render_submit.c).  The entries' bytes are already in place -- pal_intern writes the
    one table directly -- so publishing is only moving the count forward: entries below the
    old count keep their meaning, and an in-flight frame only ever names an index below the
    count IT was uploaded with. */

void
pal_publish_pending( void )
{
    if ( !s_intern.pub_dirty )
        return;
    s_intern.pub_dirty = false;
    render_pal_publish( s_intern.count );
}

/*  Forget every scale.  The build re-notes each one as its windows land, so this runs at the top of
    a frame that will emit and NOT on an idle-skipped one -- a clean frame lands nothing, and a set
    that emptied itself there would trip the epoch against the primary alone and discard the
    geometry the skip exists to keep. */

void
pal_style_reset( void )
{
    s_intern.scale_count = 0;

    /* The BUILD FRAME counter, and this is the one call that ticks once per frame that will
       tessellate -- which is exactly the unit pal_cand_qualifies measures in.  An
       idle-skipped frame builds nothing, so it must not count: a record would otherwise
       qualify against a frame in which nothing was drawn at all. */
    ++s_intern.frame;
}

/*  Note a landed style as one of the scales in play.  Called once at the top of a building frame
    for the primary surface and again from each mixed-DPI landing; the content key makes the repeats
    free.  Only the key is kept -- nothing here reads a var, it only has to notice when one moves. */

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

    for ( u32 s = 0; s < s_intern.scale_count; ++s )
        if ( s_intern.scale_key[ s ] == key )
            return;                  /* already noted -- every landing after the first */

    /* A third scale evicts the oldest.  Costs one extra epoch reset while three are genuinely in
       play, and nothing at all otherwise. */
    u32 at = s_intern.scale_count;
    if ( at >= PAL_MAX_SCALES )
    {
        memmove( &s_intern.scale_key[ 0 ], &s_intern.scale_key[ 1 ],
                 ( PAL_MAX_SCALES - 1u ) * sizeof( s_intern.scale_key[ 0 ] ) );
        at = PAL_MAX_SCALES - 1u;
    }
    else
    {
        s_intern.scale_count++;
    }

    s_intern.scale_key[ at ] = key;
}

/*  What the live table was built against: every noted scale, in order, plus the bindless slot of
    each atlas.  The atlases are in here because an interned record carries its slot in the `tex`
    lane, and a repack or a lazy first creation relocates it -- dropping the table is cheaper than
    patching entries.

    ALL THREE, not just the coverage one.  Records reach `tex` from res_sdf_idx (styled glyphs,
    SDF icons and shapes) and res_sprite_idx (the RGBA path) as well, and both of those atlases
    are created lazily, so each moves at least once in a run that uses it.  A slot left out here
    does not draw wrong -- the table is content-addressed, so a moved slot simply stops matching --
    but the entries holding the old one are never reclaimed and sit against GUI_PAL_MAX. */

static u32
pal_digest( void )
{
    u32 h = 2166136261u;
    for ( u32 s = 0; s < s_intern.scale_count; ++s )
        h = ( h ^ s_intern.scale_key[ s ] ) * 16777619u;
    h = ( h ^ s_intern.scale_count ) * 16777619u;
    h = ( h ^ res_atlas_idx()     ) * 16777619u;
    h = ( h ^ res_sdf_idx()       ) * 16777619u;
    h = ( h ^ res_sprite_idx()    ) * 16777619u;
    return h ? h : 1u;               /* 0 is the never-built sentinel */
}

/*==============================================================================================
    pal_epoch -- empty the table when the style it was learned against has moved.

    Every entry is a record some widget drew under one theme at one DPI against one atlas layout.
    Change any of those and the whole table is dead weight: nothing will draw those records again,
    and left in place they would accumulate until a few theme switches filled GUI_PAL_MAX.  So the
    table is dropped whole and the frame re-learns, which takes two build frames.

    Returns true only when it DROPPED, which the caller reads as "every palette index in cached
    geometry is now stale" and answers with a full re-place (gui_build_place.c).  That is what
    separates this from an intern: this invalidates indices, while pal_intern only ever appends and
    owes nothing.  Self-gating -- a frame whose inputs have not moved costs one fold.

    A DROP COSTS TWO RE-PLACES, NOT ONE, and the second is what refills the table.  An entry is
    earned by a record seen in two DIFFERENT build frames (pal_cand_qualifies), and a drop clears
    the candidate set along with the table, so on the drop frame every record is a first sighting
    and nothing interns.  Left there, the frame after would reuse every unchanged window from the
    retained cache (gui_build_place.c), tessellate nothing, and the table would stay empty until
    each window happened to change on its own -- a theme switch on a settled UI would silently
    give up the palette for good.  So a drop arms relearn_frame and the next BUILD frame re-places
    once more; that frame supplies the second sighting and the table refills whole.

    Keyed on the build-frame counter rather than a plain flag because a single build can run the
    placement pass twice (cache_build_frame's repack retry), and the retry must not consume the
    arming its own drop just did.
==============================================================================================*/

bool
pal_epoch( void )
{
    u32 digest = pal_digest();
    if ( digest == s_intern.digest || s_intern.scale_count == 0u )
    {
        /* The second half of a drop: re-place once more so the drop frame's sightings meet their
           second build frame.  Nothing is invalidated here -- the table and its candidates are
           whatever the drop frame left -- so this only costs the re-place. */
        if ( s_intern.relearn_frame != 0u && s_intern.relearn_frame != s_intern.frame )
        {
            s_intern.relearn_frame = 0u;
            return true;
        }
        return false;
    }
    s_intern.digest = digest;
    s_intern.count  = 0;

    /* Armed with the frame the drop lands in; pal_style_reset has already ticked the counter for
       this frame, so it is never 0 and 0 stays free as the "nothing pending" sentinel. */
    s_intern.relearn_frame = s_intern.frame;

    memset( s_intern.slot, 0, sizeof( s_intern.slot ) );
    s_intern.hits = s_intern.misses = s_intern.cmd_hits = 0;
    s_intern.full_drops = 0;

    /* Every parked answer names an entry of the table just dropped.  A stale one would fail its
       compare and cost nothing, but clearing keeps the reasoning to one sentence. */
    memset( s_cmd_entry, 0, sizeof( s_cmd_entry ) );

    /* The candidates go with the table: they are hashes of records built at the OLD style, and a
       theme or DPI change means nothing that qualified against them will be drawn again. */
    memset( s_intern.cand, 0, sizeof( s_intern.cand ) );
    s_intern.cand_count = 0;

    render_pal_publish( 0 );
    s_intern.pub_dirty = false;
    return true;
}


/*==============================================================================================
    pal_dump -- the table, in the census's record spelling.

    Printed beside every census dump so the two can be read together: an entry is byte-identical to
    the census row it covers, so the hashes match and the join is a text compare.  A census row with
    no line here is a style the palette never took; a line here with no census row is an entry
    nothing drew this run.  Both are visible at a glance, which is why the two share a formatter.
==============================================================================================*/

void
pal_dump( void )
{
#ifdef GUI_PRIM_CENSUS
    u32 probes = s_intern.hits + s_intern.misses;

    /* The MODE is named rather than left to be inferred: an empty table reads the same whether
       the palette is off, frozen before it learned anything, or simply new. */
    static const char* const k_mode[] = { "OFF", "FROZEN", "learning" };

    gui_log( GUI_LOG_INFO, "" );
    gui_log( GUI_LOG_INFO, "---- STYLE PALETTE [%s] (%u of %u entries over %u style scale(s); %u/%u "
                           "probes hit since the epoch, %.1f%%) ----",
             k_mode[ (u32)s_pal_mode % 3u ],
             s_intern.count, (u32)GUI_PAL_MAX, s_intern.scale_count, s_intern.hits, probes,
             probes ? 100.0f * (f32)s_intern.hits / (f32)probes : 0.0f );

    /* What is still in flight: candidates are records seen once and waiting on a second build
       frame to earn an entry, so a large held count beside a small table is a UI drawing values
       rather than styles -- animation, drags, anything whose lanes move every frame. */
    gui_log( GUI_LOG_INFO, "     %u candidate hashes held%s", s_intern.cand_count,
             s_intern.full_drops ? "  TABLE FULL" : "" );

    /* The per-command memo's share of the hits.  What it answers is how much of the lookup
       the command sites absorbed before anything had to be folded or probed at all. */
    gui_log( GUI_LOG_INFO, "     %u of those hits came from a parked command answer, %.1f%%",
             s_intern.cmd_hits,
             s_intern.hits ? 100.0f * (f32)s_intern.cmd_hits / (f32)s_intern.hits : 0.0f );

    for ( u32 i = 0; i < s_intern.count; ++i )
    {
        /* Through the census's own normalization, or the join it exists for cannot happen: the
           census folds the relocating atlas slot out of `tex` before hashing, and a raw entry would
           differ from the row it covers in that one lane and nothing else. */
        gui_prim_t key;
        census_normalize( &key, &s_pal.rec[ i ] );

        char line[ 256 ];
        census_rec_line( line, sizeof( line ), i + 1u, &key );
        gui_log( GUI_LOG_INFO, "%s", line );
    }
    gui_log( GUI_LOG_INFO, "=======================================================================" );
#endif
}

// clang-format on
/*============================================================================================*/
