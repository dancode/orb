/*==============================================================================================

    runtime_service/gui/frame/gui_frame_resolve.c -- THE FONT RESOLVER: (family, size) -> id.

    Every managed font in the GUI comes through here: the boot font, the DPI engine's
    per-monitor sizes, the type ramp's SMALL/LARGE roles, and the public font_get.  A font is
    a REQUEST -- a family plus a pixel size -- and this file turns requests into loaded
    registry ids through a layered search:

        (a) resident  -- the memo already holds this (family, size); free.
        (b) shipped   -- an exact-size bake under assets/font/ (lazy one-time directory scan).
        (c) baker     -- the host-installed runtime baker (font_baker_set; dev_font_get is the
                         canonical wiring) bakes the exact size on demand.
        (d) ladder    -- nearest resident or shipped size in the same family, ties toward the
                         LARGER bake; failing even that, the default font (id 0).

    A resolve never hard-fails and never activates: the active font and the draw stamp are
    restored around every load, so no resolution can read as a host takeover to the DPI
    lineage guard.  Callers that need exact sizes (the type ramp) check the landed size the
    resolver reports; callers that want the nearest answer (the DPI engine) take it as-is.

    The memo is also the ownership ledger for the registry slots the resolver mints.  Entries
    resolved through the public font_get are HELD (the host caches the id) and never evicted;
    the DPI engine and type ramp PIN their live ids per frame instead.  Under registry
    pressure one unheld, unpinned owner is released (slot + atlas tenant) and the load
    retried.  A degraded answer (landed != wanted) is recorded as an ALIAS of the owning
    entry's slot -- it releases nothing -- and a request no layer could serve latches a
    failure sentinel so it costs one attempt, not one per frame.  Installing a baker drops
    latches and unheld aliases and bumps a generation counter, which tells the DPI engine to
    re-resolve: early degraded answers self-heal once a baker arrives.

    Included by gui_frame.c after gui_frame_font.c (its loads ride the same deferred-upload
    flush) and before gui_frame_dpi.c / gui_frame_type.c, its two internal clients.

==============================================================================================*/
// clang-format off

#define FONT_RESOLVE_MEMO_MAX 24            /* > registry 16: latches + aliases hold no slot */
#define FONT_RESOLVE_FAILED   0xFFFFFFFFu   /* memo id: no layer could serve; one attempt per generation */
#define FONT_RESOLVE_SHIP_MAX 64            /* shipped-scan entries (assets/font) */

typedef struct
{
    u32 name_hash;   // FNV-1a of the normalized family name (key)
    u16 size_px;     // requested size (key)
    u8  traits;      // reserved, always 0 -- the rich-type (bold/italic span) future keys here
    u8  held;        // public font_get resolved this: never evicted (the host holds the id)
    u32 id;          // registry id; 0 = the default slot; FONT_RESOLVE_FAILED = latch
    u16 landed_px;   // size actually resolved; != size_px marks a degraded ALIAS (owns no slot)

} font_memo_t;

typedef struct
{
    u32  stem_hash;  // normalized filename stem
    u16  size_px;    // parsed from "_<N>px"
    u8   tagged;     // filename carries tags after the size token
    u8   sdf;        // "_sdf" tag present -- never auto-resolved (SDF is an authored choice)
    char name[ 96 ]; // filename inside assets/font/

} font_ship_entry_t;

/* Pin slots -- ids the eviction pass may never touch while their owner has them live.
   Slot 0 (the default font) is implicitly pinned; held memo entries protect themselves. */
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

    font_ship_entry_t ship[ FONT_RESOLVE_SHIP_MAX ];    // parsed assets/font/ listing
    u32               ship_count;
    bool              ship_scanned;                     // lazy one-time sys_file_glob

    u32               generation;                       // baker installs bump this (dpi re-poll signal)
    bool              fresh;                            // a resolve loaded pixels since the last take

} s_resolver;

/*==============================================================================================
    Small helpers -- hashing, canonical names, pin/held tests.
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

/* Hash of a request's canonical family name: the baker source string for a curated family
   (so an enum request and a string request for the same face share one memo lineage), the raw
   string otherwise. */
static u32
resolve_name_hash( gui_font_family_t fam, const char* name )
{
    const char* src = fam != GUI_FONT_NONE ? font_family_bake_source( fam ) : name;
    char        norm[ 96 ];
    font_name_normalize( src ? src : "", norm, sizeof( norm ) );
    return resolve_hash_str( norm );
}

/* Hash the shipped-bake scan matches on: the ship stem for a curated family, the raw string
   otherwise -- a string request finds shipped files only by their actual file stem. */
static u32
resolve_ship_hash( gui_font_family_t fam, const char* name )
{
    const char* src = fam != GUI_FONT_NONE ? font_family_ship_stem( fam ) : name;
    char        norm[ 96 ];
    font_name_normalize( src ? src : "", norm, sizeof( norm ) );
    return resolve_hash_str( norm );
}

static bool
resolve_id_pinned( u32 id )
{
    for ( u32 i = 0; i < FONT_PIN_COUNT; ++i )
        if ( s_resolver.pin[ i ] == id )
            return true;
    return false;
}

/* True when any memo entry -- owner or alias -- holds this id on the host's behalf. */
static bool
resolve_id_held( u32 id )
{
    for ( u32 i = 0; i < s_resolver.memo_count; ++i )
        if ( s_resolver.memo[ i ].held && s_resolver.memo[ i ].id == id )
            return true;
    return false;
}

/*==============================================================================================
    Eviction -- how the resolver lives inside 16 registry slots.
==============================================================================================*/

/* Release one evictable OWNER: its registry slot and atlas tenant go back to the pool, and
   every memo entry naming the id (the owner plus any aliases) is dropped -- an alias must
   never outlive its slot.  False when nothing may be evicted (all owners live or held). */
static bool
resolve_evict_one( void )
{
    for ( u32 i = 0; i < s_resolver.memo_count; ++i )
    {
        font_memo_t* m = &s_resolver.memo[ i ];
        if ( m->id == 0 || m->id == FONT_RESOLVE_FAILED ) continue;
        if ( m->landed_px != m->size_px )                 continue;   /* alias: owns nothing */
        if ( m->held || resolve_id_pinned( m->id ) || resolve_id_held( m->id ) )
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

/* Make room in the memo table itself: drop a latch or an unheld alias first (both free -- no
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
                       || ( m->landed_px != m->size_px && !m->held );
        if ( free_entry )
        {
            s_resolver.memo[ i ] = s_resolver.memo[ --s_resolver.memo_count ];
            return true;
        }
    }
    return resolve_evict_one();
}

static void
resolve_memo_insert( u32 name_hash, u32 size_px, bool held, u32 id, u32 landed_px )
{
    if ( s_resolver.memo_count >= FONT_RESOLVE_MEMO_MAX )
        return;   /* resolve_memo_space ran (or was refused) before any load -- just stay safe */

    font_memo_t* m = &s_resolver.memo[ s_resolver.memo_count++ ];
    m->name_hash = name_hash;
    m->size_px   = (u16)size_px;
    m->traits    = 0;
    m->held      = held ? 1 : 0;
    m->id        = id;
    m->landed_px = (u16)landed_px;
}

/*==============================================================================================
    Loading -- the one guarded path every resolver load takes.
==============================================================================================*/

/* font_load ACTIVATES what it loads, so the active font and the draw stamp are restored around
   it -- a resolver load must never move the ambient state the DPI lineage guard and draw_reset
   read.  A load refused by a FULL registry (not a parse failure) evicts one owner and retries
   once. */
static u32
resolve_load( const char* path )
{
    u32 prev      = font_active_id();
    u32 prev_draw = draw_get_font();

    u32 id = font_load( path );
    if ( id == 0 && font_alloc_slot() == 0 && resolve_evict_one() )
        id = font_load( path );

    font_use( prev );
    draw_set_font( prev_draw );

    if ( id )
        s_resolver.fresh = true;
    return id;
}

/*==============================================================================================
    Layer (b) -- the shipped-bake scan of assets/font/.
==============================================================================================*/

static bool
resolve_ship_cb( const char* filename, const char* full_path, void* ud )
{
    (void)full_path; (void)ud;
    if ( s_resolver.ship_count >= FONT_RESOLVE_SHIP_MAX )
        return false;

    char stem[ 96 ];
    u32  px;
    bool tagged, sdf;
    if ( !font_ship_name_parse( filename, stem, sizeof( stem ), &px, &tagged, &sdf ) )
        return true;   /* size-less name: not resolvable, skip */

    char norm[ 96 ];
    font_name_normalize( stem, norm, sizeof( norm ) );

    font_ship_entry_t* e = &s_resolver.ship[ s_resolver.ship_count++ ];
    e->stem_hash = resolve_hash_str( norm );
    e->size_px   = (u16)px;
    e->tagged    = tagged ? 1 : 0;
    e->sdf       = sdf ? 1 : 0;
    fmt_snprintf( e->name, sizeof( e->name ), "%s", filename );
    return true;
}

static void
resolve_ship_scan( void )
{
    if ( s_resolver.ship_scanned )
        return;
    s_resolver.ship_scanned = true;

    char dir[ 576 ];
    fmt_snprintf( dir, sizeof( dir ), "%s/assets/font", sys_root_dir() );
    sys_file_glob( dir, "*.orb_font", resolve_ship_cb, NULL );
}

/* The shipped file serving (stem, size), or NULL.  Untagged coverage beats tagged; SDF files
   never answer. */
static const font_ship_entry_t*
resolve_ship_find( u32 stem_hash, u32 size_px )
{
    resolve_ship_scan();

    const font_ship_entry_t* best = NULL;
    for ( u32 i = 0; i < s_resolver.ship_count; ++i )
    {
        const font_ship_entry_t* e = &s_resolver.ship[ i ];
        if ( e->stem_hash != stem_hash || e->size_px != size_px || e->sdf )
            continue;
        if ( !best || ( best->tagged && !e->tagged ) )
            best = e;
    }
    return best;
}

static void
resolve_ship_path( const font_ship_entry_t* e, char* out, int out_size )
{
    fmt_snprintf( out, (size_t)out_size, "%s/assets/font/%s", sys_root_dir(), e->name );
}

/*==============================================================================================
    font_resolve -- the front door.
==============================================================================================*/

u32
font_resolve( gui_font_family_t fam, const char* name, u32 size_px,
              bool held, u32* out_landed_px )
{
    if ( out_landed_px )
        *out_landed_px = 0;
    if ( size_px == 0 || size_px > 0xFFFFu
         || ( fam == GUI_FONT_NONE && ( !name || !*name ) ) )
        return 0;

    u32 hash = resolve_name_hash( fam, name );

    /* (a) resident memo -- exact key hit answers immediately (latch answers "default"). */
    for ( u32 i = 0; i < s_resolver.memo_count; ++i )
    {
        font_memo_t* m = &s_resolver.memo[ i ];
        if ( m->name_hash != hash || m->size_px != (u16)size_px || m->traits != 0 )
            continue;
        if ( held )
            m->held = 1;
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

    u32 ship_hash = resolve_ship_hash( fam, name );

    /* (b) shipped bake at the exact size. */
    {
        const font_ship_entry_t* e = resolve_ship_find( ship_hash, size_px );
        if ( e )
        {
            char path[ 576 ];
            resolve_ship_path( e, path, sizeof( path ) );
            u32 id = resolve_load( path );
            if ( id )
            {
                resolve_memo_insert( hash, size_px, held, id, size_px );
                if ( out_landed_px )
                    *out_landed_px = size_px;
                return id;
            }
        }
    }

    /* (c) the installed baker -- exact size on demand. */
    {
        const char* source = fam != GUI_FONT_NONE ? font_family_bake_source( fam ) : name;
        if ( s_resolver.baker && source )
        {
            char path[ 576 ];
            if ( s_resolver.baker( source, size_px, path, sizeof( path ), s_resolver.baker_user ) )
            {
                u32 id = resolve_load( path );
                if ( id )
                {
                    resolve_memo_insert( hash, size_px, held, id, size_px );
                    if ( out_landed_px )
                        *out_landed_px = size_px;
                    return id;
                }
            }
        }
    }

    /* (d) ladder -- nearest in-family size, resident answers preferred over loading a shipped
       neighbour, ties toward the LARGER size (a step too big stays readable). */
    {
        u32 best_size = 0;
        u32 best_id   = 0;   /* 0 = the winning size is a shipped file not yet resident */

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

        resolve_ship_scan();
        for ( u32 i = 0; i < s_resolver.ship_count; ++i )
        {
            const font_ship_entry_t* e = &s_resolver.ship[ i ];
            if ( e->stem_hash != ship_hash || e->sdf || e->size_px == size_px )
                continue;
            u32 c  = e->size_px;
            u32 d  = c > size_px ? c - size_px : size_px - c;
            u32 bd = best_size > size_px ? best_size - size_px : size_px - best_size;
            if ( !best_size || d < bd || ( d == bd && c > best_size ) )
            {
                best_size = c;
                best_id   = 0;   /* load below */
            }
        }

        if ( best_size && best_id == 0 )
        {
            const font_ship_entry_t* e = resolve_ship_find( ship_hash, best_size );
            if ( e )
            {
                char path[ 576 ];
                resolve_ship_path( e, path, sizeof( path ) );
                best_id = resolve_load( path );
                if ( best_id )
                    resolve_memo_insert( hash, best_size, false, best_id, best_size );
            }
            if ( !best_id )
                best_size = 0;
        }

        if ( best_size )
        {
            resolve_memo_insert( hash, size_px, held, best_id, best_size );
            GUI_WARN_ONCE( "font resolver: no exact bake at %upx -- nearest is %upx "
                           "(install a font baker for exact sizes)", size_px, best_size );
            if ( out_landed_px )
                *out_landed_px = best_size;
            return best_id;
        }
    }

    /* Nothing in-family at any size: latch, warn, fall to the default font. */
    resolve_memo_insert( hash, size_px, held, FONT_RESOLVE_FAILED, 0 );
    GUI_WARN_ONCE( "font resolver: no bake for '%s' at %upx -- falling back to the default font",
                   fam != GUI_FONT_NONE ? ( font_family_bake_source( fam )
                                                ? font_family_bake_source( fam ) : "?" )
                                            : name,
                   size_px );
    return 0;
}

/*==============================================================================================
    font_resolve_boot -- resolve a PATH and load it into slot 0 (the boot font).

    gui_init's slot-0 load: same layers minus the memo (nothing is resident yet), landing in
    the default slot via font_load_into so the whole default-font machinery (style landing,
    DPI base) keeps its slot-0 anchor.  The landed (family, size) is memoized to id 0 so later
    resolves of the base size answer the boot slot instead of loading a duplicate.
==============================================================================================*/

void
font_resolve_boot( gui_font_family_t fam, u32 size_px, u32* out_landed_px )
{
    if ( out_landed_px )
        *out_landed_px = 0;
    if ( fam == GUI_FONT_NONE || size_px == 0 )
        return;

    u32  hash      = resolve_name_hash( fam, NULL );
    u32  ship_hash = resolve_ship_hash( fam, NULL );
    char path[ 576 ];
    u32  landed = 0;

    const font_ship_entry_t* e = resolve_ship_find( ship_hash, size_px );
    if ( e )
    {
        resolve_ship_path( e, path, sizeof( path ) );
        landed = size_px;
    }
    else if ( s_resolver.baker && font_family_bake_source( fam )
              && s_resolver.baker( font_family_bake_source( fam ), size_px,
                                   path, sizeof( path ), s_resolver.baker_user ) )
    {
        landed = size_px;
    }
    else
    {
        /* Nearest shipped size -- the bakerless boot still comes up readable. */
        u32 best = 0;
        resolve_ship_scan();
        for ( u32 i = 0; i < s_resolver.ship_count; ++i )
        {
            const font_ship_entry_t* s = &s_resolver.ship[ i ];
            if ( s->stem_hash != ship_hash || s->sdf )
                continue;
            u32 c  = s->size_px;
            u32 d  = c > size_px ? c - size_px : size_px - c;
            u32 bd = best > size_px ? best - size_px : size_px - best;
            if ( !best || d < bd || ( d == bd && c > best ) )
                best = c;
        }
        if ( best && ( e = resolve_ship_find( ship_hash, best ) ) != NULL )
        {
            resolve_ship_path( e, path, sizeof( path ) );
            landed = best;
        }
    }

    if ( !landed || !font_load_into( 0, path ) )
    {
        gui_log( GUI_LOG_WARN, "boot font: no bake for family %u at %upx -- continuing without text",
                 (u32)fam, size_px );
        return;
    }

    ( void )hash;   /* the memo entry for the boot size is gui_dpi_base_set's to place
                       (font_resolve_adopt_default) -- it clears the memo first */
    if ( out_landed_px )
        *out_landed_px = landed;
}

/* Memo the default slot as the answer for (fam, landed_px), so resolving the base size hits
   slot 0 instead of loading a duplicate.  gui_dpi_base_set calls this after its clear. */

void
font_resolve_adopt_default( gui_font_family_t fam, u32 landed_px )
{
    if ( fam == GUI_FONT_NONE || landed_px == 0 )
        return;
    if ( resolve_memo_space() )
        resolve_memo_insert( resolve_name_hash( fam, NULL ), landed_px, false, 0, landed_px );
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
   then every unheld owner's slot is released.  HELD entries survive: a host caches font_get
   ids in statics, and a family change must not tear them out from under it. */
void
font_resolve_clear( void )
{
    for ( u32 i = 0; i < FONT_PIN_COUNT; ++i )
        s_resolver.pin[ i ] = 0;

    for ( u32 i = 0; i < s_resolver.memo_count; )
    {
        font_memo_t* m = &s_resolver.memo[ i ];

        if ( m->held )                       { ++i; continue; }
        if ( resolve_id_held( m->id ) )      { ++i; continue; }   /* alias target of a held entry */

        if ( m->id != 0 && m->id != FONT_RESOLVE_FAILED && m->landed_px == m->size_px )
            font_slot_release( m->id );
        *m = s_resolver.memo[ --s_resolver.memo_count ];
    }
}

/* Install / replace the runtime baker.  Failure latches and unheld degraded aliases are
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
                 || ( m->landed_px != m->size_px && !m->held );
        if ( !drop )
            s_resolver.memo[ keep++ ] = *m;
    }
    s_resolver.memo_count = keep;
    ++s_resolver.generation;

    gui_type_resolve();   /* pre-init the guards bail; post-init the roles re-aim now */
}

// clang-format on
/*============================================================================================*/
