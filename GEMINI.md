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

## Header Conventions
Every engine library uses a three-header split:

| Header | Who includes it | What it contains |
|--------|----------------|-----------------|
| `<module>.h` | Headers needing only types | Pure types, enums, structs, constants, callback typedefs, macros. No vtable, no function decls. |
| `<module>_api.h` | DLL `.c` files calling through vtable | Includes `<module>.h` + `mod_import.h`. Adds `<module>_api_t`, `MOD_GATEWAY_*`, `MOD_USE_*`. |
| `<module>_host.h` | Host exes, unity entries, sandboxes | Includes `<module>_api_h`. Adds direct-call function declarations. |

**`mod` is self-hosting** and uses four files: `mod_import.h`, `mod_api.h`, `mod_host.h`, and `mod_export.h`.

## Module System (Hot-Reload)
- Hot-reloadable modules are implemented as DLLs.
- Each module must implement a `mod_api_t` descriptor.
- **`init()`**: Runs once on the first load.
- **`reload()`**: Runs on every subsequent hot-swap. Use this to re-cache sibling API pointers.
- **State**: Persistent state is allocated/zeroed by the system and passed to `init`/`reload`. Modules must not free this state.
- **API Surface**: `func_api_size` must remain constant across hot-reloads. Adding/removing API functions requires a full host restart.

## Build Modes
| Mode | Flag | Modules | Hot-reload |
|------|------|---------|------------|
| Modular (default) | *(none)* | `.dll` | Yes |
| Monolithic | `-monolithic` | `.lib` | No |

The `-monolithic` flag defines `BUILD_STATIC` globally. `MOD_GATEWAY_STATIC` / `MOD_GATEWAY_DYNAMIC` macros in module API headers switch behavior automatically.

## Reflection System (rs_)
- **Module pattern**: leaf module (no deps), inits before core.
- **Internal string pool**: 16 KB flat pool.
- **Stack-frame registry**: each module pushes a frame on load and pops on unload.
- **Lazy resolution**: fields reference base types by hash; resolved to stable IDs after all registrations.
- **Schema hash**: deterministic hash of reflected layout used to detect hot-reload ABI breaks.

## Coding Standards
- **Language**: C11.
- **Aggregator**: Include engine APIs via `source/engine_api.h` when needed.
- **Style**: Adhere to `.clang-format` (Google-based).
  - 4-space indentation (spaces only).
  - 110-column limit.
  - Braces on their own lines (`BreakBeforeBraces: Custom`).
  - Spaces inside parentheses: `func( arg1, arg2 )`.
  - Pointer alignment left: `int* ptr`.
  - `SortIncludes: false` - keep include order as written.
- **Character Set**: Use standard ASCII only; do not use Unicode in source generation or comments.

## Custom Build System
ORB uses a self-contained, high-performance C-based build orchestrator.

- **Bootstrapper**: `bootstrap_build_tool.bat` compiles the build tool only. `bootstrap_gen.bat` compiles and immediately runs `-gen`.
- **Orchestrator**: `source/tools/build_tool/build_tool.c` manages compiler/linker flags.
- **Artifacts**: All binaries land in `bin/` and all intermediate objects/PDBs land in `obj/`.
- **Interface**:
  - `bin\build_tool.exe -gen`: Generates/updates IDE project files.
  - `bin\build_tool.exe -clean`: Wipes `bin/` and `obj/`.
  - `bin\build_tool.exe -config <Debug|Release>`: Performs the actual build.
  - `bin\build_tool.exe -target <name>`: Build a specific target.

## Build & Execution
- **Bootstrap**: Run `bootstrap_build_tool.bat` or `bootstrap_gen.bat` from a **Developer Command Prompt**.
- **Hot-Rebuild**: Use `build_hot.bat <target> [Debug|Release]` to rebuild a module while the debugger is attached.
- **Verification**: Validate by running sandbox executables:
  - `sb_engine_sys` - sys layer (OS abstractions)
  - `sb_engine_core` - core layer (memory, logging, cvars)
  - `sb_engine_reflect` - rs_ reflection system
  - `sb_engine_mod` - module system / hot-reload
  - `sb_engine_app` - application / windowing

## Troubleshooting & Common Mistakes
- **Shell Environment**: When running on Windows via PowerShell, always use `.\script.bat` or `cmd /c script.bat` to execute batch files.
- **Missing cl.exe**: If `cl.exe` is not found, the MSVC environment is not initialized. Use a Developer Command Prompt.
- **Git Ignored Files**: The `build_new/` directory and `.vcxproj` files are often git-ignored. When searching or reading them with tools, ensure `respect_git_ignore: false` is set.

## AI Workflow Guidelines
- **Stateless Base**: When modifying `source/base/`, ensure no global or static state is introduced.
- **Surgical Edits**: Follow the project's formatting strictly.
- **Verification**: For engine changes, prioritize running the relevant sandbox executables.
- **ASCII Only**: Strictly avoid non-ASCII symbols in source, comments, or generated code.
