#ifndef WORLD_H
#define WORLD_H
/*==============================================================================================

    game/framework/world.h -- the reflected world: entities, components, queries.

    The world is DATA DESCRIBED BY REF_: a component is any REF_STRUCT-annotated POD;
    registering it takes one line and the pool layout (size, schema) comes from the
    reflection registry.  Storage is per-component dense pools iterated linearly --
    no archetypes, no system scheduler.  Systems are plain functions the project calls
    from on_sim / on_frame.

    Everything is addressed by id:
        world_id_t  -- slot into the framework's bound-world table
        ent_t       -- { index, generation } stale-safe handle (the asset_id_t idiom)
        comp_id_t   -- component registration index within a world

    Pointers appear only as TRANSIENT access to component data inside a call or a query
    loop.  They are never stored, and never cross a frame or a hot-reload boundary.

    world_t is one fixed-size POD block with no pointers inside (pool storage is
    offset-addressed), so an owner that embeds it in module state gets hot-reload
    persistence for free.  The owner must call world_bind() in BOTH init() and reload(),
    and re-issue its comp_register() calls in both (the cvar re-registration idiom) --
    ref type ids are only stable within one load of a module.

    This header is the whole public surface; the implementation joins the owner's unity
    via game/framework/framework.c.

==============================================================================================*/

#include "orb.h"
#include "engine/core/sid/sid.h"

/*==============================================================================================
    Limits

    Fixed capacities keep world_t a flat POD block (creed: state is POD in system-owned
    memory).  Raise them here when a real project outgrows them.
==============================================================================================*/

#define WORLD_MAX_WORLDS  4                       // simultaneously bound worlds (play + editor previews)
#define WORLD_MAX_ENT     4096                    // entity slots per world; index 0 is reserved invalid
#define WORLD_MAX_COMP    64                      // component types per world; comp_id is a u64 mask bit
#define WORLD_POOL_BYTES  ( 8u * 1024u * 1024u )  // pool arena carved at comp_register time

#define WORLD_INVALID     ( ( world_id_t )-1 )
#define COMP_INVALID      ( ( comp_id_t )-1 )

/*==============================================================================================
    Handles
==============================================================================================*/

/* Stale-safe entity handle.  index is 1-based (0 = invalid, so a zeroed ent_t is invalid);
   generation is bumped when the slot is recycled so an old copy is detected as dead.
   Pass by value; compare with ent_eq. */
typedef struct ent_s
{
    u32 index;
    u32 gen;

} ent_t;

typedef i32 world_id_t;    // bound-world slot; WORLD_INVALID when unbound
typedef i32 comp_id_t;     // per-world registration index; stable if registration order is stable

static inline bool
ent_eq( ent_t a, ent_t b )
{
    return a.index == b.index && a.gen == b.gen;
}

static inline bool
ent_valid( ent_t e )
{
    return e.index != 0;
}

#define ENT_INVALID  ( ( ent_t ){ 0, 0 } )

/*==============================================================================================
    Component pool record  (internal layout, public type -- world_t embeds it)

    All storage lives in world_t.pool_mem and is addressed by byte offset, never by
    pointer, so the whole world block is trivially relocatable and reload-safe.
==============================================================================================*/

typedef struct world_comp_s
{
    u32   name_hash;      // ref_hash_str of the component type name -- identity across rebinds
    sid_t name;           // interned type name for diagnostics
    u32   schema_hash;    // ref schema at carve time; drift on rebind wipes the pool
    u32   cap;            // max live instances (fixed at first registration)
    u32   count;          // live instances -- the dense prefix
    u16   type_id;        // ref type id -- re-resolved by every comp_register call
    u16   elem_size;      // sizeof(component) from the ref record
    u32   data_bytes;     // carved data block size; a grown struct that no longer fits kills the pool
    u32   sparse_off;     // pool_mem: u32[WORLD_MAX_ENT+1]  ent index -> dense slot
    u32   dense_off;      // pool_mem: u32[cap]              dense slot -> ent index
    u32   data_off;       // pool_mem: elem_size*cap bytes   dense slot -> component data

} world_comp_t;

/*==============================================================================================
    World block

    One flat POD struct.  Embed it in module state (or a static for a sandbox) and bind it;
    the framework never allocates.  Zero-initialization is a valid empty world.
==============================================================================================*/

#define WORLD_MAGIC  0x574F524Cu    /* 'WORL' -- distinguishes fresh storage from a rebind */

/* C4324: the pool arena's 16-byte alignment pads the struct tail -- intended, not a defect. */
#pragma warning( push )
#pragma warning( disable : 4324 )

typedef struct world_s
{
    u32 magic;                            // WORLD_MAGIC once initialized
    u32 ent_count;                        // live entities
    u32 ent_high;                         // highest index ever allocated (1-based)
    u32 ent_free_count;                   // entries in ent_free
    u32 comp_count;                       // registered component types
    u32 pool_used;                        // bytes carved from pool_mem

    u32 ent_gen[ WORLD_MAX_ENT + 1 ];     // per-slot generation; monotonic, survives reset
    u8  ent_live[ WORLD_MAX_ENT + 1 ];    // 1 = alive
    u64 ent_mask[ WORLD_MAX_ENT + 1 ];    // attached components; bit index == comp_id
    u32 ent_free[ WORLD_MAX_ENT ];        // recycled index stack

    world_comp_t comps[ WORLD_MAX_COMP ];

    ORB_ALIGNAS( 16 ) u8 pool_mem[ WORLD_POOL_BYTES ];    // all pool storage, offset-addressed

} world_t;

#pragma warning( pop )

/*==============================================================================================
    World lifecycle
==============================================================================================*/

/* Bind storage into a world slot.  Fresh storage is initialized; already-initialized
   storage (hot-reload, repeated init) is rebound in place with entities intact.
   Call in BOTH init() and reload().  Returns slot as the world id, WORLD_INVALID on error. */
world_id_t world_bind( i32 slot, world_t* storage );

/* Destroy all entities and empty every pool.  Component registrations survive; outstanding
   entity handles are invalidated (generations keep counting).  The scene-load primitive. */
void world_reset( world_id_t w );

u32 world_ent_count( world_id_t w );

/*==============================================================================================
    Entities
==============================================================================================*/

ent_t ent_create ( world_id_t w );                 // ENT_INVALID when full
void  ent_destroy( world_id_t w, ent_t e );        // detaches every component, recycles the slot
bool  ent_alive  ( world_id_t w, ent_t e );        // false for stale or zeroed handles

/*==============================================================================================
    Components

    comp_register is idempotent by type name: the first call carves the pool, later calls
    (reload re-registration) re-resolve the ref type and return the same id.  If the struct
    layout changed across a hot-reload (schema hash drift) the pool is wiped with a warning
    -- entities persist, that component's data restarts zeroed.
==============================================================================================*/

comp_id_t comp_register( world_id_t w, const char* ref_type_name, u32 cap );
comp_id_t comp_find    ( world_id_t w, const char* ref_type_name );

void* comp_attach ( world_id_t w, ent_t e, comp_id_t c );    // zeroed data; existing data if already attached
void  comp_detach ( world_id_t w, ent_t e, comp_id_t c );
void* comp_get    ( world_id_t w, ent_t e, comp_id_t c );    // NULL if absent, stale, or invalid
bool  comp_has    ( world_id_t w, ent_t e, comp_id_t c );
u32   comp_count  ( world_id_t w, comp_id_t c );             // live instances in the pool
u16   comp_type_id( world_id_t w, comp_id_t c );             // ref type id, for inspectors/serializers

/*==============================================================================================
    Queries

    Linear walk of one component's dense pool; "with B" is a comp_get probe per entity.
    Iteration order is newest-attached first (the walk runs high slot -> low) and is
    deterministic for a deterministic operation history.

    Mutation rules while iterating:
      - detaching the CURRENT entity's iterated component (or destroying the entity) is safe
      - components attached during the loop are not visited this pass
      - it.data is transient: read/write inside the loop, never store it

        for ( world_iter_t it = world_query( w, c_move ); world_iter_next( w, &it ); )
        {
            move_t*      m = it.data;
            transform_t* t = comp_get( w, it.ent, c_transform );   // probe for "and B"
            if ( !t ) continue;
            ...
        }
==============================================================================================*/

typedef struct world_iter_s
{
    ent_t     ent;     // current entity  (valid after world_iter_next returns true)
    void*     data;    // current component data -- transient, do not store
    comp_id_t comp;    // internal: iterated component
    u32       slot;    // internal: dense cursor, walks count -> 0

} world_iter_t;

world_iter_t world_query    ( world_id_t w, comp_id_t c );
bool         world_iter_next( world_id_t w, world_iter_t* it );

/*============================================================================================*/
#endif    // WORLD_H
