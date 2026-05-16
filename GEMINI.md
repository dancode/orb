# ORB "The Glowing Orb" Project Instructions

## Overview
ORB is a C11-based game engine featuring a modular, hot-reload-first architecture. It follows a strict dependency hierarchy where lower layers never depend on higher ones.

## Architecture & Layers
The engine is organized into the following layers (from lowest to highest):

1. **`source/base/`**: Stateless standard library (math, strings, memory, logging). **Mandatory Rule: Zero Global State.** This library is linked into both the host and all DLLs.
2. **`source/engine/`**: Stateful foundational static libraries.
   - `sys/`: OS abstractions (files, threads, time, DLL loading).
   - `core/`: Stateful systems (memory arenas, logging, cvars, reflection, string interning).
   - `mod/`: Module registry, dynamic loading, and hot-reload management.
   - `app/`: Windowing, events, and main-loop lifecycle.
3. **`source/runtime/`**: The host runtime. The support files are in their own directories.
	**s source/runtime_service/ **: Runtime static service libraries (jobs)
	**s source/runtime_modules/ **:	Runtime dynamic module libraries (render)
4. **`source/game/`**: Game framework (world, entity, component, actor).
5. **`source/editor/`**: Editor framework (windows, panels, tools).
6. **`source/tools/`**: Standalone utilities (asset pipeline, shader compiler).
7. **`source/host/`**: Executable entry points (game, editor, tool, sandbox).

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
- **Sandbox**: Use `source/sandbox/sb_*` executables for verifying specific layers.

## AI Workflow Guidelines
- **Stateless Base**: When modifying `source/base/`, ensure no global or static state is introduced.
- **Surgical Edits**: Follow the project's formatting strictly.
- **Verification**: For engine changes, prioritize running the relevant `sb_engine_*` sandbox targets.
- **Module Updates**: When adding functions to a module API, remember that it breaks hot-reload compatibility for that session.
