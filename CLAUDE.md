# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**ORB "The Glowing Orb"** -- a C11 game engine with a modular, hot-reload-first architecture.
Primary target is Windows with Visual Studio 2022 or higher.
POSIX (Linux/macOS) is a planned secondary target; the platform layer exists but has not been
tested yet. Do not stub POSIX paths with `#error` -- maintain them as working code.

---

## IMPORTANT: ASCII Only

**Do NOT use Unicode, UTF-8 special characters, smart quotes, em-dashes, or any non-ASCII
symbol in source files, comments, or generated code. Use only standard 7-bit ASCII.**

---

## Build System

ORB uses a self-contained C build orchestrator (`build_tool.exe`) instead of CMake or MSBuild.
It directly invokes `cl.exe`/`link.exe`/`lib.exe` and generates the Visual Studio solution files.

### First-time setup / after modifying build_tool source

Run from a **Developer Command Prompt** (vcvarsall must be loaded):

```bat
bootstrap_build_tool.bat    :: compile build_tool.exe only
bootstrap_gen.bat           :: compile build_tool.exe + immediately run -gen
```

`vc_vars_setup.bat` will print the correct `vcvarsall.bat` path if you need it.

### Daily workflow

```bat
bin\build_tool.exe -gen                         :: regenerate .sln/.vcxproj files
bin\build_tool.exe -config Debug                :: CLI build (all targets)
bin\build_tool.exe -config Debug -target core   :: CLI build (one target)
bin\build_tool.exe -clean                       :: wipe bin/ and obj/
```

Open `build\orb_make.sln` in Visual Studio to build and debug normally.
All outputs land flat in `bin/` and intermediates in `obj/<target>/`.

### Hot-rebuild (keeps debugger attached)

```bat
build_hot.bat <target> [Debug|Release]
REM Example: build_hot.bat render Debug
```

### Build modes

| Mode | Flag | Modules | Hot-reload |
|------|------|---------|------------|
| Modular (default) | *(none)* | `.dll` | Yes |
| Monolithic | `-monolithic` | `.lib` | No |

The `-monolithic` flag defines `BUILD_STATIC` globally. `MOD_GATEWAY_STATIC` /
`MOD_GATEWAY_DYNAMIC` macros in module API headers switch behavior automatically.
Call sites (`render_api()->fn()`) are identical in both modes.

## Testing

No automated test framework. Validate by running sandbox executables:

- `sb_engine_sys` -- sys layer (OS abstractions)
- `sb_engine_core` -- core layer (memory, logging, cvars)
- `sb_engine_reflect` -- rs_ reflection system
- `sb_engine_mod` -- module system / hot-reload
- `sb_engine_app` -- application / windowing
- `sb_tool_modinfo` -- loads a DLL and prints its `mod_api_t`

Build and run the relevant sandbox target in Visual Studio, or via:

```bat
bin\build_tool.exe -config Debug -target sb_engine_mod
bin\sb_engine_mod.exe
```

## Architecture

Strict dependency hierarchy -- lower layers never depend on higher ones.

```
source/base/          -- stateless stdlib (math, strings, memory, logging)
source/engine/
  sys/                -- OS abstractions: files, threads, time, DLL loading, paths
  core/               -- stateful systems: memory arenas, logging, cvars, rs_ reflection, SIDs
  mod/                -- module registry: dynamic loading, hot-reload, dependency graph
  app/                -- windowing, events, main-loop lifecycle
source/runtime/       -- simulation scaffolding: host loop + services + hot-reload DLLs
source/developer/     -- dev-only services not shipped: hot-reload wrapper
source/game/          -- game framework: world, entity, component, actor (hot-reload DLLs)
source/editor/        -- editor framework: windows, panels, tools (hot-reload DLLs)
source/tools/         -- standalone exe utilities: asset pipeline, shader compiler, launcher
source/host/          -- executable entry points: game, editor, tool, sandbox
source/sandbox/       -- test executables for each engine layer
source/project/       -- game-specific code (sample_game)
```

**`source/base/`** must remain stateless (no globals) -- it links into both the host exe and DLLs.

**Engine libraries** (`sys`, `core`, `mod`, `app`) are always statically linked into the host. Never in a DLL.

## Header Conventions

Every engine library uses a three-header split:

| Header | Who includes it | What it contains |
|--------|----------------|-----------------|
| `<module>.h` | Headers needing only types | Pure types, enums, structs, constants, callback typedefs, macros. No vtable, no function decls, no downstream includes. |
| `<module>_api.h` | DLL `.c` files calling through the vtable | Includes `<module>.h` + `mod_import.h`. Adds `<module>_api_t`, `MOD_GATEWAY_*`, `MOD_USE_*` / `MOD_FETCH_*`. |
| `<module>_host.h` | Host exes, unity build entries, test sandboxes | Includes `<module>_api.h`. Adds direct-call function declarations, `<module>_get_mod_desc()`. |

Include chain: `module_host.h` -> `module_api.h` -> `module.h`

**mod is self-hosting** -- four files instead of three:
- `mod_import.h` -- infrastructure macros (`MOD_GATEWAY_*`, `MOD_FETCH_API`, `MOD_DEFINE_API_PTR`). Include only in `_api.h` files.
- `mod_api.h` -- mod's own vtable (`mod_api_t`, `MOD_USE_MOD`/`MOD_FETCH_MOD`). Includes `mod_import.h`.
- `mod_host.h` -- direct-call mod functions. Includes `mod_api.h`.
- `mod_export.h` -- module implementation header (`.c` files only). Defines `mod_desc_t`, lifecycle typedefs, `MOD_DEFINE_EXPORTS`.

Existing sets: `mod_import.h/mod_api.h/mod_host.h/mod_export.h`, `ref.h/ref_api.h/ref_host.h`,
`sys.h/sys_api.h/sys_host.h`, `app.h/app_api.h/app_host.h`, `core.h/core_api.h/core_host.h`.

## Reflection System (rs_)

Located in `source/engine/ref/`. Unity build entry is `ref.c`. Loaded via
`mod_static_load("ref", ref_get_mod_desc())` -- see `ref_host.h` for host integration.

Key design points:
- **Module pattern**: leaf module (no deps), inits before core. Hosts call `ref_wire_mod_callbacks()` to connect DLL load events -- no boilerplate needed.
- **Internal string pool**: 16 KB flat pool; `rs_init()` sets it up.
- **Stack-frame registry**: each module pushes a frame on load and pops on unload; O(1) registration and teardown.
- **Lazy resolution**: fields reference base types by hash; `rs_finalize_frame()` resolves to stable type IDs after all registrations.
- **Packed modifier chain**: up to four declarator modifiers encoded in a single 16-bit value per field.
- **Schema hash**: deterministic hash of the reflected layout, used to detect hot-reload ABI breaks.

Include `ref.h` in DLL modules; include `ref_host.h` in hosts, unity entries, and sandboxes.

Implementation: `rs_registry.c`, `rs_access.c`, `rs_walk.c`, `rs_serialize.c`, `rs_print.c`, `rs_test.c`.

## Module System

Every hot-reloadable `.dll` implements a `mod_api_t` descriptor.

**Implementing a module:**

```c
static bool render_init( void* state, get_api_fn get_api )
{
    if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
    return true;  // one-time setup
}

static bool render_reload( void* state, get_api_fn get_api )
{
    if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
    return true;  // re-cache sibling API pointers after DLL swap
}

static mod_api_t s_render_mod_api = {
    .version       = 1,
    .state_size    = sizeof( render_state_t ),
    .func_api_size = sizeof( render_api_t ),
    .func_api      = &g_render_api_struct,
    .deps          = { "core" },
    .dep_count     = 1,
    .init          = render_init,
    .reload        = render_reload,
    .exit          = render_exit,
};

mod_api_t* render_get_mod_api( void ) { return &s_render_mod_api; }
MOD_DEFINE_EXPORTS( render )
```

Key invariants:
- `func_api_size` **must not change** across a hot-reload. Adding/removing API functions requires a full host restart.
- `state` is allocated and zeroed by the system on first load and preserved across reloads. Modules must not free it.
- `init()` runs once; `reload()` runs on every subsequent hot-swap.

**Consuming a module API:**

```c
MOD_DEFINE_API_PTR( render_api_t, render );   // file scope

// In init() / reload():
if ( !MOD_FETCH_API( render_api_t, render ) ) return false;

// Call site -- identical in modular and monolithic builds:
render_api()->begin_frame( dt );
```

## Code Style

Governed by `.clang-format` (Google base, customized):
- 4-space indentation, spaces only, 110-column limit
- All braces on their own line (`BreakBeforeBraces: Custom`)
- Pointer alignment left: `int* ptr`
- Spaces inside parentheses: `func( arg1, arg2 )`
- `SortIncludes: false` -- keep include order as written
- Add comments showing intent in a concise way at each block.

