# ORB -- Addendum: C as the Scripting Language

Companion to `ARCHITECTURE_NORTH_STAR.md`. EXPERIMENTAL -- not critical path. The
north star's world/editor/ship phases do not depend on anything here; this explores
the user-space front end that could sit on top of them.

The premise: ORB will not grow a scripting language. Instead, **C source becomes a
data type**. A level ships with logic written in C; the engine compiles and loads it
on the fly like a script, with everything a native toolchain gives for free --
real debugging, native speed, zero-marshalling interop with engine and game APIs.
On release, scripts are packed as compiled native code: no compiler in the shipping
product, nothing loose to tamper with.

The reframe that makes this different from "just more modules": a script is not an
engine extension, it is **content**. It lives in the data domain (`content/`, vpaths,
the asset registry), is owned by levels and entities, exists in the hundreds, and is
authored against a deliberately *managed* surface -- the engine API for allocation
and object access, no system calls, no file IO, handles instead of raw pointers.
C used as a managed language, sandboxed by discipline to the engine ecosystem.

---

## 1. Why ORB can do this cheaply

Most engines that want this must build three hard systems first. ORB has all three:

1.  **Hot-reload DLL lifecycle with preserved state.** The mod system already owns
    zeroed state blocks that survive DLL swaps, dep-ordered init, and reload hooks.
    Scripts reuse the mechanism (not the registry -- see sec. 4).
2.  **A build tool that IS a compiler driver.** build_tool invokes cl.exe/link.exe
    directly, has a parallel scheduler, a content-hash skip system, and the seeded
    vcvars cache (`build\.vcvars_x64`) that lets it compile from any plain shell --
    which means *the editor can spawn compiles*. dev_build/dev_hot already do
    runtime rebuild + swap for sb_mod. The script compiler is a verb away, not a
    subsystem away.
3.  **Reflection.** Script state is `REF_STRUCT`-described by mandate, which buys
    state migration across reloads, inspector editing, and scene serialization of
    script instances -- the exact keystone leverage of the north star.

What scripting languages actually provide, and how this design covers each:

    fast iteration        -> compile-on-save of ONE small TU (sub-second), hot-swap
    safety/sandboxing     -> curated env vtable + no system includes (sec. 6)
    late binding to data  -> script = asset by vpath; entities reference it in scenes
    inspectable state     -> ref-described state blocks, editable like any component
    ship without sources  -> packs compile to native code linked into the product

---

## 2. What authoring looks like

A script is one `.c` file under `content/`, includes exactly one header, and speaks
only through the environment vtable it is handed. No other include compiles.

```c
    /* content/levels/e1/door_timer.c */
    #include "orb_script.h"                 /* the entire visible world */

    SCRIPT_STATE( door_timer_t )            /* REF_STRUCT under the hood */
    {
        REF_PROP() f32   open_after;        /* editable in inspector, saved in scene */
        REF_PROP() f32   elapsed;
        REF_PROP() ent_t door;              /* handle, never a pointer */
    };

    static void
    on_sim( script_ctx_t* ctx, door_timer_t* s, f32 fdt )
    {
        s->elapsed += fdt;
        if ( s->elapsed >= s->open_after )
            env( ctx )->ent_signal( ctx, s->door, SID( "open" ) );
    }

    static void
    on_event( script_ctx_t* ctx, door_timer_t* s, const script_event_t* ev )
    {
        if ( ev->id == SID( "reset" ) )
            s->elapsed = 0.0f;
    }

    SCRIPT_EXPORT( door_timer,
        .on_sim   = on_sim,
        .on_event = on_event );
```

The developer loop:

    save door_timer.c
      -> asset watch sees the change (existing asset hot-reload path)
      -> build_tool compiles the TU against the script SDK   (~100-300 ms)
      -> cache: bin/script_cache/door_timer-<hash>.dll + pdb
      -> script service swaps the unit; instance state blocks preserved
      -> every entity running door_timer continues with new code, same state

Same feel as an interpreted language with a good REPL, except the "interpreter" is
cl.exe and the debugger is Visual Studio with full PDBs. Breakpoints in script code
work because it is simply native code (build_hot already proved debugger-attached
swaps).

The contract is a scaled-down echo of `run_project.h` -- the same fractal the engine
already uses at every seam:

    host    drives project  via  on_start/on_sim/on_frame/on_draw/on_stop
    world   drives script   via  on_spawn/on_sim/on_frame/on_event/on_despawn

`on_sim` scripts obey the same determinism boundary as project `on_sim` -- and the
capability vtable enforces it structurally (sec. 6).

---

## 3. The three shapes of a script

One source form, three compiled shapes, chosen by context -- exactly the
static/dynamic gateway idiom (`MOD_GATEWAY_*`) lifted one level up. Call sites and
authoring never change.

**DEV -- per-script DLL.** One tiny TU per DLL, compiled on save, content-hash
cached. Maximum iteration granularity; a hundred loose DLLs in a cache dir is fine
for development.

**PACK -- the game-derived collection.** Scripts group by ownership into one DLL:
`content/levels/e1/**.c -> e1_scripts.dll`. Each script stays its own TU (file
statics never collide); a generated registry TU maps vpath -> descriptor. Packs are
the unit of *distribution* granularity: a mod, a level pack, a DLC. Dev builds can
also run from packs when iteration granularity is not needed (faster cold boot).

**SHIP -- linked native code.** package_tool compiles every script into the project
DLL (or the monolithic exe). The script registry resolves vpaths to linked-in
descriptors instead of loading anything. No compiler on the player's machine, no
loose code to tamper with, no load-time compile cost, LTO across the whole set.

The registry is the only component that knows which shape is live. Resolution is
always: vpath -> `script_desc_t*`.

---

## 4. Engine integration (where each piece lives)

```
    content/**.c                      script SOURCE -- data domain, vpath-addressed
        |
    asset service       [exists]     type "script": identity, refcount, hot-reload watch
        |
    script service      [new]        registry: vpath -> compiled unit -> script_desc_t
        |                            owns per-INSTANCE state pools; binds env vtables;
        |                            swap protocol + state migration
    world (T4)          [north star] script component = { asset_id, instance id };
        |                            dispatch groups by script type, iterates instances
    build_tool          [exists]     `-script <file>` compile verb + cache + scheduler
    package_tool        [north star] ship step: compile packs / link into product
```

- **Scripts are instanced; modules are singletons.** This is why scripts do not
  enter the module registry or its dep graph. One `script_desc_t` per script type;
  N state blocks per type, held in dense pools by the script service. Dispatch is
  data-oriented by construction: for each script type, run its instance pool
  linearly -- one icache-warm pass per behavior, not a virtual call per entity.
- **The world binds them.** A script component on an entity references the script
  asset and an instance slot. Scenes serialize the vpath plus the ref-described
  state block -- so script parameters are set in the inspector and saved in the
  scene like any component field. Spawning a scene instantiates its scripts;
  `on_spawn` fires with state already deserialized.
- **The dev compiler is a build_tool verb**, not an in-engine compiler. The editor
  (or asset watch) spawns `build_tool -script <src> -out <cache>`; the existing
  spawn/toolchain/vcvars machinery does the rest. dev_build/dev_hot generalize from
  "rebuild a known target" to "compile an arbitrary TU against the script SDK".
  Compiles run on job threads; the frame never blocks (old code keeps running until
  the swap lands).
- **Failed compiles are non-events.** Diagnostics route to the console/log with the
  script vpath; the last good DLL keeps running. The cache keeps the previous unit
  precisely so failure has a fallback (see cr.h prior art, sec. 8).

---

## 5. build_tool: what makes it viable

The difference between "clever hack" and "scripting system" is the toolchain UX.
All of it is extension, not invention:

- **`-script` verb**: compile one TU + link one DLL with a fixed, minimal command
  line: script SDK include dir ONLY, `/X` (ignore standard include paths), no
  default libs, exports exactly the descriptor symbol. Reuses compile/link/spawn.
- **Content-hash cache**: `script_cache/<name>-<srchash>.dll`. Editor cold boot
  compiles only what changed since last run; a clean project boots with zero
  compiles. (The skip system already works this way for targets.)
- **Precompiled `orb_script.h`**: one PCH shared by every script compile keeps the
  100-300 ms target honest even as the SDK header grows.
- **Parallel pack builds**: the scheduler already fans out; a pack of 200 scripts is
  200 small independent TUs -- near-perfectly parallel.
- **`-script-pack <dir>`**: compile all scripts under a root, generate the registry
  TU, link one DLL. package_tool calls this per pack at ship.
- **Doctor coverage**: `-doctor` gains script-SDK checks (cache dir writable,
  vcvars cache seeded) so "scripts won't compile" is diagnosable in one command.

MSVC stays the single compiler. A TCC-style instant-compile tier is tempting
(~10 ms compiles) but adds a second ABI, second codegen quality profile, and worse
debug info -- rejected until sub-second cl.exe is proven insufficient. One
toolchain, one debugger, one set of bugs.

---

## 6. The managed-C discipline

"Managed" is a *capability model*, not a VM. Three fences, in order of strength:

**Fence 1 -- the only door is the vtable.** `orb_script.h` defines types, the
`SCRIPT_*` macros, and the env API structs. Scripts cannot name an engine function,
an OS call, or malloc -- those symbols simply do not exist in their translation
unit, and the linker resolves nothing else. Allocation goes through
`env->alloc( ctx, ... )` (arena-backed, owned by the service, freed on despawn --
scripts cannot leak). Files, if ever needed, go through a vpath read capability.
Compiled with `/X` and linked with no default libs, `#include <windows.h>` and
`fopen` are compile/link errors, not policy violations.

**Fence 2 -- context-split capabilities.** The env handed to `on_sim` is the SIM
table: deterministic verbs only (world queries, signals, spawn requests, fixed
time). No wall clock, no surface size, no input polling -- those live only on the
FRAME table handed to `on_frame`. Determinism is enforced by what is *reachable*,
not by review. (Same trick as run_project.h keeping the view out of on_sim, made
structural.)

**Fence 3 (optional) -- the strict subset.** A lint tier for teams that want it:
no pointer declarations in state (handles/indexes only -- pointers in *locals* are
fine and unavoidable), no statics outside `SCRIPT_STATE`, no recursion beyond a
depth bound. Enforcement is a reflect_tool-style scan pass (the lexer
infrastructure exists), run on save in dev and as a pack-build gate. This is a
*dialect*, not a new language -- strict scripts are ordinary C that also passes
the lint.

Honesty clause: **native code cannot be truly sandboxed in-process.** A malicious
script can inline-asm its way to a syscall; fences 1-3 are anti-footgun and
API-discipline, not a security boundary. The actual trust model is the same as
every native-DLL engine: dev-time scripts are first-party content; shipped scripts
are compiled into the product by the packager. Third-party *untrusted* script
distribution is explicitly out of scope (that is what real VMs are for).

---

## 7. Pitfalls and their answers

**ABI drift between engine and scripts.** The env vtable is versioned and
append-only (the run_view_t rule); `script_desc_t` carries the SDK version + the
ref schema hash of its state. Mismatch = unit rejected at load with a clear log
line, old unit keeps running. Never a mystery crash.

**State layout changes across hot-reload.** The mod system punts (restart on size
change). Scripts can do better *because state is ref-described by mandate*:
schema hash equal -> keep the block; changed -> serialize old state through ref,
swap, deserialize by field name into the new layout, defaults for new fields.
Renames lose values (acceptable in dev); the fallback is reset-to-scene-values,
never a torn block.

**Pointer capture across reloads.** The root C-as-script hazard: a stale pointer
into an old DLL's rodata or a reallocated pool. Answer is layered: state pointers
banned by convention (strict tier: by lint); env verbs traffic in handles; string
literals returned to the engine are interned to SIDs at the boundary. Locals do
not survive the frame, so swap-at-frame-boundary makes them safe.

**A crashing script kills the session.** In-process native code -- containment is
best-effort, and honestly so. Dev mitigations: script calls run under the SEH/crash
harness (sys_crash exists) so faults report `script vpath + function + instance`
before breaking; the service can quarantine the unit (disable dispatch, keep state)
so the editor session survives to save work. cr.h-style auto-rollback to the last
good DLL on load-crash. Ship builds are past this: packs are as trusted as the
engine itself.

**Compile latency breaking flow.** Sub-second for one TU is the contract: PCH +
`/X` + tiny SDK surface + async compile with old-code-runs-until-swap. If a team
edits scripts faster than cl.exe turns them around, packs give a coarse-grain
fallback. Measure before adding a second compiler (sec. 5).

**Symbol and identity collisions.** Per-script DLLs export one well-known symbol.
Packs keep one TU per script (statics stay private) and reach descriptors through
the generated registry -- no cross-TU symbol traffic at all. Script identity is the
vpath, never the C name.

**Windows-only comfort.** cl.exe spawning is the Windows path; the POSIX port
(north star M6) swaps the toolchain module under the same `-script` verb --
build_tool already abstracts spawn/toolchain per-platform.

**Scope creep into a framework.** Scripts are leaves. A script may not export an
API to other scripts, register cvars, or hold engine resources directly --
cross-script talk goes through world signals/events (`ent_signal`, script events).
The moment something needs to be *depended on*, it graduates to a real module.
This single rule keeps the dep graph clean and reloads trivial (any script can
swap in isolation, no ordering).

---

## 8. Prior art -- who tried native-code-as-script, and what it teaches

**Quake 2 (id Tech 2), 1997.** Game logic as `game.dll`, mods written in C against
`game_import_t` / `game_export_t` -- two exchanged function-pointer structs, i.e.
exactly ORB's gateway idiom. Proved native game code + stable vtable seam works at
scale and birthed a huge mod scene. Taught: version-stamp the exchange structs
(id did; every engine since copies it), and a crashing mod takes the process --
players accepted it, editors should not (hence quarantine, sec. 7).

**Half-Life / Source.** Client + server C++ DLLs, same lineage. Taught: the DLL
seam is also the *distribution* seam -- mods ship as compiled DLLs, matching ORB's
pack shape.

**Doom 3 -- the counterexample.** id had native DLLs and *still* added a custom
script language, largely so level designers could write logic without a compiler
and crash without killing the game. The lesson is not "add a language": it is that
the audience matters. ORB's answers to those two pressures are compile-on-save that
feels interpreter-fast (no toolchain ceremony in the author's face) and dev-time
quarantine. If a designer-tier need emerges later, it should be *data on top of
scripts* (event/trigger wiring in scenes, north-star ref machinery), not a parser.

**Handmade Hero (Casey Muratori), 2014+.** Game as a hot-reloaded DLL with all
state in one externally-owned memory block -- the pattern ORB's mod system already
implements. Taught the two iron rules ORB inherited: the reloaded code must never
own its state, and pointers must not cross the swap.

**The Machinery (Our Machinery), 2018-2022.** An entire engine as C plugins:
struct-of-function-pointer APIs, hot-reload everywhere, versioned interfaces,
ref-like data model ("The Truth"). The closest architectural cousin to ORB.
Taught: append-only versioned API structs work for years; hot-reload discipline
degrades without an enforcement mechanism (their plugin checklist -> ORB's fences
and lint); and singleton plugins are the wrong grain for *content* logic -- which
is exactly why ORB scripts are instanced (sec. 4).

**cr.h (fungos).** Single-header C hot-reload harness: versioned reload loop,
crash detection via guarded call, automatic ROLLBACK to the last working DLL.
Small, proven, and the direct inspiration for the script service's
last-good-unit fallback.

**TCC / libtcc live-C environments (CToy, tcc-script experiments).** Demonstrate
C compiling fast enough to *feel* interpreted (~10 ms), enabling REPL-like flows.
Taught both directions: the feel is achievable and worth chasing -- and the cost
of a second compiler (weak debug info, divergent codegen, ABI corners) is why ORB
chases the feel with cl.exe + cache + PCH instead.

**Playdate SDK.** Games are C compiled to a dylib the runtime loads, authored
against ONE vendor header exposing a single `PlaydateAPI*` vtable -- allocation,
files, input, drawing all as capabilities. The clearest shipping example of
"managed C": the platform never hands you libc, it hands you its API. ORB's
`orb_script.h` + env vtable is the same shape scoped to a game world.

**Godot GDExtension / Unity native plugins.** Native libraries bound *by name*
into a data-driven object model, resolved through a versioned interface struct.
Taught: name-based late binding at the data boundary (ORB: vpath -> descriptor)
is what lets native code behave like script from the content side.

**Unreal Live Coding (Live++).** The other road: patch functions *in place* inside
the running process, no DLL swap, no state contract. Magical when it works,
fragile at structure/layout changes, and unshippable as a content model. Validates
ORB's choice: swap whole units at a frame boundary against an explicit state
contract -- less magic, always explainable.

Net lessons ORB adopts: versioned exchanged vtables (Quake 2, Machinery),
state-outside-the-code (Handmade Hero), rollback-on-crash (cr.h), one curated
header as the whole world (Playdate), vpath late binding (Godot), instanced
content grain (Machinery's gap), and interpreter-feel via compile speed, not via
an interpreter (TCC).

---

## 9. What does NOT get built

- **No interpreter, no bytecode, no custom parser.** The C compiler is the entire
  language toolchain. The strict subset is a lint, not a dialect compiler.
- **No in-place function patching.** Unit swap at frame boundary only.
- **No second compiler tier** until measured compile latency demands it.
- **No script-to-script linking.** Leaves only; graduation path is a real module.
- **No security claims.** Capability discipline, honestly labeled (sec. 6).
- **No editor-scripting fork.** If the editor ever wants scripted tools, it is the
  same service over editor env tables -- not a parallel system.

---

## 10. Experiment plan (gated; runs beside the north star)

**E1 -- the naked loop.** Sandbox `sb_script`: one `.c` compiled by a new
`build_tool -script` verb, loaded, called, edited, recompiled, hot-swapped with
state preserved, compile failure falls back to last-good. No world, no assets.
Exit: sub-second save-to-swap, debugger breakpoint hits in script code before and
after a swap. *This proves or kills the whole idea at minimal cost.*

**E2 -- instanced + described.** Script service with instance state pools;
`SCRIPT_STATE` through ref; schema-hash gate + serialize-migrate on layout change;
SEH quarantine. Exit: 100 instances of 3 script types dispatching in `on_sim`;
edit a state struct mid-run and keep field values.

**E3 -- content binding.** "script" asset type; script component on world entities
(needs north-star M2); scene save/load round-trips script params; inspector edits
them. Exit: a level whose behavior is defined entirely by `content/**.c`.

**E4 -- packs + ship.** `-script-pack`, generated registry TU, package_tool link
step, registry resolving linked-in descriptors. Exit: the E3 level runs from a
shipped build with zero compiles and zero loose script DLLs.

Gate honestly at E1: if save-to-swap cannot be made to feel like scripting on real
hardware, stop there -- the north star loses nothing.

---

## 11. Fit test (addendum edition)

The north star's four questions apply, plus three of its own:

1. Does the script speak only through env vtables reachable from its context?
2. Is every datum in `SCRIPT_STATE` a value or a handle -- reload-safe by
   construction?
3. Could this script become a pack member and a shipped linked unit with zero
   source changes?

If any answer is no, it is not a script -- it is a module wearing a costume, and it
should go live in the module system where those powers are legal.
