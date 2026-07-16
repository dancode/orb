# ORB -- Architecture North Star

A model of the full stack as it exists today, and the target shape of the finished
engine. This is the document to refactor toward: when a new system lands, it should
drop into one of the slots below without inventing a new kind of thing.

Status legend used throughout:

    [SOLID]    built, campaign complete, validated by a sandbox
    [PARTIAL]  real code, incomplete surface
    [STUB]     placeholder file(s), no behavior
    [EMPTY]    directory reserved, nothing in it

---

## 1. The creed (what ORB already is)

These invariants are the engine's identity. Every current system obeys them, and every
future system must. They are the reason the stack stays simple.

1.  **One loop.** `run_host_main()` is THE frame loop. Hosts are policy shells: a
    descriptor, a module list, and a handful of callbacks. Nobody else owns a loop.
2.  **Everything is a module.** One lifecycle contract (`mod_desc_t`: init / reload /
    exit, system-owned zeroed state, dep-ordered boot). Static service or hot-reload
    DLL is a build decision, not an architecture decision -- call sites are identical
    (`render()->begin_frame()`) in both modes.
3.  **Down only.** Lower layers never include, link, or know about higher ones.
    Sideways calls go through a module API struct. Upward calls do not exist --
    the higher layer polls or registers a callback.
4.  **Data first.** State is POD in system-owned blocks; identity is a
    `{ index, generation }` handle (asset_id_t, win_id_t, gui ids); memory is arenas;
    strings are interned SIDs where they are keys. Pointers do not cross frames or
    reload boundaries.
5.  **Described once.** A struct annotated with `REF_STRUCT`/`REF_PROP` is visible to
    the reflection registry, and everything that can be derived from the description
    (printing, serialization, inspection, diffing, replication) is derived -- never
    hand-written per type.
6.  **Complexity lives behind the seam.** Vulkan bootstrap, WASAPI threading, DLL
    swap protocols, VFS/zip, packet reliability -- each is wrapped by a small vtable
    whose caller-side surface is a handful of verbs. User space sees descriptors,
    handles, and vtables. Nothing else.
7.  **The contract is runtime-level.** A project DLL implements five functions
    (`run_project.h`: on_start / on_sim / on_frame / on_draw / on_stop) and nothing
    forces the game framework on it. Determinism boundary is explicit: play state
    advances only in `on_sim( fixed_dt )`.

---

## 2. The stack today (bottom to top)

```
                                   TOOLS (offline exes)
   build_tool [SOLID]  reflect_tool [SOLID]  shader_tool [SOLID]  asset_tool [SOLID]
   font_tool [SOLID]   launch_tool [EMPTY]   package_tool [EMPTY] test_tool [EMPTY]
  ================================================================================
   T6  PROJECT      sample_game [SOLID]   template_game [SOLID]   my_project (child)
  ---- force-dynamic DLL, implements run_project.h ------------------------------
   T5  EDITOR       editor lib [STUB]     editor_service/viewport [PARTIAL]
                    editor_modules/editor_example [STUB]
                    (the real editor shell currently lives in sb_gui_editor)
  --------------------------------------------------------------------------------
   T4  GAME         game runner DLL [SOLID]   game/framework (world) [EMPTY]
                    game_service/nav [STUB]   game_modules/game_example [STUB]
  --------------------------------------------------------------------------------
   T3  RUNTIME      render [PARTIAL: rect replay + scene pass]   audio [SOLID]
       MODULES      physics [STUB]   animation [EMPTY]   example [SOLID]
  ---- hot-reload DLLs, one mod_desc_t each --------------------------------------
   T2  RUNTIME      run (the host loop) [SOLID]
       + SERVICES   rhi/Vulkan [SOLID]  draw [SOLID]  gui [SOLID, large]
                    asset [SOLID]  input [SOLID]  ahi/WASAPI [SOLID]
  ---- static libs, module lifecycle, linked into the host -----------------------
   T1  ENGINE       mod [SOLID]  sys [SOLID]  ref [SOLID]  prof [SOLID]
                    fs [SOLID: mounts/zip]  job [SOLID]  net [SOLID: channels/handshake/frag/sim]
                    core [SOLID: arena/log/cvar/cmd/console/config/sid/debug] -- top of T1
                    app [SOLID]
  ---- host-only statics, never inside a DLL -------------------------------------
   T0  BASE         math/rng [SOLID]  str/str_buf/str_arena [SOLID]  mem/bit/char
                    [SOLID]  test [SOLID]
  ---- stateless, no globals, links into host AND every DLL ----------------------

   HOSTS (orthogonal column, one per product shape):
       host_game [SOLID]  host_editor [SOLID]
       host_common (launch params, project resolve) [SOLID]
```

Dependency flow (allowed edges only):

```
   project --> game runner --> run + services --> engine --> base
      |            |                |
      |            +--> (via run_view_t, versioned, passed every call)
      +--> render()/core() module APIs -- never a direct link
   editor  --> gui/draw/asset/ref + game runner (session verbs only)
   hosts   --> everything below them, by name, intentionally (no generic iteration)
   tools   --> sys + base only (offline; no engine runtime)
```

The load-bearing observations:

- **The bottom half is finished.** T0-T2 are a genuinely complete platform: window,
  GPU, audio, input, jobs, net, VFS/assets, reflection, hot-reload, GUI. Campaigns
  there are polish, not construction.
- **The middle is hollow.** T4 has a session state machine but no world. There is no
  entity, no component, no scene file. Every project must invent its own state layout.
- **The editor is an illusion.** host_editor is a real shell with Play/Stop/Pause/Step,
  but the editor *framework* (panels, selection, undo, inspector) is a placeholder;
  the accumulated editor knowledge sits in a sandbox (sb_gui_editor).
- **The ship path stops at the dev machine.** -create makes a project; nothing yet
  packages one. launch_tool / package_tool / test_tool are reserved names.

---

## 3. The keystone: a reflected world

The single highest-leverage move in the whole roadmap. Everything the engine is
missing at the top converges on one design: **the world is data described by ref_.**

```
   source/game/framework/          (unity: joins the game runner DLL's state)
       world.h      world_t, ent_t { u32 index; u32 gen; }, queries
       world.c      entity alloc/free, generation recycling
       comp.h/.c    component registry: type = ref type id + pool
       pool.c       per-type chunked SoA storage, dense iteration
       scene.h/.c   scene = serialized world slice (ref_serialize)
```

Design rules, mirroring what already works elsewhere in ORB:

- `ent_t` is `{ index, generation }` -- the asset_id_t idiom, stale-safe, pass by value.
- A component is a `REF_STRUCT`-annotated POD. Registering it takes one line; the pool
  layout, size, and field metadata come from the reflection registry.
- Storage is per-component dense pools (SoA chunks), iterated linearly. No archetype
  machinery -- queries are "for each entity with A [and B]" over the smaller pool.
  If profiling ever demands archetypes, they hide behind the same query verbs.
- The world lives inside the game module's system-owned state block, so **the world
  survives hot-reload for free** -- same guarantee sample_game already demonstrates.
- Systems are plain functions called from the project's `on_sim` / `on_frame`. The
  framework does not schedule them; the project (or a default scaffold) does. No
  dependency solver, no system graph -- determinism stays legible.

Why this is the keystone -- one description, five features:

```
   REF_STRUCT( transform_t ) ----+--> inspector panel      (editor walks ref fields)
                                 +--> scene save/load      (ref_serialize -> .oscn)
                                 +--> prefab + overrides   (ref walk diff vs source)
                                 +--> net replication      (net channels x ref delta)
                                 +--> save games           (same serializer, vfs path)
```

None of those five systems gets hand-written per-type code. That is the data-oriented
payoff and the reason ref_ was built as a leaf module.

---

## 4. Target shape, layer by layer

### T0 base -- complete the stdlib
- Everything else stays as-is. base stays stateless; it is the only code shared
  verbatim by host and every DLL.
- Many systems use bespoke isolated no-dependency data-structures, like pool and index hashes.
  This is ok, and part of the engine design.

### T1 engine -- one addition, no reshaping
- **async file IO**: `sys` gains overlapped reads; `job` gains an IO-completion path.
  Consumed by asset streaming (ASSET_LOADING state is already reserved for it).
- **profiler**: DONE -- landed as the `prof` leaf library (fixed rings, id = SID,
  thread-local), drained by the gui timeline overlay.

### T2 runtime -- the platform is done; only render grows
- `run`, `rhi`, `draw`, `gui`, `asset`, `input`, `ahi` keep their current shapes.
- **render module graduates**: from rect replay to mesh + material + camera over
  the existing draw_scene pass. Scene submission stays behind `render()` verbs;
  rhi stays the only Vulkan speaker. The .oshd shader pipeline already covers the
  material story's hard half.
- **asset types grow with consumers**: mesh, material, scene, audio bank, prefab --
  each is just `type_register` + loader; the id/refcount/hot-reload machinery is done.

### T3 runtime modules -- fill the reserved slots
- **physics**: fixed-step, deterministic, called from `on_sim` only. Starts as 2D/3D
  primitives + sweep queries; a broadphase over world transform pools. Hot-reloadable.
- **animation**: sampling + blending over reflected pose components; lands after
  render has meshes.
- Both are consumers of the world (T4) via component pools -- they iterate data,
  they do not own entities.

### T4 game -- the world (section 3) plus services
- game runner keeps its exact contract (bind/play/stop/pause/step/tick).
- **replication** (game_service): net channels x ref-described components; the
  deterministic `on_sim` boundary is what makes rollback/lockstep even possible.
- **nav** fills in when a game needs it; the slot already exists.

### T5 editor -- framework in-tree, panels as modules
- `editor` static lib becomes the real framework, owning exactly four things:
  1. **panel registry** -- dock-aware panels register a name + draw fn (gui docking
     is done; this is a thin table over it)
  2. **selection** -- a set of ent_t + asset_id_t, published for any panel
  3. **command/undo stack** -- every mutation is a command (ref-described params,
     so undo data is serialized automatically)
  4. **gizmo layer** -- translate/rotate/scale over draw + viewport ray-pick
     (ed_viewcam + ray-pick already exist in editor_service/viewport)
- **Core panels**: hierarchy (world tree), inspector (ref walk of selection), asset
  browser (asset registry view), viewport (scene render target), console (exists),
  profiler (T1 zones).
- **Editor tools ship as editor_modules DLLs** -- the editor hot-reloads its own
  panels the same way the game hot-reloads gameplay. editor_example already models it.
- **Play-in-editor needs no new architecture**: the run_view_t seam was designed for
  it -- the editor hands the project a viewport rhi context instead of the main
  window's, and Play/Stop already drive the session.
- sb_gui_editor's shell knowledge (dockspace, layout save, chrome) migrates into
  editor + host_editor, and the sandbox goes back to being a testbed.

### T6 project -- the user gateway
The full outsider journey, end to end, with nothing else to learn:

```
   > orb create my_game            (launch_tool wrapping build_tool -create)
   > cd my_game ; orb dev          -> editor opens the project, F5 debugging works
       edit a component struct    -> hot-reload lands in-session, world intact
       Play / Pause / Step        -> session verbs, deterministic sim
   > orb run                       -> host_game plays it, no editor
   > orb ship                      -> dist/: exe + packed bundle, double-click runs
```

- A project is: an `orb.targets`, a `src/` with one DLL implementing five functions,
  and a `content/` dir. That is the whole mental model.
- `template_game` stays the canonical source; sample_game stays the living reference.

### Hosts -- two shapes, both real
- **host_game**: shipping shape. Monolithic build, packed VFS bundle, hot-reload and
  console compiled out. Already a policy shell; packaging makes it a product.
- **host_editor**: dev shape. Grows only by delegating to the editor framework.
- No generic tool/sandbox host. Each tool and each sb_* sandbox is its own
  standalone exe (build_tool makes a new target cheap) -- a shared dynamic-dispatch
  host was tried (host_tool, host_sandbox) and removed: the flexibility it bought
  wasn't worth the dependency-graph and dynamic-loading complexity for short-lived,
  build-time-known binaries.

### Tools -- close the ring
- **package_tool**: cook (asset_tool) + pack (zip bundle) + monolithic host_game +
  project DLL -> `dist/`. The ship command's engine.
- **launch_tool**: the `orb` front door -- create/dev/run/ship/doctor verbs that
  dispatch to build_tool, hosts, and package_tool. One binary for users.
- **test_tool**: builds and runs the sb_* suite, parses PASS/FAIL check output,
  one summary line. Turns the sandbox convention into CI.

---

## 5. What does NOT get built

Guard rails against nuance creep -- rejections are as architectural as additions:

- **No scripting language.** C hot-reload of the project DLL *is* the scripting
  story. One language, one debugger, one build.
- **No archetype ECS, no system scheduler.** Dense pools + explicit system calls in
  `on_sim`. Reorderable by reading the code top to bottom.
- **No render graph framework.** Named passes inside the render module; rhi stays a
  thin explicit vtable. Revisit only when a real scene proves the need.
- **No general plugin marketplace machinery.** Module tiers + `-create` cover
  extension. Every module is in-tree or in the project.
- **No cross-layer convenience includes.** engine_api.h / orb.h umbrellas are for
  hosts and projects, not for layers to peek upward.
- **No second GUI or tool UI toolkit.** The in-house gui is the only UI, editor
  included. Tools that need UI become hosts.

---

## 6. Roadmap (dependency order, each phase ships something runnable)

**M1 -- Foundations catch-up** *(unblocks everything, all small)*
- Delete dead wood: game/framework junk file.

**M2 -- The world** *(the keystone; sec. 3)*
- ent_t + pools + component registry over ref_; world in game-module state.
- sample_game rewritten as: components + two systems in on_sim + draw from pools.
- Exit test: hot-reload a component's behavior mid-play; entities persist.

**M3 -- Scenes as assets**
- .oscn via ref_serialize; "scene" asset type; world load/save verbs.
- Scene hot-reload = asset reload -> world re-instantiation (editor-triggered).
- Exit test: host_game boots into a scene file instead of code-spawned entities.

**M4 -- Editor framework**
- panel registry + selection + undo in editor lib; migrate sb_gui_editor shell.
- hierarchy + inspector (ref walk) + viewport panels; gizmos over ray-pick.
- Play-in-editor: run_view render_ctx -> viewport context.
- Exit test: select, edit a reflected field, undo, save scene, play it -- no restart.

**M5 -- Simulation modules**
- render: mesh/material/camera (asset-loaded, .oshd materials).
- physics MVP driven from on_sim over world pools; animation after meshes.
- profiler zones + timeline panel (feeds every later perf decision).

**M6 -- Ship + gateway**
- package_tool, then launch_tool (`orb` verbs), then test_tool over sb_*.
- POSIX bring-up (sys/app backends exist as working code; needs a test bed).
- Exit test: `orb create x && orb ship` on a clean machine produces a runnable dist.

Ordering rationale: M2 before the editor because every editor panel is a view of the
world; M3 before M4 because undo/prefabs/scene-save all ride the serializer; ship
last because it packages what exists rather than constraining it.

---

## 7. Fit test for new work

Before adding anything, it must answer all four:

1. Which tier does it live in, and does it depend only downward?
2. Is its state POD in a system-owned block, addressed by generation handles?
3. Is its data described by ref_ (if it has user-visible data at all)?
4. Does user space touch it through a descriptor, a handle, or a vtable verb --
   and nothing else?

If a proposal cannot answer cleanly, the proposal is reshaped, not the architecture.
