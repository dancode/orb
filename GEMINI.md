# ORB "The Glowing Orb" Project Instructions

## Overview
ORB is a C11-based game engine featuring a modular, hot-reload-first architecture. It follows a strict dependency hierarchy where lower layers never depend on higher ones.

## Architecture & Layers
The engine is organized into the following hierarchical layers (from lowest to highest):

1. **`source/base/`**: Stateless standard library (math, strings, memory, hashing). **Mandatory Rule: Zero Global State.** This library is linked into both the host and all dynamic modules.
2. **`source/engine/`**: Stateful foundational static libraries.
   - `sys/`: OS abstractions (files, threads, time, DLL loading).
   - `core/`: Stateful systems (arenas, logging, cvars, string interning).
   - `rs/`: Reflection system (metadata, serialization).
   - `net/`: Networking abstractions and protocols.
   - `mod/`: Module registry, dynamic loading, and hot-reload management.
   - `app/`: Windowing, OS events, and application lifecycle.
3. **`source/runtime/`**: The host runtime scaffolding (`runtime_host`).
4. **`source/runtime_service/`**: Runtime static service libraries (e.g., `jobs`, `rhi`).
5. **`source/runtime_modules/`**: Runtime dynamic modules (e.g., `render`, `audio`, `physics`).
6. **`source/developer/`**: Development-only static tools and services (`dev_build`, `dev_hot`, `dev_reflect_gen`). Stripped in public builds.
7. **`source/game/`**: Reusable game framework components (world, entity, component, actor).
8. **`source/project/`**: User game project implementations (e.g., `sample_game`).
9. **`source/editor/`**: Editor framework components (windows, panels, tools).
10. **`source/tools/`**: Standalone utilities (asset pipeline, shader compiler).
11. **`source/sandbox/`** & **`sandbox_example/`**: Experimental testing grounds and layer-specific verification targets.
12. **`source/host/`**: Executable entry points gluing layers together into specific applications (game, editor, tool, sandbox).

## Module System (Hot-Reload)
- Hot-reloadable modules are implemented as DLLs.
- Each module must implement a `mod_api_t` descriptor.
- **`init()`**: Runs once on the first load.
- **`reload()`**: Runs on every subsequent hot-swap. Use this to re-cache sibling API pointers.
- **State**: Persistent state is allocated/zeroed by the system and passed to `init`/`reload`. Modules must not free this state.
- **API Surface**: `func_api_size` must remain constant across hot-reloads. Adding/removing API functions requires a full host restart.

## Coding Standards
- **Language**: C11.
- **Root Header**: `source/orb.h` MUST be included in every source file.
- **Aggregator**: Include engine APIs via `source/engine_api.h` when needed.
- **Style**: Adhere to `.clang-format` (Google-based).
  - 4-space indentation (spaces only).
  - 110-column limit.
  - Braces on their own lines (`BreakBeforeBraces: Custom`).
  - Spaces inside parentheses: `func( arg1, arg2 )`.
  - Pointer alignment left: `int* ptr`.
- **Character Set**: Use standard ASCII only; do not use Unicode in source generation.

## Build & Execution
- **Generate Solution**: Use `build_vs_2022 - MSVC (DYNAMIC).bat` for development.
- **Hot-Rebuild**: Use `build_hot.bat <build_dir> <target> <config>` to rebuild a module while the debugger is attached.
- **Output**: Binaries land in `<build_dir>/bin/`.
- **Verification**: Use relevant sandbox targets (e.g., `sb_engine_*`) to verify changes in specific layers.

## AI Workflow Guidelines
- **Stateless Base**: When modifying `source/base/`, ensure no global or static state is introduced.
- **Surgical Edits**: Follow the project's formatting strictly.
- **Verification**: For engine changes, prioritize running the relevant sandbox executables.
- **Module Updates**: When adding functions to a module API, remember that it breaks hot-reload compatibility for that session.
