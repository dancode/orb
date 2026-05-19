# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**ORB "The Glowing Orb"** — a C11 game engine with a modular, hot-reload-first architecture. Primary target is Windows with Visual Studio 2022 or higher;

Only create the MSVC Win64 implementation and stub in #error messages for other platforms.

## Build

Generate the Visual Studio solution (run once, or after CMakeLists changes):

```bat
REM Dynamic build — hot-reload enabled (default for development)
"build_vs_2022 - MSVC (DYNAMIC).bat"

REM Monolithic build — static linking, no hot-reload (for shipping)
"build_vs_2022 - MSVC (STATIC).bat"

REM Clang-CL variant
"build_vs_2022 - CLANG.bat"

REM Clean all build outputs
clean_build.bat
```

After generating, open the `.sln` in `build/` or `build_monolithic/` and build from Visual Studio. Build outputs land flat in `<build_dir>/bin/` (no Release/Debug subdirectories).

**Hot-rebuild a single module** (keeps debugger attached):

```bat
build_hot.bat <build_dir> <target> <config>
REM Example: build_hot.bat build_dynamic render Debug
```

## Testing

There is no automated test framework. Validation is done by running sandbox executables:

- `sb_engine_sys` — sys layer (OS abstractions)
- `sb_engine_core` — core layer (memory, logging, cvars)
- `sb_engine_reflect` — rs_ reflection system (registration, lookup, hot-reload simulation)
- `sb_engine_mod` — module system / hot-reload
- `sb_engine_app` — application / windowing
- `sb_tool_modinfo` — standalone module descriptor inspector (loads a DLL and prints its `mod_api_t`)

Build and run the relevant sandbox target in Visual Studio, or via:

```bat
cmake --build build_dynamic --target sb_engine_mod --config Debug
build_dynamic\bin\sb_engine_mod.exe
```

## Architecture

The engine is organized as a strict dependency hierarchy. Lower layers never depend on higher ones.

```
source/base/          — stateless stdlib (math, strings, memory, logging)
source/engine/
  sys/                — OS abstractions: files, threads, time, DLL loading, paths
  core/               — stateful systems: memory arenas, logging, cvars, rs_ reflection, SIDs
  mod/                — module registry: dynamic loading, hot-reload, dependency graph
  app/                — windowing, events, main-loop lifecycle
source/runtime/       — simulation scaffolding: host loop + services (jobs, input, timing, assets) + hot-reload DLLs (render, audio, physics, animation, asset)
source/developer/     — dev-only services not shipped: in-engine CMake invoker, hot-reload wrapper
source/game/          — game framework: world, entity, component, actor (hot-reload DLLs)
source/editor/        — editor framework: windows, panels, tools (hot-reload DLLs)
source/tools/         — standalone exe utilities: asset pipeline, shader compiler, launcher
source/host/          — executable entry points: game, editor, tool, sandbox
source/sandbox/       — test executables for each engine layer
source/project/       — game-specific code (sample_game)
```

**`source/base/`** must remain stateless (no globals) because it links into both the host exe and DLLs. Any state lives in `engine/core/` or higher.

**Engine libraries** (`sys`, `core`, `mod`, `app`) are always statically linked into the host. They are never inside a DLL.

## Header Conventions

Every engine library uses a three-header split:

| Header | Who includes it | What it contains |
|--------|----------------|-----------------|
| `<module>.h` | Everyone | Types, enums, structs, constants, callback typedefs, helper macros. No vtable. No function declarations. Includes `<module>_api.h` at the bottom. |
| `<module>_api.h` | DLL modules, generated code, anything calling through the vtable | `<module>_api_t` function-pointer struct, `MOD_GATEWAY_*` macro, `MOD_USE_*` / `MOD_FETCH_*` macros. Includes `mod.h`. |
| `<module>_host.h` | Host executables, unity build entries, test sandboxes | Direct-call function declarations (lifecycle, registration, lookup, diagnostics, etc.) and `<module>_get_mod_desc()`. Includes `<module>.h` and `mod_host.h`. |

**The rule:** if your code calls `module_fn()` directly, include `<module>_host.h`. If it only uses the vtable (`module()->fn()`), include `<module>.h` (which pulls in `<module>_api.h` automatically).

Existing sets: `mod.h/mod_host.h`, `rs.h/rs_api.h/rs_host.h`, `sys.h/sys_api.h/sys_host.h`, `app.h/app_api.h/app_host.h`.

When adding a new engine library, follow the same split.

## Reflection System (rs_)

Located in `source/engine/rs/`. The unity build entry is `rs.c`, which defines the internal string pool, the shared `rs_registry_t`, and includes all `rs/*.c` translation units. It is a proper engine module loaded via `mod_static_load("rs", rs_get_mod_api())` — see `rs_host.h` for host integration.

Key design points:
- **Module pattern**: rs is a leaf module (no deps) that inits before core. core declares `"rs"` as a dependency so the mod system inits rs first. Hosts include `engine/rs/rs_host.h` and call `rs_wire_mod_callbacks()` to connect DLL load events automatically — no boilerplate callbacks needed.
- **Internal string pool**: rs owns a 16 KB flat string pool; `rs_init()` (no args) sets it up. No sid or external interner needed.
- **Stack-frame registry**: each module pushes a frame on DLL load and pops it on unload; registration and teardown are O(1) with no tombstones or validity flags.
- **Lazy resolution**: fields reference base types by hash during registration; `rs_finalize_frame()` resolves hashes to stable type IDs, so cross-type forward references work regardless of registration order.
- **Packed modifier chain**: up to four declarator modifiers (pointer, array, const, etc.) encoded in a single 16-bit value per field, preserving exact C declaration order.
- **Schema hash**: every registered type gets a deterministic hash of its reflected layout; used to detect hot-reload ABI breaks.

Follows the project header convention (see above). Include `rs.h` in DLL modules and generated registration code; include `rs_host.h` in host executables, unity build entries, and test sandboxes.

Implementation files: `rs_registry.c`, `rs_access.c`, `rs_walk.c`, `rs_serialize.c`, `rs_print.c`, `rs_test.c`.

## Module System

Every hot-reloadable `.dll` implements a `mod_api_t` descriptor. Module authors include only `engine/mod/mod_export.h`; consumers include `engine/mod/mod_api.h`.

**Implementing a module** (in the module's `.c`):

```c
// Declare the module's API accessor via the static/dynamic gateway macro
// (placed in the module's public .h, not here)

static bool render_init( void* state, get_api_fn get_api )
{
    if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
    // one-time setup — not called again on hot-reload
    return true;
}

static bool render_reload( void* state, get_api_fn get_api )
{
    if ( !MOD_FETCH_API( core_api_t, core ) ) return false;
    // re-cache sibling API pointers after a DLL swap
    return true;
}

static mod_api_t s_render_mod_api = {
    .version      = 1,
    .state_size   = sizeof( render_state_t ),
    .func_api_size = sizeof( render_api_t ),
    .func_api     = &g_render_api_struct,
    .deps         = { "core" },
    .dep_count    = 1,
    .init         = render_init,
    .reload       = render_reload,
    .exit         = render_exit,
};

mod_api_t* render_get_mod_api( void ) { return &s_render_mod_api; }
MOD_DEFINE_EXPORTS( render )   // emits the undecorated DLL entry point
```

Key invariants:
- `func_api_size` **must not change** across a hot-reload — adding/removing API functions requires a full host restart.
- `state` is allocated and zeroed by the system on first load and preserved across reloads. Modules must not free it.
- `init()` runs once; `reload()` runs on every subsequent hot-swap.

**Consuming a module API** (in a `.c` that calls into a sibling module):

```c
// File scope: allocate the cached pointer (no-op in monolithic builds)
MOD_DEFINE_API_PTR( render_api_t, render );

// In init() / reload():
if ( !MOD_FETCH_API( render_api_t, render ) ) return false;

// Call site is identical in dynamic and static builds:
render_api()->begin_frame( dt );
```

## Build Modes

Controlled by the `ENGINE_MONOLITHIC` CMake option:

| Mode | Define | Modules | Hot-reload |
|------|--------|---------|------------|
| Dynamic (default) | *(none)* | `.dll` | Yes |
| Monolithic | `BUILD_STATIC` | `.lib` | No |

`MOD_GATEWAY_STATIC` / `MOD_GATEWAY_DYNAMIC` macros in module API headers switch the accessor implementation automatically. Call sites (`render_api()->fn()`) are identical in both modes.

## Code Style

Governed by `.clang-format` (Google base, customized):
- 4-space indentation, spaces only, 110-column limit
- All braces on their own line (`BreakBeforeBraces: Custom` with wrapping on everything)
- Pointer alignment left: `int* ptr`
- Spaces inside parentheses: `func( arg1, arg2 )`
- `SortIncludes: false` — keep include order as written
- Comments should be added to show intention concisely at every block to make it easy to read through in english. 
- DO NOT USED UNICODE characters in source generation, just use standard ascii
The root header `source/orb.h` must be included in every source file. 

