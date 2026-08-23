/*==============================================================================================

    runtime_service/gui/render/gui_prim_census.c -- PRIM RECORD CENSUS: what the tessellator
    actually emits, counted across a whole session.

    Prim records dedup only within a window cache slot (tess_prim_local's memo cannot reach past
    slot_prim_base), so every window that draws a rounded button mints its own copy of that
    button's record.  This counts those copies: which distinct records exist, how many quads
    resolve to each, and -- the number that matters -- how many arena entries each one CONSUMED
    across the session.  That last figure is exactly what a frame-global palette entry would
    reclaim, so the dump is both the measurement and the source the palette table is written from.

    Two records that differ only in their atlas bindless slot are the SAME record here: the slot
    relocates on an atlas repack, and counting it would split one entry into two halfway through a
    run.  Only the sampling model (GUI_TEX_MODE) is kept.

    The table never clears and never evicts.  It accumulates until the process exits or
    prim_census_reset is called, because the whole question is cross-window and cross-frame.

    Included by gui_render.c BEFORE gui_build_tess.c (which calls the hooks) -- this file depends
    on nothing but the public gui types.  Compiled out unless GUI_PRIM_CENSUS.

==============================================================================================*/
// clang-format off

#ifdef GUI_PRIM_CENSUS

/* Distinct records tracked, and the open-addressed slot table over them.  512 is well past what a
   session produces (a busy sb_gui frame settles around 25 per window slot, and the whole point is
   that the DISTINCT count is small even when the append count is not); the slot table is 4x that
   so the linear probe stays short.  Index 0 in the slot table means empty, so a slot holds
   `record index + 1`. */

#define CENSUS_MAX     512u
#define CENSUS_SLOTS   2048u

/* "No pass seen yet" -- distinct from every real generation, including 0. */
#define CENSUS_GEN_NONE  0xFFFFFFFFu

typedef struct
{
    gui_prim_t rec;         // the record, with the atlas slot masked out of tex
    u32        quads;       // quads that resolved to it
    u32        appends;     // arena entries it consumed -- what a palette entry would reclaim
    u32        passes;      // distinct tessellation passes it appeared in
    u32        last_gen;    // tess generation of the last sighting, which drives `passes`
                            //   (CENSUS_GEN_NONE until first seen -- 0 is a real generation,
                            //    the one a volatile patch's scratch pass runs under)
    gui_id_t   first_win;   // window it was first seen in, for attribution in the dump

} census_rec_t;

static struct
{
    census_rec_t recs [ CENSUS_MAX   ];
    u16          slots[ CENSUS_SLOTS ];   // record index + 1, 0 = empty

    u32          count;                   // distinct records tracked
    u32          dropped;                 // commits seen after the table filled
    gui_id_t     cur_win;                 // window currently being tessellated
    u32          cur_gen;                 // that pass's tessellation generation

} s_census;

/*==============================================================================================
    Lookup -- content-keyed, with the atlas slot normalized away.
==============================================================================================*/

static void
census_normalize( gui_prim_t* dst, const gui_prim_t* src )
{
    *dst     = *src;
    dst->tex = src->tex & GUI_TEX_MODE_MASK;
}

/* The record's IDENTITY, folded over the whole 128 bytes: the hash column of every dump,
   and what makes two runs joinable.  pal_dump prints the palette table through this same
   formatter, so a palette entry and the census row it covers carry equal hashes.
   Deliberately not pal_hash, which folds only a record's live rows -- that one is a probe
   key on the per-quad path and is free to be weaker; this one is read by a human
   comparing two sessions. */
static u32
census_hash( const gui_prim_t* rec )
{
    return fnv1a( 2166136261u, rec, (u32)sizeof( gui_prim_t ) );
}

/* Resolve a record to its census entry, creating it on first sight.  NULL once the table is full
   -- the caller counts that as a drop rather than folding it into a neighbour, which would make
   the histogram quietly wrong at exactly the tail we are trying to read. */

static census_rec_t*
census_entry( const gui_prim_t* rec )
{
    gui_prim_t key;
    census_normalize( &key, rec );

    u32 i = census_hash( &key ) & ( CENSUS_SLOTS - 1u );
    for ( u32 probe = 0; probe < CENSUS_SLOTS; ++probe, i = ( i + 1u ) & ( CENSUS_SLOTS - 1u ) )
    {
        u16 slot = s_census.slots[ i ];
        if ( slot == 0u )
        {
            if ( s_census.count >= CENSUS_MAX )
                return NULL;

            census_rec_t* r = &s_census.recs[ s_census.count ];
            r->rec          = key;
            r->quads        = 0;
            r->appends      = 0;
            r->passes       = 0;
            r->last_gen     = CENSUS_GEN_NONE;
            r->first_win    = s_census.cur_win;

            s_census.slots[ i ] = (u16)( ++s_census.count );
            return r;
        }

        census_rec_t* r = &s_census.recs[ slot - 1u ];
        if ( memcmp( &r->rec, &key, sizeof( gui_prim_t ) ) == 0 )
            return r;
    }
    return NULL;
}

/*==============================================================================================
    Capture hooks -- called from gui_build_tess.c.
==============================================================================================*/

/* Which window slot the following commits belong to.  The generation identifies the PASS, so a
   record seen twice in one window's tessellation counts one pass, and the same record in fifty
   windows counts fifty -- the cross-slot spread the memo can never collapse. */

void
prim_census_window( gui_id_t win, u32 tess_gen )
{
    s_census.cur_win = win;
    s_census.cur_gen = tess_gen;
}

/* One quad resolved this record, whether the memo caught it or not. */

void
prim_census_quad( const gui_prim_t* rec )
{
    census_rec_t* r = census_entry( rec );
    if ( !r )
    {
        ++s_census.dropped;
        return;
    }
    ++r->quads;
    if ( r->last_gen != s_census.cur_gen )
    {
        r->last_gen = s_census.cur_gen;
        ++r->passes;
    }
}

/* This record cost an arena entry -- the memo missed and tess_prim_local is about to append. */

void
prim_census_append( const gui_prim_t* rec )
{
    census_rec_t* r = census_entry( rec );
    if ( r )
        ++r->appends;
}

void
prim_census_reset( void )
{
    memset( &s_census, 0, sizeof( s_census ) );
}

/*==============================================================================================
    Dump -- the decoded histogram, sorted by the entries each record consumed.
==============================================================================================*/

static const char* const k_census_field[] = {
    "NONE", "BOX", "NGON", "TRI", "(4)", "BEZIER", "SEG", "ARC", "PIE", "(9)", "ARCGRAD",
};

#define CENSUS_FIELD_COUNT  ( sizeof( k_census_field ) / sizeof( k_census_field[ 0 ] ) )

static const struct
{
    u32         bit;
    const char* name;

} k_census_op[] = {
    { GUI_OP_BAND,        "BAND"      },
    { GUI_OP_CUT,         "CUT"       },
    { GUI_OP_INSET,       "INSET"     },
    { GUI_OP_PULSE,       "PULSE"     },
    { GUI_OP_STRIPES,     "STRIPES"   },
    { GUI_OP_SELF,        "SELF"      },
    { GUI_OP_GRAD,        "GRAD"      },
    { GUI_OP_GRAD_RADIAL, "RADIAL"    },
    { GUI_OP_GRAD_CONIC,  "CONIC"     },
    { GUI_OP_SPIN,        "SPIN"      },
    { GUI_OP_DASH,        "DASH"      },
    { GUI_OP_DITHER,      "DITHER"    },
    { GUI_OP_FRAME,       "FRAME"     },
    { GUI_OP_TILE_U,      "TILE_U"    },
    { GUI_OP_TEXT_EDGE,   "TEXT_EDGE" },
    { GUI_OP_CHECKER,     "CHECKER"   },
    { GUI_OP_GRID,        "GRID"      },
    { GUI_OP_GLOW,        "GLOW"      },
    { GUI_OP_REPEAT,      "REPEAT"    },
    { GUI_OP_REPEAT_POLAR,"POLAR"     },
    { GUI_OP_GRAD_ALONG,  "ALONG"     },
    { GUI_OP_GRAD_CELL,   "CELL"      },
    { GUI_OP_CELL_FILL,   "CELL_FILL" },
    { GUI_OP_CUT_SHAPE,   "CUT_SHAPE" },
};

#define CENSUS_OP_COUNT  ( sizeof( k_census_op ) / sizeof( k_census_op[ 0 ] ) )

static const char* const k_census_tex[] = {
    "COVERAGE", "IMAGE", "SDF",
};

#define CENSUS_TEX_COUNT  ( sizeof( k_census_tex ) / sizeof( k_census_tex[ 0 ] ) )

/* The record's lanes, named -- what the PALETTE CANDIDATES section prints.  Driven off offsetof so
   a lane that moves in gui_prim_t moves here with it instead of silently printing its neighbour. */

typedef enum { CEN_U32 = 0, CEN_F32 } census_kind_t;

static const struct
{
    const char*   name;
    census_kind_t kind;
    u32           off;

} k_census_lane[] = {
    { "field",       CEN_U32, (u32)offsetof( gui_prim_t, field       ) },
    { "ops",         CEN_U32, (u32)offsetof( gui_prim_t, ops         ) },
    { "tex",         CEN_U32, (u32)offsetof( gui_prim_t, tex         ) },
    { "glow_k",      CEN_F32, (u32)offsetof( gui_prim_t, glow_k      ) },
    { "r_tl",        CEN_F32, (u32)offsetof( gui_prim_t, r_tl        ) },
    { "r_tr",        CEN_F32, (u32)offsetof( gui_prim_t, r_tr        ) },
    { "r_br",        CEN_F32, (u32)offsetof( gui_prim_t, r_br        ) },
    { "r_bl",        CEN_F32, (u32)offsetof( gui_prim_t, r_bl        ) },
    { "feather",     CEN_F32, (u32)offsetof( gui_prim_t, feather     ) },
    { "border",      CEN_F32, (u32)offsetof( gui_prim_t, border      ) },
    { "corner_pow",  CEN_F32, (u32)offsetof( gui_prim_t, corner_pow  ) },
    { "pat_angle",   CEN_F32, (u32)offsetof( gui_prim_t, pat_angle   ) },
    { "param_a",     CEN_F32, (u32)offsetof( gui_prim_t, param_a     ) },
    { "param_b",     CEN_F32, (u32)offsetof( gui_prim_t, param_b     ) },
    { "param_c",     CEN_F32, (u32)offsetof( gui_prim_t, param_c     ) },
    { "col_b",       CEN_U32, (u32)offsetof( gui_prim_t, col_b       ) },
    { "grad_x",      CEN_F32, (u32)offsetof( gui_prim_t, grad_x      ) },
    { "grad_y",      CEN_F32, (u32)offsetof( gui_prim_t, grad_y      ) },
    { "cut_dx",      CEN_F32, (u32)offsetof( gui_prim_t, cut_dx      ) },
    { "cut_dy",      CEN_F32, (u32)offsetof( gui_prim_t, cut_dy      ) },
    { "anim_rate",   CEN_F32, (u32)offsetof( gui_prim_t, anim_rate   ) },
    { "anim_curve",  CEN_U32, (u32)offsetof( gui_prim_t, anim_curve  ) },
    { "anim_param",  CEN_F32, (u32)offsetof( gui_prim_t, anim_param  ) },
    { "grad_mid",    CEN_F32, (u32)offsetof( gui_prim_t, grad_mid    ) },
    { "dash_period", CEN_F32, (u32)offsetof( gui_prim_t, dash_period ) },
    { "dash_duty",   CEN_F32, (u32)offsetof( gui_prim_t, dash_duty   ) },
    { "dash_scroll", CEN_F32, (u32)offsetof( gui_prim_t, dash_scroll ) },
    { "res_c",       CEN_F32, (u32)offsetof( gui_prim_t, reserved_c  ) },
    { "pat_cell",    CEN_F32, (u32)offsetof( gui_prim_t, pat_cell    ) },
    { "pat_size",    CEN_F32, (u32)offsetof( gui_prim_t, pat_size    ) },
    { "pat_phase",   CEN_U32, (u32)offsetof( gui_prim_t, pat_phase   ) },
    { "pat_col",     CEN_U32, (u32)offsetof( gui_prim_t, pat_col     ) },
};

#define CENSUS_LANE_COUNT  ( sizeof( k_census_lane ) / sizeof( k_census_lane[ 0 ] ) )

/* Append to a bounded cursor.  fmt_snprintf is C99-shaped and returns the WOULD-BE length, so the
   cursor is clamped to the buffer rather than advanced by it -- an unclamped cursor walks past the
   end on the first truncation and every later append writes out of bounds. */

static u32
census_cat( char* buf, u32 cap, u32 w, const char* fmt, ... )
{
    if ( cap == 0u )
        return 0u;
    if ( w + 1u >= cap )
        return cap - 1u;

    va_list ap;
    va_start( ap, fmt );
    int n = fmt_vsnprintf( buf + w, cap - w, fmt, ap );
    va_end( ap );

    w += ( n > 0 ) ? (u32)n : 0u;
    return w < cap ? w : cap - 1u;
}

/*  One record as "<n> <hash> lane=value ..." -- every non-zero lane, named, and nothing else.  The
    shared spelling of a prim record: the census prints its candidates through it and the palette
    prints its table through it (pal_dump, pipeline/gui_render_intern.c), so an entry and the census
    row it covers are the same line of text when they match. */

void
census_rec_line( char* buf, u32 cap, u32 n, const gui_prim_t* rec )
{
    const u8* p = (const u8*)rec;
    u32       w = census_cat( buf, cap, 0, "%3u %08X ", n, census_hash( rec ) );

    for ( u32 l = 0; l < CENSUS_LANE_COUNT; ++l )
    {
        if ( k_census_lane[ l ].kind == CEN_F32 )
        {
            f32 v;
            u32 bits;
            memcpy( &v,    p + k_census_lane[ l ].off, sizeof( v ) );
            memcpy( &bits, p + k_census_lane[ l ].off, sizeof( bits ) );

            /* Tested on the BITS, not against 0.0f: negative zero compares equal to zero and hashes
               as a different record, so a lane holding it has to show up here or two records that
               print identically will carry different hashes with nothing to explain it. */
            if ( bits != 0u )
                w = census_cat( buf, cap, w, "%s=%.9g ", k_census_lane[ l ].name, v );
        }
        else
        {
            u32 v;
            memcpy( &v, p + k_census_lane[ l ].off, sizeof( v ) );
            if ( v != 0u )
                w = census_cat( buf, cap, w, "%s=0x%X ", k_census_lane[ l ].name, v );
        }
    }
}

/* Ops as "FRAME|GRAD", or "-" for a record that carries none. */

static const char*
census_ops_str( u32 ops, char* buf, u32 cap )
{
    u32 w = 0;
    buf[ 0 ] = 0;
    for ( u32 i = 0; i < CENSUS_OP_COUNT; ++i )
    {
        if ( ops & k_census_op[ i ].bit )
            w = census_cat( buf, cap, w, "%s%s", w ? "|" : "", k_census_op[ i ].name );
    }
    if ( !buf[ 0 ] )
        fmt_snprintf( buf, cap, "-" );
    return buf;
}

static const char*
census_field_str( u32 field )
{
    return field < CENSUS_FIELD_COUNT ? k_census_field[ field ] : "?";
}

static const char*
census_tex_str( u32 tex )
{
    u32 mode = tex >> GUI_TEX_MODE_SHIFT;
    return mode < CENSUS_TEX_COUNT ? k_census_tex[ mode ] : "-";
}

/* Window name, or a hex id where the name registry has nothing (Release, or an unnamed window). */

static const char*
census_win_str( gui_id_t id, char* buf, u32 cap )
{
    if ( id == GUI_ID_NONE )
        return "-";
    const char* n = gui_debug_name( id );
    if ( n )
        return n;
    fmt_snprintf( buf, cap, "%08X", id );
    return buf;
}

/*============================================================================================*/
/* Dump the histogram.  Three sections:

     HISTOGRAM          every distinct record, most arena entries first, with a running cumulative
                        share -- read down it to find where a palette of N entries stops paying.
     COLLAPSE           what the distinct count would be with the record's two COLOURS masked out
                        (col_b, pat_col).  A large gap says the tail is one shape in many colours,
                        which a palette cannot dedup but moving those lanes onto the quad would.
     PALETTE CANDIDATES the top entries with every non-zero lane named, so what the palette should
                        be holding is transcribed rather than reverse-engineered. */

#define CENSUS_CANDIDATES  64u

void
prim_census_dump( const char* tag )
{
    if ( !s_census.count )
    {
        gui_log( GUI_LOG_INFO, "prim census [%s]: nothing recorded yet", tag ? tag : "-" );
        return;
    }

    /* Sort by arena entries consumed.  Insertion sort over an index array -- a few hundred
       entries, once, on a key press. */
    static u16 order[ CENSUS_MAX ];
    for ( u32 i = 0; i < s_census.count; ++i )
        order[ i ] = (u16)i;

    for ( u32 i = 1; i < s_census.count; ++i )
    {
        u16 v = order[ i ];
        u32 j = i;
        while ( j > 0 && s_census.recs[ order[ j - 1u ] ].appends < s_census.recs[ v ].appends )
        {
            order[ j ] = order[ j - 1u ];
            --j;
        }
        order[ j ] = v;
    }

    u32 tot_app = 0, tot_quad = 0;
    for ( u32 i = 0; i < s_census.count; ++i )
    {
        tot_app  += s_census.recs[ i ].appends;
        tot_quad += s_census.recs[ i ].quads;
    }

    gui_log( GUI_LOG_INFO, "" );
    gui_log( GUI_LOG_INFO, "==== PRIM RECORD CENSUS [%s] ==========================================",
             tag ? tag : "-" );
    gui_log( GUI_LOG_INFO, "%u distinct records   %u arena entries consumed   %u quads resolved",
             s_census.count, tot_app, tot_quad );
    if ( s_census.dropped )
        gui_log( GUI_LOG_WARN, "%u commits DROPPED -- census table full (CENSUS_MAX %u), "
                               "the tail below is incomplete", s_census.dropped, (u32)CENSUS_MAX );

    /* The HASH column is what makes two runs comparable.  It keys the record's content, so the same
       hash in a "dark" run and a "rounded" run is the same record -- which is the whole differential
       test: a record that appears in both is theme-INDEPENDENT (a literal, or content), and one that
       appears in only one moved because a style var moved it, i.e. it scales with the theme. */
    gui_log( GUI_LOG_INFO, "" );
    gui_log( GUI_LOG_INFO, "  #  hash      entries    cum%%   quads  passes  field    ops           "
                           "             radii                edge             tex       first window" );

    u32 cum = 0;
    for ( u32 i = 0; i < s_census.count; ++i )
    {
        const census_rec_t* r = &s_census.recs[ order[ i ] ];
        cum += r->appends;

        char ops[ 96 ], win[ 24 ], rad[ 40 ], edge[ 40 ];
        fmt_snprintf( rad, sizeof( rad ), "%.1f/%.1f/%.1f/%.1f",
                      r->rec.r_tl, r->rec.r_tr, r->rec.r_br, r->rec.r_bl );
        fmt_snprintf( edge, sizeof( edge ), "f%.2f b%.2f p%.1f",
                      r->rec.feather, r->rec.border, r->rec.corner_pow );

        gui_log( GUI_LOG_INFO,
                 "%3u  %08X  %7u  %5.1f%%  %6u  %6u  %-7s  %-26s  %-19s  %-15s  %-8s  %s",
                 i + 1u, census_hash( &r->rec ), r->appends,
                 tot_app ? 100.0f * (f32)cum / (f32)tot_app : 0.0f,
                 r->quads, r->passes,
                 census_field_str( r->rec.field ),
                 census_ops_str( r->rec.ops, ops, sizeof( ops ) ),
                 rad, edge,
                 census_tex_str( r->rec.tex ),
                 census_win_str( r->first_win, win, sizeof( win ) ) );
    }

    /* Cumulative share at the palette sizes worth considering, so the cut is picked off a number
       rather than off the shape of the list. */
    gui_log( GUI_LOG_INFO, "" );
    static const u32 k_cut[] = { 16u, 32u, 64u, 128u };
    for ( u32 c = 0; c < sizeof( k_cut ) / sizeof( k_cut[ 0 ] ); ++c )
    {
        u32 n = k_cut[ c ] < s_census.count ? k_cut[ c ] : s_census.count;
        u32 s = 0;
        for ( u32 i = 0; i < n; ++i )
            s += s_census.recs[ order[ i ] ].appends;
        gui_log( GUI_LOG_INFO, "  top %3u records cover %5.1f%% of arena entries (%u of %u)",
                 k_cut[ c ], tot_app ? 100.0f * (f32)s / (f32)tot_app : 0.0f, s, tot_app );
    }

    /* COLLAPSE: distinct count with both record colours masked.  O(n^2) over a few hundred
       entries, once -- no second hash table for a one-shot figure. */
    {
        u32 distinct = 0, colour_app = 0;
        for ( u32 i = 0; i < s_census.count; ++i )
        {
            gui_prim_t a = s_census.recs[ i ].rec;
            a.col_b = a.pat_col = 0;

            bool first = true;
            for ( u32 j = 0; j < i; ++j )
            {
                gui_prim_t b = s_census.recs[ j ].rec;
                b.col_b = b.pat_col = 0;
                if ( memcmp( &a, &b, sizeof( gui_prim_t ) ) == 0 ) { first = false; break; }
            }
            if ( first ) ++distinct;
            else         colour_app += s_census.recs[ i ].appends;
        }
        gui_log( GUI_LOG_INFO, "" );
        gui_log( GUI_LOG_INFO, "  colour collapse: %u distinct -> %u with col_b/pat_col masked "
                               "(%u entries, %.1f%%, are one shape in several colours)",
                 s_census.count, distinct, colour_app,
                 tot_app ? 100.0f * (f32)colour_app / (f32)tot_app : 0.0f );
    }

    /* PALETTE CANDIDATES -- every non-zero lane, named.  What the palette should be holding. */
    gui_log( GUI_LOG_INFO, "" );
    gui_log( GUI_LOG_INFO, "---- PALETTE CANDIDATES (non-zero lanes) ------------------------------" );

    u32 cand = s_census.count < CENSUS_CANDIDATES ? s_census.count : CENSUS_CANDIDATES;
    for ( u32 i = 0; i < cand; ++i )
    {
        char line[ 256 ];
        census_rec_line( line, sizeof( line ), i + 1u, &s_census.recs[ order[ i ] ].rec );
        gui_log( GUI_LOG_INFO, "%s", line );
    }
    gui_log( GUI_LOG_INFO, "=======================================================================" );
}

#endif    // GUI_PRIM_CENSUS

/*==============================================================================================
    build_prim_census -- the host entry point behind gui()->debug_prim_census.

    Defined in EVERY build so the vtable slot is unconditional, and so a driver that scripts a
    census run (sb_gui_example's -census mode) needs no build-configuration knowledge: a build
    without the census says so once and does nothing.

    Dump and clear are one call because that is how a multi-run sweep uses them -- dump the run
    just finished, clear for the next -- and because two calls invite the ordering mistake of
    clearing first.  A NULL tag skips the dump, which is the bare-clear case.
==============================================================================================*/

void
build_prim_census( const char* tag, bool clear )
{
#ifdef GUI_PRIM_CENSUS
    if ( tag )
    {
        prim_census_dump( tag );
        pal_dump();          /* beside the run it is meant to cover -- the two are read together */
    }
    if ( clear )
        prim_census_reset();
#else
    UNUSED( tag );
    UNUSED( clear );
    GUI_WARN_ONCE( "prim census requested, but this build has GUI_PRIM_CENSUS compiled out.\n" );
#endif
}

// clang-format on
/*============================================================================================*/
