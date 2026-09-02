/*==============================================================================================

    gui/frame/gui_frame_resolve.c -- THE FONT RESOLVER: (family, size) -> id.

    Every managed font in the GUI comes through here: the boot font, the DPI engine's
    per-monitor sizes, the type ramp's SMALL/LARGE roles, and the public font_get.  A font is
    a REQUEST -- a family (the directory under content/font) plus a pixel size -- and this file
    turns requests into loaded registry ids through a layered search:

        (a) resident  -- the memo already holds this (family, size); free.
        (b) cooked    -- the bake "font/<family>/<size>.orb_font" read through the fs mounts
                         (gui_res.h): a size some RID() named, cooked by the build.
        (c) baker     -- the host-installed runtime baker (font_baker_set; dev_font_get is the
                         canonical wiring) bakes the exact size on demand and hands back bytes.
        (d) ladder    -- nearest RESIDENT size in the same family, ties toward the LARGER bake;
                         failing even that, the default font (id 0).

    The mounts are never enumerated -- a pack cannot be listed, and the set of bakes that exist
    is exactly the set of names the code marked -- so layer (d) knows only what is already
    loaded.  In a shipped build without a baker that is the boot size and whatever else was
    RID'd and requested; the DPI retarget degrades to the boot bake, which is the readable
    fallback, and the type ramp stays off for sizes with no bake.

    A resolve never hard-fails and never activates: the active font and the draw stamp are
    restored around every load, so no resolution can read as a host takeover to the DPI
    lineage guard.  Callers that need exact sizes (the type ramp) check the landed size the
    resolver reports; callers that want the nearest answer (the DPI engine) take it as-is.

    The memo is also the ownership ledger for the registry slots the resolver mints.
    Retention is IMMEDIATE-MODE, like every other piece of per-widget state: being requested
    IS the hold.  A public font_get stamps its entry with the current emitted-frame counter;
    an entry requested this frame or the previous emitted frame is LIVE and eviction-exempt.
    Stop requesting it and it goes stale -- still resident (unloading is lazy; a stale font
    costs nothing), but its slot is reclaimable the moment a load needs one, and requesting
    it again reloads.  The DPI engine and type ramp PIN their live ids per frame instead of
    stamping.  Under registry pressure one stale, unpinned owner is released (slot + atlas
    tenant) and the load retried.  A degraded answer (landed != wanted) is recorded as an
    ALIAS of the owning entry's slot -- it releases nothing -- and a request no layer could
    serve latches a failure sentinel so it costs one attempt, not one per frame.  Installing a
    baker drops latches and stale aliases and bumps a generation counter, which tells the DPI
    engine to re-resolve: early degraded answers self-heal once a baker arrives.

    Included by gui_frame.c after gui_frame_font.c (its loads ride the same deferred-upload
    flush) and before gui_frame_dpi.c / gui_frame_type.c, its two internal clients.

==============================================================================================*/
// clang-format off

#define FONT_RESOLVE_MEMO_MAX 24        // > registry 16: latches + aliases hold no slot
#define FONT_RESOLVE_FAILED   0xFFFFu   // memo id: no layer could serve; one attempt per generation

typedef struct
{
    u32 name_hash;   // FNV-1a of the normalized family name (key)
    u16 size_px;     // requested size (key)
    u16 id;          // registry id; 0 = the default slot; FONT_RESOLVE_FAILED = latch
    u16 landed_px;   // size actually resolved; != size_px marks a degraded ALIAS (owns no slot)
    u8  traits;      // reserved, always 0 -- the rich-type (bold/italic span) future keys here
    u8  _unused;
    u32 seen_gen;    // emitted frame a public font_get last requested this; 0 = internal
                     // entry (DPI / ramp / boot).  Live -- eviction-exempt -- while within
                     // one emitted frame of now; stale after that (the immediate-mode hold)
} font_memo_t;

/* Pin slots -- ids the eviction pass may never touch while their owner has them live.
   Slot 0 (the default font) is implicitly pinned; publicly requested memo entries protect
   themselves through their frame stamp (resolve_entry_live). */
enum
{
    FONT_PIN_SMALL = 0,          // type ramp SMALL role
    FONT_PIN_LARGE,              // type ramp LARGE role
    FONT_PIN_VP0,                // + one per viewport: the landed DPI font of that surface
    FONT_PIN_COUNT = FONT_PIN_VP0 + GUI_MAX_VIEWPORTS
};

static struct
{
    gui_font_bake_fn  baker;                            // host-installed runtime baker; NULL = layer (c) off
    void*             baker_user;

    font_memo_t       memo[ FONT_RESOLVE_MEMO_MAX ];    // the request -> id ledger
    u32               memo_count;
    u32               pin [ FONT_PIN_COUNT ];           // eviction-exempt ids (0 = pin free)

    u32               generation;                       // baker installs bump this (dpi re-poll signal)
    u32               frame_gen;                        // emitted-frame counter (font_resolve_frame_tick)
    bool              fresh;                            // a resolve loaded pixels since the last take

} s_resolver = { .frame_gen = 1 };                      // gen 0 = "never requested" in seen_gen

/*==============================================================================================
    Small helpers -- hashing, names, pin/liveness tests.
==============================================================================================*/

static u32
resolve_hash_str( const char* s )
{
    u32 h = 2166136261u;
    for ( ; *s; ++s )
    {
        h ^= (u8)*s;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/* Hash of a request's family, normalized so two spellings of one directory share a lineage. */
static u32
resolve_name_hash( const char* family )
{
    char norm[ 96 ];
    font_name_normalize( family ? family : "", norm, sizeof( norm ) );
    return resolve_hash_str( norm );
}

/* The resource name a request resolves to: "font/<family>/<size>".  False when it does not fit. */
static bool
resolve_name( char* out, size_t cap, const char* family, u32 size_px )
{
    int n = fmt_snprintf( out, (int)cap, "font/%s/%u", family, size_px );
    return n > 0 && (size_t)n < cap;
}

static bool
resolve_id_pinned( u32 id )
{
    for ( u32 i = 0; i < FONT_PIN_COUNT; ++i )
        if ( s_resolver.pin[ i ] == id )
            return true;
    return false;
}

/* The immediate-mode hold: a public font_get requested this entry within the last emitted
   frame.  Stale entries keep their slot resident but stop protecting it. */
static bool
resolve_entry_live( const font_memo_t* m )
{
    return m->seen_gen != 0 && s_resolver.frame_gen - m->seen_gen <= 1u;
}

/* True when any live memo entry -- owner or alias -- names this id on the host's behalf. */
static bool
resolve_id_live( u32 id )
{
    for ( u32 i = 0; i < s_resolver.memo_count; ++i )
        if ( s_resolver.memo[ i ].id == id && resolve_entry_live( &s_resolver.memo[ i ] ) )
            return true;
    return false;
}

/*==============================================================================================
    Eviction -- how the resolver lives inside 16 registry slots.
==============================================================================================*/

/* Release one evictable OWNER: its registry slot and atlas tenant go back to the pool, and
   every memo entry naming the id (the owner plus any aliases) is dropped -- an alias must
   never outlive its slot.  False when nothing may be evicted (every owner live or pinned).
   The ambient active font and the draw stamp are never touched, whatever their memo state --
   reclaiming the font mid-use would hand its slot to different glyphs under the same id. */
static bool
resolve_evict_one( void )
{
    for ( u32 i = 0; i < s_resolver.memo_count; ++i )
    {
        font_memo_t* m = &s_resolver.memo[ i ];
        if ( m->id == 0 || m->id == FONT_RESOLVE_FAILED ) continue;
        if ( m->landed_px != m->size_px )                 continue;   /* alias: owns nothing */
        if ( resolve_id_pinned( m->id ) || resolve_id_live( m->id ) )
            continue;
        if ( m->id == font_active_id() || m->id == draw_get_font() )
            continue;

        /* A slot still waiting for its first atlas upload is mid-arrival, not stale --
           evicting it would drop pixels loaded this very frame (a prewarm bake for another
           viewport lands unpinned until that surface's own resolve). */
        font_slot_t* slot = font_slot_ptr( m->id );
        if ( slot && slot->needs_upload )
            continue;

        u32 id = m->id;
        font_slot_release( id );
        for ( u32 j = 0; j < s_resolver.memo_count; )
        {
            if ( s_resolver.memo[ j ].id == id )
                s_resolver.memo[ j ] = s_resolver.memo[ --s_resolver.memo_count ];
            else
                ++j;
        }
        return true;
    }
    return false;
}

/* Make room in the memo table itself: drop a latch or a stale alias first (both free -- no
   slot behind them), else evict an owner.  False leaves the table full; the caller degrades
   without memoizing. */
static bool
resolve_memo_space( void )
{
    if ( s_resolver.memo_count < FONT_RESOLVE_MEMO_MAX )
        return true;

    for ( u32 i = 0; i < s_resolver.memo_count; ++i )
    {
        font_memo_t* m = &s_resolver.memo[ i ];
        bool free_entry = m->id == FONT_RESOLVE_FAILED
                       || ( m->landed_px != m->size_px && !resolve_entry_live( m ) );
        if ( free_entry )
        {
            s_resolver.memo[ i ] = s_resolver.memo[ --s_resolver.memo_count ];
            return true;
        }
    }
    return resolve_evict_one();
}

static void
resolve_memo_insert( u32 name_hash, u32 size_px, bool touch, u32 id, u32 landed_px )
{
    if ( s_resolver.memo_count >= FONT_RESOLVE_MEMO_MAX )
        return;   /* resolve_memo_space ran (or was refused) before any load -- just stay safe */

    font_memo_t* m = &s_resolver.memo[ s_resolver.memo_count++ ];
    m->name_hash = name_hash;
    m->size_px   = (u16)size_px;
    m->traits    = 0;
    m->_unused   = 0;
    m->id        = (u16)id;
    m->landed_px = (u16)landed_px;
    m->seen_gen  = touch ? s_resolver.frame_gen : 0;
}

/*==============================================================================================
    Loading -- the one guarded path every resolver load takes.
==============================================================================================*/

/* font_load_mem ACTIVATES what it loads, so the active font and the draw stamp are restored
   around it -- a resolver load must never move the ambient state the DPI lineage guard and
   draw_reset read.  A load refused by a FULL registry (not a parse failure) evicts one owner
   and retries once. */
static u32
resolve_load( const void* data, u32 size, const char* name )
{
    u32 prev      = font_active_id();
    u32 prev_draw = draw_get_font();

    u32 id = font_load_mem( data, size, name );
    if ( id == 0 && font_alloc_slot() == 0 && resolve_evict_one() )
        id = font_load_mem( data, size, name );

    font_use( prev );
    draw_set_font( prev_draw );

    if ( id )
        s_resolver.fresh = true;
    return id;
}

/*==============================================================================================
    Layers (b) and (c) -- where a bake's bytes come from.

    Both hand back a buffer the caller releases with resolve_bytes_free: layer (b) is an fs
    blob, layer (c) a malloc'd buffer from the baker (gui_font_bake_fn).  `name` is the
    resource name the bytes stand for, kept as the slot's identity.
==============================================================================================*/

typedef struct
{
    void*     data;
    u32       size;
    bool      ok;
    bool      from_fs;    // release through fs()->free rather than free()
    fs_blob_t blob;

} resolve_bytes_t;

/* Layer (b): the cooked bake through the mounts.  Quiet on a miss -- most requests that reach
   this layer are runtime-composed sizes no RID() named, and the layers below are the answer. */
static resolve_bytes_t
resolve_bytes_cooked( const char* name )
{
    resolve_bytes_t r = { 0 };
    r.blob = gui_res_read( name, ".orb_font" );
    if ( r.blob.ok )
    {
        r.data    = r.blob.data;
        r.size    = r.blob.size;
        r.ok      = true;
        r.from_fs = true;
    }
    return r;
}

/* Layer (c): the installed baker, asked for the family's typeface at the exact size. */
static resolve_bytes_t
resolve_bytes_baked( const char* family, u32 size_px )
{
    resolve_bytes_t r = { 0 };
    if ( !s_resolver.baker )
        return r;
    void* data = NULL;
    u32   size = 0;
    if ( s_resolver.baker( font_family_face( family ), size_px, &data, &size, s_resolver.baker_user )
         && data && size )
    {
        r.data = data;
        r.size = size;
        r.ok   = true;
    }
    else
    {
        free( data );
    }
    return r;
}

static void
resolve_bytes_free( resolve_bytes_t* r )
{
    if ( r->from_fs )
        fs()->free( &r->blob );
    else
        free( r->data );
    memset( r, 0, sizeof( *r ) );
}

/* Layers (b) then (c) for one (family, size): the bake's bytes, or ok=false. */
static resolve_bytes_t
resolve_bytes( const char* family, u32 size_px, const char* name )
{
    resolve_bytes_t r = resolve_bytes_cooked( name );
    if ( !r.ok )
        r = resolve_bytes_baked( family, size_px );
    return r;
}

/*==============================================================================================
    font_resolve -- the front door.
==============================================================================================*/

u32
font_resolve( const char* family, u32 size_px, bool touch, u32* out_landed_px )
{
    if ( out_landed_px )
        *out_landed_px = 0;
    if ( size_px == 0 || size_px > 0xFFFFu || !family || !*family )
        return 0;

    u32 hash = resolve_name_hash( family );

    /* (a) resident memo -- exact key hit answers immediately (latch answers "default").  A
       public request re-stamps the entry: the per-frame font_get call IS the hold. */
    for ( u32 i = 0; i < s_resolver.memo_count; ++i )
    {
        font_memo_t* m = &s_resolver.memo[ i ];
        if ( m->name_hash != hash || m->size_px != (u16)size_px || m->traits != 0 )
            continue;
        if ( touch )
            m->seen_gen = s_resolver.frame_gen;
        if ( m->id == FONT_RESOLVE_FAILED )
            return 0;
        if ( out_landed_px )
            *out_landed_px = m->landed_px;
        return m->id;
    }

    if ( !resolve_memo_space() )
    {
        GUI_WARN_ONCE( "font resolver: memo full and nothing evictable -- request degraded" );
        return 0;
    }

    /* (b) the cooked bake, (c) the baker -- either way the exact size. */
    char name[ RES_NAME_MAX + 1 ];
    if ( resolve_name( name, sizeof( name ), family, size_px ) )
    {
        resolve_bytes_t bytes = resolve_bytes( family, size_px, name );
        if ( bytes.ok )
        {
            u32 id = resolve_load( bytes.data, bytes.size, name );
            resolve_bytes_free( &bytes );
            if ( id )
            {
                resolve_memo_insert( hash, size_px, touch, id, size_px );
                if ( out_landed_px )
                    *out_landed_px = size_px;
                return id;
            }
        }
    }

    /* (d) ladder -- nearest RESIDENT in-family size, ties toward the LARGER size (a step too
       big stays readable). */
    {
        u32 best_size = 0;
        u32 best_id   = 0;

        for ( u32 i = 0; i < s_resolver.memo_count; ++i )
        {
            font_memo_t* m = &s_resolver.memo[ i ];
            if ( m->name_hash != hash || m->id == FONT_RESOLVE_FAILED
                 || m->landed_px != m->size_px )
                continue;
            u32 c  = m->size_px;
            u32 d  = c > size_px ? c - size_px : size_px - c;
            u32 bd = best_size > size_px ? best_size - size_px : size_px - best_size;
            if ( !best_size || d < bd || ( d == bd && c > best_size ) )
            {
                best_size = c;
                best_id   = m->id;
            }
        }

        if ( best_size )
        {
            resolve_memo_insert( hash, size_px, touch, best_id, best_size );
            GUI_WARN_ONCE( "font resolver: no bake for '%s' at %upx -- nearest resident is %upx "
                           "(mark RID( \"font/%s/%u\" ) or install a font baker for exact sizes)",
                           family, size_px, best_size, family, size_px );
            if ( out_landed_px )
                *out_landed_px = best_size;
            return best_id;
        }
    }

    /* Nothing in-family at any size: latch, warn, fall to the default font. */
    resolve_memo_insert( hash, size_px, touch, FONT_RESOLVE_FAILED, 0 );
    GUI_WARN_ONCE( "font resolver: no bake for '%s' at %upx -- falling back to the default font",
                   family, size_px );
    return 0;
}

/*==============================================================================================
    font_resolve_boot -- load a family at a size into slot 0 (the boot font).

    gui_init's slot-0 load: layers (b) and (c) minus the memo (nothing is resident yet), landing
    in the default slot via font_load_into_mem so the whole default-font machinery (style
    landing, DPI base) keeps its slot-0 anchor.  There is no nearest-size fallback here: the
    mounts cannot be enumerated, and the boot size is the one name every host marks with
    RID(), so it is cooked by construction.  The landed (family, size) is memoized to id 0 by
    gui_dpi_base_set (font_resolve_adopt_default) so later resolves of the base size answer the
    boot slot instead of loading a duplicate.
==============================================================================================*/

void
font_resolve_boot( const char* family, u32 size_px, u32* out_landed_px )
{
    if ( out_landed_px )
        *out_landed_px = 0;
    if ( !family || !*family || size_px == 0 )
        return;

    char name[ RES_NAME_MAX + 1 ];
    if ( !resolve_name( name, sizeof( name ), family, size_px ) )
        return;

    resolve_bytes_t bytes = resolve_bytes( family, size_px, name );
    bool            ok    = bytes.ok && font_load_into_mem( 0, bytes.data, bytes.size, name );
    if ( bytes.ok )
        resolve_bytes_free( &bytes );

    if ( !ok )
    {
        gui_log( GUI_LOG_WARN, "boot font: no bake for '%s' (not in the content mounts, no baker) "
                               "-- continuing without text", name );
        return;
    }

    if ( out_landed_px )
        *out_landed_px = size_px;
}

/* Memo the default slot as the answer for (family, landed_px), so resolving the base size hits
   slot 0 instead of loading a duplicate.  gui_dpi_base_set calls this after its clear. */

void
font_resolve_adopt_default( const char* family, u32 landed_px )
{
    if ( !family || !*family || landed_px == 0 )
        return;
    if ( resolve_memo_space() )
        resolve_memo_insert( resolve_name_hash( family ), landed_px, false, 0, landed_px );
}

/*==============================================================================================
    Frame + lifecycle surface.
==============================================================================================*/

/* A fresh load happened since the last take -- frame_begin dirties the frame so new pixels
   paint (they ride the same frame's atlas flush). */
bool
font_resolve_fresh_take( void )
{
    bool f = s_resolver.fresh;
    s_resolver.fresh = false;
    return f;
}

u32
font_resolve_generation( void )
{
    return s_resolver.generation;
}

/* One stale-font eviction on demand -- the atlas packer's pressure valve.  res_place
   (render/resource/gui_res_atlas.c) calls this upward when a tenant set stops fitting:
   retiring a stale font frees its tenant so the packer can re-trial at the CURRENT texture
   size, and the atlas takes a growth rung only when nothing stale remains.  True = a font
   was released (try again); false = every resident font is live, pinned, or mid-upload. */
bool
font_resolve_evict_stale( void )
{
    return resolve_evict_one();
}

/* Advance the immediate-mode retention clock -- called once per EMITTED frame (gui_frame_end,
   gated on the frame having actually run widget code).  A clean skipped frame must not age
   the holds: no emission ran, so no font_get could have re-stamped its entry.  Entries not
   re-requested within one emitted frame go stale -- still resident, but reclaimable the
   moment a load needs a registry slot; re-requesting reloads. */
void
font_resolve_frame_tick( void )
{
    ++s_resolver.frame_gen;
}

/* Mark an id eviction-exempt while its owner (a ramp role, a viewport's landed font) has it
   live.  0 clears the pin. */
void
font_resolve_pin( u32 pin_slot, u32 id )
{
    if ( pin_slot < FONT_PIN_COUNT )
        s_resolver.pin[ pin_slot ] = id;
}

/* Forget everything resolved -- the managed family changed underneath (gui_dpi_base_set) or
   the GUI is shutting down.  Pins are cleared first (their owners re-pin after re-resolving),
   then every stale owner's slot is released.  LIVE entries survive: an id a font_get handed
   out this frame may still be applied later in the same build, and a family change must not
   tear it out from under the host mid-frame. */
void
font_resolve_clear( void )
{
    for ( u32 i = 0; i < FONT_PIN_COUNT; ++i )
        s_resolver.pin[ i ] = 0;

    for ( u32 i = 0; i < s_resolver.memo_count; )
    {
        font_memo_t* m = &s_resolver.memo[ i ];

        if ( resolve_entry_live( m ) )       { ++i; continue; }
        if ( resolve_id_live( m->id ) )      { ++i; continue; }   /* alias target of a live entry */

        if ( m->id != 0 && m->id != FONT_RESOLVE_FAILED && m->landed_px == m->size_px )
            font_slot_release( m->id );
        *m = s_resolver.memo[ --s_resolver.memo_count ];
    }
}

/* Install / replace the runtime baker.  Failure latches and stale degraded aliases are
   dropped -- the new baker may bake what the ladder had to substitute -- and the generation
   moves so the DPI engine re-resolves every viewport.  Exact loads are kept.  NULL uninstalls. */
void
gui_font_baker_set( gui_font_bake_fn fn, void* user )
{
    s_resolver.baker      = fn;
    s_resolver.baker_user = user;

    u32 keep = 0;
    for ( u32 i = 0; i < s_resolver.memo_count; ++i )
    {
        font_memo_t* m = &s_resolver.memo[ i ];
        bool drop = m->id == FONT_RESOLVE_FAILED
                 || ( m->landed_px != m->size_px && !resolve_entry_live( m ) );
        if ( !drop )
            s_resolver.memo[ keep++ ] = *m;
    }
    s_resolver.memo_count = keep;
    ++s_resolver.generation;

    gui_type_resolve();   /* pre-init the guards bail; post-init the roles re-aim now */
}

/*==============================================================================================
    Debug readout -- the font overlay's window into the ledger (gui_frame_overlay.c).
==============================================================================================*/

font_resolve_debug_t
font_resolve_debug( void )
{
    font_resolve_debug_t d;
    d.memo_used = s_resolver.memo_count;
    d.memo_cap  = FONT_RESOLVE_MEMO_MAX;
    d.baker     = s_resolver.baker != NULL;
    return d;
}

/* Ownership marks for one registry id: G = a public font_get requested it within the last
   emitted frame (the immediate-mode hold), S / L = live type-ramp role, v<N> = viewport N's
   landed DPI font.  Empty = evictable (or the default slot 0, which is implicitly pinned
   and carries no marks). */
void
font_resolve_debug_flags( u32 id, char* out, int out_size )
{
    if ( out_size <= 0 )
        return;
    out[ 0 ] = 0;
    if ( id == 0 )
        return;

    int n = 0;
    if ( resolve_id_live( id ) && n < out_size - 1 )
        out[ n++ ] = 'G';
    if ( s_resolver.pin[ FONT_PIN_SMALL ] == id && n < out_size - 1 )
        out[ n++ ] = 'S';
    if ( s_resolver.pin[ FONT_PIN_LARGE ] == id && n < out_size - 1 )
        out[ n++ ] = 'L';
    out[ n ] = 0;

    for ( u32 v = 0; v < GUI_MAX_VIEWPORTS; ++v )
    {
        if ( s_resolver.pin[ FONT_PIN_VP0 + v ] == id && n < out_size - 3 )
        {
            n += fmt_snprintf( out + n, (size_t)( out_size - n ), "v%u", v );
        }
    }
}

// clang-format on
/*============================================================================================*/
