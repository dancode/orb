# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**ORB "The Glowing Orb"** — a C11 game engine with a modular, hot-reload-first architecture. Primary target is Windows with Visual Studio 2022; also supports Clang-CL.

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

After generating, open the `.sln` in `build_dynamic/` or `build_monolithic/` and build from Visual Studio. Build outputs land flat in `<build_dir>/bin/` (no Release/Debug subdirectories).

**Hot-rebuild a single module** (keeps debugger attached):

```bat
build_hot.bat <build_dir> <target> <config>
REM Example: build_hot.bat build_dynamic render Debug
```

## Testing

There is no automated test framework. Validation is done by running sandbox executables:

- `sb_engine_sys` — sys layer (OS abstractions)
- `sb_engine_core` — core layer (memory, logging, cvars)
- `sb_engine_mod` — module system / hot-reload
- `sb_engine_app` — application / windowing

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
  core/               — stateful systems: memory arenas, logging, cvars, reflection, SIDs
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

The root header `source/orb.h` must be included in every source file. Include engine APIs via their aggregator `source/engine_api.h` where needed.
