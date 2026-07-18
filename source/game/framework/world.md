# The ORB World Framework -- a tutorial

`source/game/framework/` -- the engine's core simulation data structure: entities,
components, and queries over a reflected world.  This is the keystone of the roadmap
(ARCHITECTURE_NORTH_STAR.md section 3): every editor panel, scene file, prefab, save
game, and replicated snapshot is a *view of this one structure*.

Test bed and living reference: `source/sandbox/game/sb_world/sb_world.c`.

```
world.h        the whole public surface: ent_t, world_id_t, comp_id_t, world_t, queries
world.c        bound-world table, entity alloc/free, generation recycling
comp.c         component registry: ref type -> pool, attach/detach/get, queries
pool.c         dense sparse-set storage (internal)
framework.c    unity unit -- include once from the owning target
```

---

## 1. The mental model: the world is a database

Forget objects.  The world is a tiny in-memory relational database:

- an **entity** is a row id -- it *is* nothing and *has* no behavior; it is a stable
  key that components hang off
- a **component** is a table -- one dense array of identical POD structs, one row per
  entity that has one
- a **system** is a query loop -- a plain function that walks one table and joins
  against others by key

```
              sbw_pos_t pool          sbw_vel_t pool          sbw_info_t pool
   ent 7  ->  { 10.0, 3.0 }           { 1.0, 0.0 }            { "player", parent=0 }
   ent 12 ->  { -4.0, 8.0 }              (none)               { "crate",  parent=7 }
   ent 3  ->  {  0.0, 0.0 }           { 0.0, 2.0 }               (none)
```

An entity's "shape" is just the set of tables it appears in, and that set can change
at any moment at runtime (`comp_attach` / `comp_detach`).  There is no class
hierarchy to design up front and no wrong place for data to live.

## 2. Everything is an id

The engine is id-focused, and the world doubles down on it:

| Id           | What it is                              | Invalid value    |
|--------------|-----------------------------------------|------------------|
| `world_id_t` | slot in the framework's bound table     | `WORLD_INVALID`  |
| `ent_t`      | `{ index, generation }` handle          | `ENT_INVALID`    |
| `comp_id_t`  | registration index within a world       | `COMP_INVALID`   |
| `sid_t`      | interned string (names)                 | `SID_INVALID`    |
| `u16` ref id | the component's type in the ref registry| `REF_TYPE_INVALID` |

Pointers exist only as **transient access**: `comp_get` / `it.data` hand you the
component's bytes so you can read and write them *right now*, inside the current
call or loop.  You never store one -- across a frame the pool may have swap-moved
the data; across a hot-reload the whole block may sit at a new address; in a scene
file a pointer is meaningless.  Ids survive all three.  This is the same contract
as `asset_id_t` and the gui's ids, applied to gameplay state.

### Why { index, generation }?

A bare index lies: destroy entity 7, create a new one, and any stored index 7 now
silently points at a stranger (the "ABA problem").  The generation counter catches
it -- each slot remembers how many times it has been recycled, and a handle carries
the generation it was created with.  `ent_alive()` compares the two:

```
    ent_t e = ent_create( w );      // { index 7, gen 2 }
    ent_destroy( w, e );             // slot 7 bumps to gen 3
    ent_t n = ent_create( w );      // { index 7, gen 3 }  -- slot reused
    ent_alive( w, e );               // false: 2 != 3.  Never a false positive.
```

Dead references are therefore *detectable, not dangerous* -- the difference between
"the AI's target quietly became a health pickup" and "the AI's target reads as gone,
pick a new one."  Handles can be stored in components, serialized, replicated, and
held across frames with no lifetime protocol at all.

## 3. Components are described once, by ref_

A component is any POD struct the reflection registry knows:

```c
    REF_STRUCT()
    typedef struct transform_s
    {
        REF_PROP() f32 x, y;
        REF_PROP() f32 angle;
    } transform_t;

    comp_id_t c_tf = comp_register( w, "transform_t", 512 );
```

`comp_register` asks ref_ for everything it needs -- size for the pool stride,
schema hash for reload compatibility -- so the framework contains **zero
per-component code**.  That is the entire point.  One description fans out into
every data-driven feature the engine will grow:

```
    REF_STRUCT( transform_t ) --+--> pool layout          (this framework)
                                +--> inspector panel      (editor walks ref fields)
                                +--> scene save/load      (ref serializer, M3)
                                +--> prefab + overrides   (ref walk diff)
                                +--> net replication      (ref delta x net channels)
                                +--> save games           (same serializer)
```

`sb_world.c` demonstrates the last-mile proof: a live component round-trips through
`ref_write` / `ref_read` using only its `comp_type_id` -- no hand-written
serialization, and the same call will work for every component ever added.

## 4. Storage: dense pools (the sparse set)

Each component type owns three offset-addressed blocks inside the world's arena:

```
    sparse[ent index] -> dense slot     "does entity E have one, and where"  O(1)
    dense[slot]       -> ent index      the packed owner list
    data[slot]        -> struct bytes   the packed component array
```

Attach appends to the packed end; detach swap-moves the last element into the hole.
The live region is therefore always a contiguous prefix -- iteration is a linear
walk over cache-friendly memory with no holes, no branches, no indirection per
element.  Lookup by entity stays O(1) through the sparse table.

What we deliberately did NOT build:

- **No archetypes.**  Archetype ECS (Unity DOTS, flecs) groups entities by their
  exact component *set* so multi-component queries iterate perfectly packed chunks.
  The price: adding/removing one component moves the entity's entire data between
  archetypes, and the machinery (chunk management, query caching, structural-change
  buffers) is a subsystem of its own.  ORB queries iterate the smaller pool and
  probe the other -- O(1) per probe, zero machinery.  If a profile ever proves this
  wrong, archetypes can hide behind the same query verbs.
- **No system scheduler.**  Systems are plain functions the project calls, in
  source order, from `on_sim`.  Execution order is readable top to bottom in one
  place, which is what keeps the deterministic-sim guarantee legible.

## 5. Queries and systems

```c
    static void
    sys_move( world_id_t w, comp_id_t c_vel, comp_id_t c_pos, f32 dt )
    {
        for ( world_iter_t it = world_query( w, c_vel ); world_iter_next( w, &it ); )
        {
            vel_t* v = it.data;
            pos_t* p = comp_get( w, it.ent, c_pos );    // the join: probe by key
            if ( !p )
                continue;
            p->x += v->dx * dt;
            p->y += v->dy * dt;
        }
    }
```

Conventions that make this safe and deterministic:

- **Iterate the rarer component** when joining ("for each vel, get pos" -- there are
  fewer movers than things with positions).
- The cursor walks the dense prefix **top down**, so swap-remove of the *current*
  element is safe: destroying `it.ent` or detaching its iterated component mid-loop
  neither skips nor revisits anything.  (An already-visited element is what moves
  into the vacated slot.)
- Components attached during a loop are not visited that pass.
- Iteration order is "newest attached first" and is a pure function of the
  operation history -- run the same sim ticks, get the same order.  That is what
  `on_sim` determinism needs; no sorting, no ids-as-priorities.

## 6. Names and parenting are components, not world features

The world core knows nothing about names, tags, or hierarchy.  Identity data rides
in an ordinary component (see `sbw_info_t` in the sandbox):

```c
    typedef struct info_s
    {
        sid_t name;      // interned string id -- compare by value, print via sid_cstr
        ent_t parent;    // hierarchy edge: a stored handle, ENT_INVALID = root
    } info_t;
```

**Names** are `sid_t`, not char buffers: 4 bytes in the pool, O(1) equality, one
shared copy of the actual string in core's intern table.  (Session-local caveat:
a sid does not serialize as a string by itself -- the M3 scene serializer will
write names out canonically.)

**Parenting** needs the longer explanation, because it is where this design differs
most from a traditional scene graph:

- The hierarchy is nothing but *each child storing one `ent_t` to its parent*.
  There is no tree structure anywhere -- no child lists, no sibling links, no
  ordered node pool.  The "tree" is derived whenever someone needs it: the editor's
  hierarchy panel builds its view by iterating the info pool and grouping by
  parent; a transform system resolves a child's world matrix by walking parent
  handles upward.
- Destroying a parent does not touch its children.  Their stored `parent` handle
  simply goes stale -- `ent_alive()` says false, and it can never dangle into
  recycled memory.  *Policy* (orphan to root? destroy the subtree? reattach?) is a
  system's decision, written in gameplay code where it belongs, not a hard-coded
  world behavior.
- Cost model: walking up is a few O(1) probes; enumerating children is a pool scan.
  For a game with thousands of entities that scan is nothing; when a real scene
  proves it matters, a derived child-list acceleration can be built *beside* the
  authoritative parent handles without changing any data.

Contrast: in Unity/Unreal the hierarchy is load-bearing infrastructure -- transforms
compose through it, lifetime cascades through it, and every object pays for it
whether it needs it or not.  Here an entity with no `info_t` costs nothing, and the
1024 bullets in the bullet pool do not have names, parents, or a place in anyone's
tree.

## 7. The hot-reload story (why world_t looks the way it does)

`world_t` is one flat POD block: fixed-capacity arrays, an 8 MB offset-addressed
arena, and **not a single pointer**.  That shape is not aesthetic -- it is what
makes the whole persistence story free:

- Embedded in a module's system-owned state block, the world *survives hot-reload*
  because the module system preserves the block and nothing in it can go stale.
- The two things that CAN go stale after a reload live outside the block and are
  rebuilt by contract: the framework's slot table (DLL statics reset) and ref type
  ids (the module's ref frame is popped and re-pushed).  Hence the owner's rule:

```c
    /* in BOTH init() and reload(): */
    s->w = world_bind( 0, &s->world );            // re-point the slot table
    s->c_tf = comp_register( s->w, "transform_t", 512 );   // re-resolve ref, same ids back
```

- If a component struct's *layout* changed across the reload, the schema hash
  catches it: that one pool is wiped with a warning, every other component and all
  entities carry on.  Change behavior freely mid-play; change data layouts and only
  the changed data resets.

The same flatness is what makes `world_reset` (the scene-load primitive), fixed
memory budgets, and eventually snapshot/rollback trivially correct.

## 8. Contrast with the traditional approaches

**OOP actor hierarchies** (classic Unreal-style): entity = class instance, behavior
= virtual methods, composition = inheritance.  Strengths: discoverable, one object
one place.  Weaknesses ORB avoids: the diamond problem ("is an amphibious vehicle a
Boat or a Car"), data scattered across heap allocations (cache-hostile), lifetime
via pointers (dangling references), and per-type serialization/inspection code.

**Unity GameObject/MonoBehaviour**: composition without inheritance -- close in
spirit -- but components are managed objects addressed by reference, iteration
means virtual `Update()` per behavior per object, and the scene graph is
mandatory.  ORB keeps the composition model and swaps the substrate: POD in dense
arrays, one loop per system instead of N virtual calls, hierarchy opt-in.

**Archetype ECS** (Unity DOTS, flecs, EnTT groups): same data-oriented goals,
maximal iteration speed for multi-component queries.  ORB deliberately stops one
step earlier (sparse sets + probes) because the archetype machinery is a large
permanent complexity tax and the engine's honest bottleneck is rendering, not
entity iteration.  The query verbs are the seam: if that call is ever wrong, the
storage can change without touching a caller.

**Honest weaknesses of the ORB design**, accepted on purpose:

- Fixed caps: 4096 entities, 64 component types, 8 MB pool arena per world.  Hard
  walls -- but visible, budgeted, one-line raises, and the reason reload safety is
  free.  (Constraint, not conviction: growable storage can come later behind the
  same API.)
- Multi-component joins probe per entity instead of iterating packed pairs --
  slower than archetypes at very large scale.
- One u64 mask caps component types at 64 per world.
- Per-pool sparse tables cost 16 KB each regardless of population.
- No queries like "entities with A but not B" yet -- write the probe yourself; the
  per-entity mask exists when a mask-filter helper earns its place.
- Systems have no automatic ordering or parallelism -- you write the call list.
  (In ORB, that one is a feature.)

## 9. Quick start

```c
    static world_t s_world;                              // or a field in module state

    world_id_t w     = world_bind( 0, &s_world );        // in init() AND reload()
    comp_id_t  c_pos = comp_register( w, "pos_t", 512 ); // type must be in the ref registry

    ent_t e = ent_create( w );
    pos_t* p = comp_attach( w, e, c_pos );               // zeroed; write through it now
    p->x = 10.0f;

    for ( world_iter_t it = world_query( w, c_pos ); world_iter_next( w, &it ); )
        ( ( pos_t* )it.data )->x += 1.0f;

    ent_destroy( w, e );                                 // detaches everything, handle goes stale
    world_reset( w );                                    // scene teardown; registrations survive
```

Build and run the full reference: `bin\build_tool.exe -config Debug -target sb_world && bin\sb_world.exe`.
