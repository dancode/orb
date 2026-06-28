# CLAUDE.md

**ORB "The Glowing Orb"** -- a C11 game engine with a modular, hot-reload-first architecture.
Primary target is Windows with Visual Studio 2022+. POSIX (Linux/macOS) is a planned secondary
target -- maintain POSIX paths as working code, never stub with `#error`.

---

## IMPORTANT: ASCII Only

**Do NOT use Unicode, UTF-8 special characters, smart quotes, em-dashes, or any non-ASCII
symbol in source files, comments, or generated code. Use only standard 7-bit ASCII.**

---

## Build System

Custom C build orchestrator (`build_tool.exe`) -- not CMake or MSBuild. Directly invokes
`cl.exe`/`link.exe`/`lib.exe` and generates Visual Studio solution files.

**First-time setup** (run from a Developer Command Prompt with vcvarsall loaded):

```bat
bootstrap_build_tool.bat    :: compile build_tool.exe only
bootstrap_gen.bat           :: compile + run -gen
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

- `sb_engine_sys` -- sys layer
- `sb_engine_core` -- core layer
- `sb_engine_reflect` -- ref_ reflection
- `sb_engine_mod` -- module system / hot-reload
- `sb_engine_app` -- application / windowing

```bat
bin\build_tool.exe -config Debug -target sb_engine_mod && bin\sb_engine_mod.exe
```

## Architecture

Strict dependency hierarchy -- lower layers never depend on higher ones.

```
source/base/          -- stateless stdlib (math, strings, memory); no globals; links into host + DLLs
source/engine/
  sys/                -- OS abstractions: files, threads, time, DLL loading, paths
  core/               -- stateful systems: memory arenas, logging, cvars, ref_ reflection, SIDs
  mod/                -- module registry: dynamic loading, hot-reload, dependency graph
  app/                -- windowing, events, main-loop lifecycle
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
```

Engine libraries (`sys`, `core`, `mod`, `app`) are always statically linked into the host.
Never in a DLL.

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

Existing header sets: `mod_*`, `ref.*`, `sys.*`, `app.*`, `core.*`.

## Module System

Every hot-reloadable DLL implements a `mod_api_t` descriptor.

```c
static bool render_init( void* state, get_api_fn get_api )
{
    if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
    return true;
}

static mod_api_t s_render_mod_api = {
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

mod_api_t* render_get_mod_api( void ) { return &s_render_mod_api; }
MOD_DEFINE_EXPORTS( render )
```

Consuming a module API:

```c
MOD_DEFINE_API_PTR( render_api_t, render );          // file scope
if ( !MOD_FETCH_API( render_api_t, render ) ) ...    // in init()/reload()
render_api()->begin_frame( dt );                      // call site (same in both modes)
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

`.clang-format` (Google base, customized):
- 4-space indentation, spaces only, 110-column limit
- All braces on their own line (`BreakBeforeBraces: Custom`)
- Pointer alignment left: `int* ptr`
- Spaces inside parentheses: `func( arg1, arg2 )`
- `SortIncludes: false` -- keep include order as written
- Comments show intent concisely at each block.
- Comments use cpp style // after fields, but c style for larger blocks.

