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

## Custom Build System
ORB uses a self-contained, high-performance C-based build orchestrator to replace traditional CMake or complex batch scripts.

- **Bootstrapper**: `bootstrap_build_tool.bat` compiles the build tool from source using a minimal `cl.exe` call.
- **Orchestrator**: `source/tools/build_tool/build_tool.c` manages compiler/linker flags, environment detection (via `vswhere.exe`), and target orchestration.
- **Project Generator**: `source/tools/build_tool/build_tool_gen.c` (included as a unity fragment in `build_tool.c`) generates Visual Studio `.sln` and `.vcxproj` files.
- **Artifacts**: All binaries land in `bin/` and all intermediate objects/PDBs land in `obj/`.
- **Interface**:
  - `build_tool.exe -gen`: Generates/updates IDE project files.
  - `build_tool.exe -clean`: Wipes `bin/` and `obj/` for a fresh start.
  - `build_tool.exe -config <Debug|Release>`: Performs the actual build. Case-insensitive to match VS macros.

## Build & Execution
- **Bootstrap**: Run `bootstrap_build_tool.bat` if `bin/build_tool.exe` is missing or needs an update. 
  - **CRITICAL**: This requires an MSVC environment. Use `vc_vars_setup.bat` to find the path to `vcvarsall.bat` and run it in the same shell session before bootstrapping.
  - **Command**: `cmd /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 && bootstrap_build_tool.bat'` (Adjust path based on `vc_vars_setup.bat`).
- **Generate Solution**: Run `bin/build_tool.exe -gen` to create `orb_make.sln` and `orb_build.sln`.
- **Hot-Rebuild**: Use `build_hot.bat <build_dir> <target> <config>` to rebuild a module while the debugger is attached.
- **Verification**: Use relevant sandbox targets (e.g., `sb_base_custom.exe`) to verify changes.

## Troubleshooting & Common Mistakes
- **Shell Environment**: When running on Windows via PowerShell, always use `.\script.bat` or `cmd /c script.bat` to execute batch files.
- **Missing cl.exe**: If `cl.exe` is not found, the MSVC environment is not initialized. Refer to `vc_vars_setup.bat`.
- **Git Ignored Files**: The `build_new/` directory and `.vcxproj` files are often git-ignored. When searching or reading them with tools, ensure `respect_git_ignore: false` is set.
- **Tool Collisions**: Do not attempt to run `replace` on the same file multiple times in a single turn.
- **Bootstrap Failures**: If `bootstrap_build_tool.bat` fails due to locked files, ensure no instances of `build_tool.exe` are running.

## AI Workflow Guidelines
- **Stateless Base**: When modifying `source/base/`, ensure no global or static state is introduced.
- **Surgical Edits**: Follow the project's formatting strictly.
- **Verification**: For engine changes, prioritize running the relevant sandbox executables.
- **Module Updates**: When adding functions to a module API, remember that it breaks hot-reload compatibility for that session.
