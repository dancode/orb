# CLAUDE.md

**ORB "The Glowing Orb"** -- a C11 game engine with a modular, hot-reload-first architecture.
Primary target is Windows with Visual Studio 2022+. POSIX (Linux/macOS) is a planned secondary
target -- maintain POSIX paths as working code, never stub with `#error`.

Documentation is deliberately small. `docs/README.md` is the overview (invariants, the stack
as it exists, what comes next); `docs/CONTENT.md` covers resource names, cooking, the asset
service and shipping; `docs/SHADERS.md` the shader pipeline; `docs/GUI.md` the gui from the
outside. Deeper references sit beside their code: `source/runtime_service/gui/GUI_ARCHITECTURE.md`,
`source/game/framework/world.md`, `source/engine/ref/ref.md`. Code is the source of truth.

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
`cl.exe`/`link.exe`/`lib.exe`, generates Visual Studio solution files, and runs the content
cook (see docs/CONTENT.md). Targets are declared in `orb.targets`.

**First-time setup** (works from a plain shell; the script finds and loads vcvarsall itself):

```bat
bootstrap_build_tool.bat        :: compile build_tool.exe into bin/
bin\build_tool.exe -gen         :: generate build\orb_all.sln and friends
bin\build_tool.exe -doctor      :: diagnose environment, registry, child-project wiring
```

**Daily workflow:**

```bat
bin\build_tool.exe -config Debug                :: build all targets, then cook stale content
bin\build_tool.exe -config Debug -target core   :: build one target's closure
bin\build_tool.exe -gen                         :: regenerate .sln/.vcxproj after editing orb.targets
bin\build_tool.exe -clean                       :: wipe bin/ and obj/
bin\build_tool.exe -help                        :: the full flag list
build_hot.bat <target> [Debug|Release]          :: rebuild one module with -no-deps (debugger stays attached)
```

Open `build\orb_all.sln` in Visual Studio for normal build/debug. Outputs land in `bin/`,
intermediates in `build/obj/<target>/`, cooked content in `build/content/`.

Flags worth knowing: `-force` skips the up-to-date check; `-content` builds the cooker before
cooking (a plain build cooks with whatever `asset_tool.exe` is already in bin/); `-no-content`
only reports stale cooked files; `-shipping` is Release + whole-program optimization +
`-content -strict-content`; `-create <name> -type project` scaffolds a standalone child project.

**Build modes:**

| Mode | Flag | Modules | Hot-reload |
|------|------|---------|------------|
| Modular (default) | *(none)* | `.dll` | Yes |
| Monolithic | `-monolithic` | `.lib` | No |

`-monolithic` defines `BUILD_STATIC` globally. `MOD_GATEWAY_STATIC`/`MOD_GATEWAY_DYNAMIC` in
module API headers switch behavior automatically. Call sites are identical in both modes.

## Testing

No automated test framework. Every layer has a sandbox executable under `source/sandbox/`;
build it and run it. The headless ones return the failed-check count as their exit code.

| Sandbox | Validates |
|---------|-----------|
| `sb_base`, `sb_sys`, `sb_core`, `sb_mod`, `sb_app`, `sb_job`, `sb_net`, `sb_prof`, `sb_fs` | one engine library each |
| `sb_reflect`, `sb_ref_exe`, `sb_gen_exe` | ref_ reflection, including reflected DLLs |
| `sb_res` | res name helpers + the build's resource manifest (headless) |
| `sb_asset_test` | asset registry, dedup, hot reload (headless); `sb_asset_image`, `sb_asset_shader` on screen |
| `sb_world` | the world framework: entities, components, queries, rebind |
| `sb_gui_test` | gui headless assertions; `sb_gui_example` is the on-screen feature explorer |
| `sb_vulkan`, `sb_vulkan_stress`, `sb_quad_pull` | rhi |
| `sb_host_*` | host shapes over run_host |

```bat
bin\build_tool.exe -config Debug -target sb_mod && bin\sb_mod.exe
```

## Architecture

Strict dependency hierarchy -- lower layers never depend on higher ones. `docs/README.md`
has the tier diagram with status per system.

```
source/base/            -- stateless stdlib: math, strings, utf8, fmt, memory, bit/char, test
                           harness; no globals; links into the host AND every DLL
source/vendor/          -- single-header third-party code: stb_image(+_write), stb_sprintf,
                           miniz, nanosvg; each compiled in exactly one consuming TU
source/engine/          -- root engine libraries, listed lowest to highest
  mod/                  -- module registry: loading, hot-reload, dep-ordered init; substrate, online first
  sys/                  -- OS abstractions: files, threads, time, DLL loading, paths, processes (leaf)
  ref/                  -- reflection registry: types, fields, schema hash (leaf)
  res/                  -- resource names, header-only: the RID() marker the build scans for,
                           canonical-form check, name hash, name+ext join, cooked-file reference
                           section; no library, no module. See docs/CONTENT.md
  prof/                 -- profiler: SID zones, SPSC rings, trace dump (dep: sys)
  pack/                 -- compression: deflate/inflate, crc32, zip read/write; owns the single
                           engine-wide miniz copy (leaf)
  fs/                   -- virtual file system: mounts, zip bundles, catalog (deps: sys, pack)
  job/                  -- job system: worker pool (dep: sys)
  net/                  -- UDP transport: handshake, channels, fragmentation (dep: sys)
  core/                 -- engine orchestration layer: arenas, logging, cvars, cmd/console, config, SIDs
                           (deps: sys, ref) -- always the TOP of the engine root libraries
  app/                  -- windowing, events, main-loop lifecycle (no hard deps; wired by hosts)
source/runtime/         -- run: the one host loop (run_host_main) and the project contract
                           (run_project.h: on_start / on_sim / on_frame / on_draw / on_hud / on_stop)
source/runtime_service/ -- static libs with a mod_desc, linked into hosts
  rhi/                  -- render hardware interface: Vulkan, bindless descriptors, .oshd loader
  draw/                 -- 2d/3d batch drawing over rhi
  gui/                  -- in-house immediate-mode GUI; 16 unity units, one per band (gui_frame,
                           gui_core, gui_render, gui_draw, gui_flow, gui_style, gui_stock, gui_chrome...)
  asset/                -- asset registry: acquire by name, refcount, typed loaders, hot reload
  input/                -- action binding service
  console/              -- developer console UI over core's cvar/console backend
  ahi/                  -- audio hardware interface: WASAPI device + mixer
source/runtime_modules/ -- hot-reload DLLs: render, audio, example, physics (stub), animation (empty)
source/game/            -- game runner DLL: project bind, play/stop/pause/step, fixed-step clock
  framework/            -- the world: entities, ref-described components, dense pools, queries
                           (unity-joined into its owner; see framework/world.md)
source/game_service/    -- nav (stub)          source/game_modules/   -- game_example (stub)
source/editor/          -- editor static lib: menu bar, dockspace, Viewport / Game / Deploy windows
source/editor_service/  -- viewport (scene render target, view camera, ray pick)
source/editor_modules/  -- editor_example (hot-reload DLL stub)
source/developer/       -- dev-only static libs: dev_build, dev_hot (runtime rebuild + swap),
                           dev_font (stb_truetype baker), dev_image, dev_vector, dev_ship (packager)
source/tools/           -- standalone exes: build_tool, reflect_tool, res_tool, asset_tool,
                           shader_tool, font_tool, image_tool, ship_tool, launch_tool
source/host/            -- executable entry points: host_game, host_editor, host_common
source/sandbox/         -- test executables, grouped by layer (base, engine, reflect, rhi, runtime,
                           gui, game, host, tool)
source/project/         -- sample_game (living reference), template_game (source of -create)
third_party/            -- freetype-2.14.3 (font_tool only) + its prebuilt bin/
content/                -- source content root: a resource name (RID( "ui/icon/save" )) is a file's
                           path here minus its extension; the build resolves it (res_tool) and a
                           child project's content/ shadows it name by name. See docs/CONTENT.md
source_content/         -- raw sources (TTF, SVG) read by tools only; mirrors content/
build/content/          -- generated cooked forms (.oshd, .orb_font, .tex), mounted above content/
```

Engine libraries (`mod`, `sys`, `ref`, `prof`, `pack`, `fs`, `job`, `net`, `core`, `app`)
are always statically linked into the host. Never in a DLL.

## Libraries

| Library | Location | Purpose |
|---------|----------|---------|
| **Vulkan** | `%VULKAN_SDK%` (runtime loaded) | Graphics API -- no volk; custom 4-stage function pointer bootstrap in `vk_library.c` |
| **dxc** | `%VULKAN_SDK%\Bin\dxc.exe` (spawned) | HLSL -> SPIR-V in `shader_tool`; never linked |
| **SPIRV-Reflect** | `source/tools/shader_tool/vendor/` | Shader interface extraction at cook time (tool only) |
| **DXGI 1.5** | System (`dxgi.lib`) | VRR support check only -- not used for rendering |
| **FreeType 2.14.3** | `third_party/freetype-2.14.3/` | Font rasterization for the `font_tool` offline atlas baker |
| **stb_truetype / stb_rect_pack** | `source/developer/dev_font/` | Runtime dev font baker; rect_pack also drives the gui atlases |
| **stb_image / stb_image_write** | `source/vendor/` | Image decode in the asset image loader, gui icons, asset_tool, dev_image |
| **stb_sprintf** | `source/vendor/` | `base/fmt` formatting |
| **miniz** | `source/vendor/` | Deflate + zip, compiled once in `engine/pack` |
| **nanosvg** | `source/vendor/` | SVG rasterization in dev_vector (icon import) |

No GLFW, SDL, or Dear ImGui. Windowing/input use the Win32 API directly. GUI is in-house.

## Header Conventions

Every engine library and runtime service uses a three-header split:

| Header | Who includes it | Contains |
|--------|----------------|---------|
| `<module>.h` | Headers needing only types | Types, enums, structs, constants, macros. No vtable, no function decls. |
| `<module>_api.h` | DLL `.c` files | Includes `<module>.h` + `mod_import.h`. Adds `<module>_api_t`, gateway macros. |
| `<module>_host.h` | Host exes, unity entries, sandboxes | Includes `<module>_api.h`. Adds direct-call decls, `get_mod_desc()`. |

**mod** has four files (self-hosting): `mod_import.h`, `mod_api.h`, `mod_host.h`, `mod_export.h`.

Engine header sets: `mod_*`, `sys.*`, `ref.*`, `prof.*`, `pack.*`, `fs.*`, `job.*`, `net.*`,
`core.*`, `app.*`. `res` is the exception: header-only (`res.h`, `res_ref.h`, `res_cook.h`),
no `_api`/`_host` split.

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
render()->begin_frame( ctx_id );                      // call site (same in both modes)
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
