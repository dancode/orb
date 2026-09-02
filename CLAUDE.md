# CLAUDE.md

**ORB "The Glowing Orb"** -- a C11 game engine with a modular, hot-reload-first architecture.
Primary target is Windows with Visual Studio 2022+. POSIX (Linux/macOS) is a planned secondary
target -- maintain POSIX paths as working code, never stub with `#error`.

---

## IMPORTANT: ASCII Only

**Do NOT use Unicode, UTF-8 special characters, smart quotes, em-dashes, or any non-ASCII
symbol in source files, comments, or generated code. Use only standard 7-bit ASCII.**

---

## Pre-Ship: API Stability Is a Non-Concern

Nothing has shipped and there are no external users. Public API structs, vtable member
order, enum values, and header layout may be freely reordered or renamed. Never warn about
ABI breakage, hot-reload staleness, or migration concerns from reordering -- just rebuild.

---

## Build System

Custom C build orchestrator (`build_tool.exe`) -- not CMake or MSBuild. Directly invokes
`cl.exe`/`link.exe`/`lib.exe` and generates Visual Studio solution files.

**First-time setup** (run from a Developer Command Prompt with vcvarsall loaded):

```bat
bootstrap_build_tool.bat    :: compile build_tool.exe only
bootstrap_then_gen.bat      :: compile + run -gen
```

**Daily workflow:**

```bat
bin\build_tool.exe -gen                         :: regenerate .sln/.vcxproj
bin\build_tool.exe -config Debug                :: build all targets
bin\build_tool.exe -config Debug -target core   :: build one target
bin\build_tool.exe -clean                       :: wipe bin/ and obj/
build_hot.bat <target> [Debug|Release]          :: hot-rebuild (keeps debugger attached)
```

Open `build\orb_all.sln` in Visual Studio for normal build/debug. Outputs land in `bin/`,
intermediates in `obj/<target>/`.

**Build modes:**

| Mode | Flag | Modules | Hot-reload |
|------|------|---------|------------|
| Modular (default) | *(none)* | `.dll` | Yes |
| Monolithic | `-monolithic` | `.lib` | No |

`-monolithic` defines `BUILD_STATIC` globally. `MOD_GATEWAY_STATIC`/`MOD_GATEWAY_DYNAMIC` in
module API headers switch behavior automatically. Call sites are identical in both modes.

## Testing

No automated test framework. Run sandbox executables to validate:

- `sb_sys` -- sys layer
- `sb_core` -- core layer
- `sb_reflect` -- ref_ reflection
- `sb_res` -- res resource catalogue (exit code = failed checks)
- `sb_mod` -- module system / hot-reload
- `sb_app` -- application / windowing

```bat
bin\build_tool.exe -config Debug -target sb_mod && bin\sb_mod.exe
```

## Architecture

Strict dependency hierarchy -- lower layers never depend on higher ones.

```
source/base/          -- stateless stdlib (math, strings, memory); no globals; links into host + DLLs
source/engine/        -- root engine libraries, listed lowest to highest
  mod/                -- module registry: loading, hot-reload, dep-ordered init; substrate, online first
  sys/                -- OS abstractions: files, threads, time, DLL loading, paths (leaf, no deps)
  ref/                -- reflection registry: types, fields, schema hash (leaf, no deps)
  res/                -- resource catalogue: rid_t logical-name ids, RID() door, per-module
                         name tables; see RESOURCE_ID_PLAN.md (leaf, no deps)
  prof/               -- profiler: SID zones, SPSC rings, trace dump (dep: sys)
  pack/               -- compression: deflate/inflate, crc32, zip read/write; owns the single
                         engine-wide miniz copy (leaf, no deps)
  fs/                 -- virtual file system: mounts, zip bundles (deps: sys, pack)
  job/                -- job system: worker pool (dep: sys)
  net/                -- UDP transport: handshake, channels, fragmentation (dep: sys)
  core/               -- engine orchestration layer: arenas, logging, cvars, cmd/console, config, SIDs
                         (deps: sys, ref) -- always the TOP of the engine root libraries
  app/                -- windowing, events, main-loop lifecycle (no hard deps; wired by hosts, above core)
source/runtime/       -- simulation scaffolding: host loop + services + hot-reload DLLs
source/runtime_service/
  gui/                -- in-house immediate-mode GUI (gui.c + gui_backend.c static lib)
  rhi/                -- render hardware interface (Vulkan backend)
source/developer/     -- dev-only services: hot-reload wrapper
source/game/          -- world, entity, component, actor (hot-reload DLLs)
source/editor/        -- editor framework: windows, panels, tools (hot-reload DLLs)
source/tools/         -- standalone exe utilities: asset pipeline, shader compiler, launcher
source/host/          -- executable entry points: game, editor, tool, sandbox
source/sandbox/       -- test executables for each engine layer
source/project/       -- game-specific code (sample_game)
third_party/          -- vendored libraries (freetype-2.14.3)
content/              -- source content root: a resource name (RID( "ui/icon/save" )) is a file's
                         path here minus its extension; the build resolves it (res_tool) and a
                         child project's content/ shadows it name by name. See RESOURCE_ID_PLAN.md.
```

Engine libraries (`mod`, `sys`, `ref`, `res`, `prof`, `pack`, `fs`, `job`, `net`, `core`, `app`)
are always statically linked into the host. Never in a DLL.

## Libraries

| Library | Location | Purpose |
|---------|----------|---------|
| **Vulkan** | `%VULKAN_SDK%` (runtime loaded) | Graphics API -- no volk; custom 4-stage function pointer bootstrap in `vk_library.c` |
| **DXGI 1.5** | System (`dxgi.lib`) | VRR support check only -- not used for rendering |
| **FreeType 2.14.3** | `third_party/freetype-2.14.3/` | Font rasterization for `font_tool` offline atlas baker |
| **stb_rect_pack** | `source/tools/font_tool/` | Rectangle packing for font atlas layout (tool only) |

No GLFW, SDL, or Dear ImGui. Windowing/input use the Win32 API directly. GUI is in-house.

## Header Conventions

Every engine library uses a three-header split:

| Header | Who includes it | Contains |
|--------|----------------|---------|
| `<module>.h` | Headers needing only types | Types, enums, structs, constants, macros. No vtable, no function decls. |
| `<module>_api.h` | DLL `.c` files | Includes `<module>.h` + `mod_import.h`. Adds `<module>_api_t`, gateway macros. |
| `<module>_host.h` | Host exes, unity entries, sandboxes | Includes `<module>_api.h`. Adds direct-call decls, `get_mod_desc()`. |

**mod** has four files (self-hosting): `mod_import.h`, `mod_api.h`, `mod_host.h`, `mod_export.h`.

Existing header sets: `mod_*`, `sys.*`, `ref.*`, `res.*`, `prof.*`, `pack.*`, `fs.*`, `job.*`, `net.*`, `core.*`, `app.*`.

## Module System

Every hot-reloadable DLL implements a `mod_desc_t` descriptor.

```c
static bool render_init( void* state, get_api_fn get_api )
{
    if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
    return true;
}

mod_desc_t* render_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( render_state_t ),
        .func_api_size = sizeof( render_api_t ),   // must not change across hot-reload
        .func_api      = &g_render_api_struct,
        .deps          = { "core" },
        .dep_count     = 1,
        .init          = render_init,    // runs once
        .reload        = render_reload,  // re-caches API pointers after DLL swap
        .exit          = render_exit,
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( render )
```

Consuming a module API:

```c
MOD_DEFINE_API_PTR( render_api_t, render );          // file scope
if ( !MOD_FETCH_API( render_api_t, render ) ) ...    // in init()/reload()
render()->begin_frame( dt );                          // call site (same in both modes)
```

Key invariants:
- `func_api_size` must not change across hot-reload -- adding/removing functions requires a host restart.
- `state` is allocated/zeroed by the system on first load and preserved across reloads; modules must not free it.

## Reflection System (ref_)

Located in `source/engine/ref/`. Unity build entry: `ref.c`.

- Leaf module (no deps), inits before core. Hosts call `ref_wire_mod_callbacks()`.
- 16 KB internal string pool; stack-frame registry (O(1) register/teardown per module).
- Lazy field resolution by hash; `ref_finalize_frame()` resolves after all registrations.
- Schema hash detects hot-reload ABI breaks.

Include `ref.h` in DLL modules; `ref_host.h` in hosts, unity entries, sandboxes.

## Code Style

`.clang-format` (Google base, customized) governs formatting -- run it; don't hand-format.
- Comments show intent concisely at each block.
- Struct fields ALWAYS use `//` trailing comments, never `/* */`. Larger blocks (file headers,
  function/section comments) use C style `/* */`.
- Wrap block comments at 94 characters in a line.

### Comment voice

Write comments for a reader seeing the file for the first time, with no memory of how it got
this way -- not as a reply to whatever task produced the code. State what a thing is and does,
plainly and confidently, the way you'd explain it to a competent teammate who just joined.

- Say what the code IS/DOES, not the story of how it got here. No "the model says", no
  "unlike before", no "this fixes/adds/changes X" -- that belongs in a commit message, not a
  comment that will outlive it.
- Don't re-litigate the design. A comment can state an invariant or a gotcha; it shouldn't argue
  for why the design is right, contrast it with alternatives, or restate architecture doc prose.
  If it reads like a persuasive essay, cut it down to the fact.
- Reserve real prose for the non-obvious: a hidden constraint, a lifetime/ownership rule, a unit
  quirk, a workaround for a specific bug. If a reader wouldn't be surprised, they don't need a
  paragraph.
- A one-line trailing `//` beats a multi-line `/* */` block whenever the fact fits on one line.
- Avoid in-house shorthand a newcomer wouldn't know from context ("the seam", "the door", "THE
  X") -- name the file, function, or mechanism directly instead.