# ORB -- where the engine stands

A C11 game engine: hot-reload-first, modular, data-oriented.  Windows + Vulkan today; the
POSIX paths are kept as working code.  This directory is deliberately small:

    README.md    this file: the invariants, the stack as it exists, and what comes next
    CONTENT.md   resource names, content roots, the cook phase, the asset service, shipping
    SHADERS.md   shader_tool, the .oshd container, reflection-driven pipelines
    GUI.md       the gui service from the outside in

Deeper references live beside the code they describe: `CLAUDE.md` (build, conventions),
`source/runtime_service/gui/GUI_ARCHITECTURE.md`, `source/game/framework/world.md`,
`source/engine/ref/ref.md`.  Code is the source of truth; when a document and the tree
disagree, fix the document.

---

## 1. Invariants

Every system obeys these, and every new one must.

1.  **One loop.**  `run_host_main()` is the frame loop.  A host is a policy shell: a
    descriptor, a module list, a handful of callbacks.  Nobody else owns a loop.
2.  **Everything is a module.**  One lifecycle contract (`mod_desc_t`: init / reload / exit,
    system-owned zeroed state, dep-ordered boot).  Static service or hot-reload DLL is a
    build decision; call sites are identical (`render()->begin_frame()`) in both modes.
3.  **Down only.**  Lower layers never include, link, or know about higher ones.  Sideways
    calls go through a module API struct.  Upward calls do not exist: the higher layer
    polls or registers a callback.
4.  **Data first.**  State is POD in system-owned blocks; identity is a `{ index, generation }`
    handle (`ent_t`, `aid_t`, `win_id_t`, gui ids); memory is arenas; strings are interned
    SIDs where they are keys.  Pointers do not cross frames or reload boundaries.
5.  **Described once.**  A struct annotated with `REF_STRUCT` / `REF_PROP` is visible to the
    reflection registry, and everything derivable from the description (printing,
    serialization, inspection, pool layout) is derived, never hand-written per type.
6.  **Complexity lives behind a vtable.**  Vulkan bootstrap, WASAPI threading, DLL swap
    protocols, VFS/zip, packet reliability: each is wrapped by a small vtable whose caller
    side is a handful of verbs.  User space sees descriptors, handles, and vtables.
7.  **The project contract is runtime-level.**  A project DLL implements the six functions
    of `runtime/run_project.h` (on_start / on_sim / on_frame / on_draw / on_hud / on_stop).
    Nothing forces the game framework on it.  Play state advances only in
    `on_sim( fixed_dt )`; `on_hud` is the only phase where gui calls are legal.

---

## 2. The stack

```
   TOOLS       build_tool  reflect_tool  res_tool  asset_tool  shader_tool  font_tool
   (offline)   image_tool  ship_tool  launch_tool (gui project manager / command hub)
  ================================================================================
   T6 PROJECT  sample_game (living reference)   template_game (source of -create)
  ---- force-dynamic DLL implementing run_project.h ------------------------------
   T5 EDITOR   editor: menu bar + dockspace + Viewport / Game / Frame Stats / Deploy windows
               editor_service/viewport (scene render target + view camera + ray pick)
               editor_modules/editor_example (stub)
  --------------------------------------------------------------------------------
   T4 GAME     game runner: project bind, play/stop/pause/step, fixed-step clock, tick, hud
               game/framework: the world -- entities, ref-described components, dense
               pools, queries; survives hot-reload inside module state (world.md)
               game_service/nav (stub)   game_modules/game_example (stub)
  --------------------------------------------------------------------------------
   T3 RUNTIME  render (per-context frame, rect replay through draw, offscreen targets)
      MODULES  audio   example   physics (stub)   animation (empty)
  ---- hot-reload DLLs, one mod_desc_t each --------------------------------------
   T2 RUNTIME  run (the host loop)   rhi (Vulkan, bindless)   draw (2d/3d batches)
      SERVICES gui (see GUI.md)   asset   input   console   ahi (WASAPI)
  ---- static libs with a mod_desc, linked into the host -------------------------
   T1 ENGINE   mod  sys  ref  res (header-only)  prof  pack  fs  job  net  core  app
  ---- host-only statics, never inside a DLL -------------------------------------
   T0 BASE     math (vec/mat/quat/geo/ease/rng)  str/str_buf/str_arena  utf8  fmt
               mem  bit  char  container  test
  ---- stateless, no globals, links into the host AND every DLL ------------------

   HOSTS       host_game (shipping shape)   host_editor (dev shape)   host_common
   DEVELOPER   dev_build  dev_hot  dev_font  dev_image  dev_vector  dev_ship
   SANDBOXES   sb_<layer> per engine library, sb_gui_* (18), sb_asset_*, sb_world,
               sb_host_*, sb_vulkan* -- run them to validate; there is no test framework
```

Allowed dependency edges:

```
   project --> game runner --> run + services --> engine --> base
      |            |                |
      |            +--> (via run_view_t, versioned, passed every call)
      +--> render()/core() module APIs -- never a direct link
   editor  --> gui/draw/asset/ref + game runner (session verbs only)
   hosts   --> everything below them, by name, intentionally
   tools   --> sys + base (+ pack) only; no engine runtime
```

The shape in one breath: the bottom half (T0-T2) is a complete platform -- window, GPU,
audio, input, jobs, net, VFS, resource names, cooking, shipping, reflection, hot-reload,
GUI.  The world exists but nothing serializes it.  The editor is a real shell with
play-in-editor, but panels, selection and undo are not a framework yet.  Physics and
animation are reserved names.

---

## 3. What comes next

In dependency order.  Each item ships something runnable.

- **Scenes as assets.**  A scene file written by the ref serializer; a "scene" asset type;
  world load/save verbs.  host_game boots into a scene instead of code-spawned entities.
- **Editor framework.**  A panel registry, a selection set (`ent_t` + `aid_t`), and a
  command/undo stack in the editor lib; hierarchy, inspector (ref walk), asset browser and
  profiler panels; gizmos over the existing ray pick.  Editor tools ship as
  editor_modules DLLs, hot-reloaded like gameplay.
- **Simulation modules.**  render grows mesh + material + camera over the draw scene pass;
  physics runs fixed-step from `on_sim` over world pools; animation lands after meshes.
- **Ship and gateway.**  launch_tool grows create / dev / run / ship verbs over build_tool,
  the hosts and ship_tool; a test_tool runs the sb_* suite and parses PASS/FAIL; POSIX
  bring-up gets a test bed.
- **Explored, not started.**  A WYSIWYG UI layout editor over the gui (the document is a
  flat POD array so undo is a memcpy; design mode is a flag on one interpreter, never a
  second widget set; every gesture edits a layout unit, never free pixels).  C as the
  scripting language (`content/**.c` compiled on save by build_tool and hot-swapped with
  ref-described instance state; gated on a sandbox proving sub-second save-to-swap).

---

## 4. What does not get built

- **No scripting language.**  C hot-reload of the project DLL is the scripting story.
- **No archetype ECS, no system scheduler.**  Dense pools + explicit system calls in
  `on_sim`, readable top to bottom.
- **No render graph framework.**  Named passes inside the render module; rhi stays a thin
  explicit vtable.
- **No plugin marketplace machinery.**  Module tiers + `build_tool -create` cover extension.
- **No cross-layer convenience includes.**  `orb.h` is for hosts and projects, not for a
  layer to peek upward.
- **No second GUI toolkit.**  The in-house gui is the only UI, editor included.
- **No runtime resource catalogue.**  Names are strings; the build proves them (CONTENT.md).

---

## 5. Fit test for new work

1. Which tier does it live in, and does it depend only downward?
2. Is its state POD in a system-owned block, addressed by generation handles?
3. Is its user-visible data described by ref_?
4. Does user space touch it through a descriptor, a handle, or a vtable verb, and nothing
   else?

If a proposal cannot answer cleanly, reshape the proposal, not the architecture.
