# Orb Engine Stack

A C11-based game engine with a modular architecture designed for flexibility, hot-reloading, and separation of concerns. The engine is organized into several layers and modules, each responsible for specific functionality.

---

## Root Header

### `orb.h`
Root configuration header. Should be included in all source files.

---

## Config

Configuration files for the engine, such as:

- Build settings
- Platform-specific configurations
- Other global settings

---

## Build

Output directory for all compiled binaries and intermediate files.

Includes:

- Executables (`.exe`)
- Dynamic libraries (`.dll`)
- Static libraries (`.lib`)
- Object files (`.obj`)
- Debug symbols (`.pdb`)
- Other intermediate build artifacts

---

## Base

A standard library (`.lib`) included by all modules.

Restrictions:

- Cannot contain state because it is linked into DLLs.

Provides:

- Math
- Memory
- Strings
- Logging

---

# Engine

Contains four stateful foundational static libraries (`.lib`).

## `sys` — System Layer

Platform-specific systems:

- Files
- Threading
- Time
- Dynamic library loading
- Process management
- Paths
- Platform console
- Other OS abstractions

## `core` — Core Layer

Stateful foundational systems:

- Memory
- File system
- Logging
- Reflection
- Cvars
- String interning
- Other core systems

## `app` — Application Layer

Application framework systems:

- Windowing
- Main loop
- Events
- Lifecycle management

## `mod` — Registry Layer

Module system:

- Dynamic loading
- Module management
- Hot-reload support

---

# Runtime

Simulation scaffolding with resident services (static APIs) and a bootstrap host loop (`runtime_host`).

## Runtime Host

Responsible for:

- Startup
- Shutdown
- Running the main loop

## Runtime Services (`.lib`)

Static libraries providing non-hot-reload services:

- Jobs
- Input
- Timing
- Assets
- Console

## Runtime Modules (`.dll`)

Dynamic libraries providing hot-reloadable systems:

- Renderer
- Physics
- Audio
- Other runtime systems

---

# Developer

Static services for developer only tools that are not shipped.

- Run-time build invoker for cmake-based projects.
- Developer hot-reload convenience service (wraps build invoker)

---

# Game

Game framework (`.dll`).

Includes:

- World
- Entity
- Component
- Actor systems

## Game Host

Responsible for:

- Game-specific startup
- Shutdown
- Running the main loop

## Game Services (`.lib`)

Static libraries providing non-hot-reload services:

- World management
- Entity management
- Component management

## Game Modules (`.dll`)

Dynamic libraries providing hot-reloadable systems:

- Gameplay
- AI
- Other game systems

---

# Editor

Editor framework (`.dll`).

Includes:

- Windows
- Panels
- Tools

## Editor Host

Responsible for:

- Editor-specific startup
- Shutdown
- Running the main loop

## Editor Services (`.lib`)

Static libraries providing non-hot-reload services:

- Window management
- Panel management
- Tool management

## Editor Modules (`.dll`)

Dynamic libraries providing hot-reloadable systems:

- Scene view
- Asset browser
- Inspector
- Other editor systems

---

# Tools

A collection of standalone tools (`.exe`) used for content creation and development workflows.

Examples:

- Asset pipeline
- Shader compiler
- Level editor
- Profilers
- Build utilities

## Tools Common

Static library shared by all tools.

Provides:

- Logging
- Configuration
- Shared utilities

## Tools Host

Responsible for:

- Tool startup
- Shutdown
- Running the tool main loop

## Asset Pipeline

Processes raw assets into engine-ready formats:

- Textures
- Models
- Audio
- Other assets

## Shader Compiler

Compiles shader source code into platform-specific binary formats.

## Level Editor

Standalone level editing tool separate from the main editor.

Can share modules and services with the editor.

## Other Tools

Additional development tools such as:

- Performance profilers
- Debugging tools
- Diagnostic utilities

## Build System

Responsible for:

- Building engine and game projects
- Managing dependencies
- Automating builds

## Launcher

Responsible for:

- Launching the game or editor
- Managing configurations (debug, release, etc.)
- Managing multiple engine or game versions

---

# Sandbox

A separate executable (`.exe`) used for:

- Testing new features
- Experiments
- Prototypes

Without affecting the main editor or game.

## Sandbox Host

Responsible for:

- Sandbox startup
- Shutdown
- Running the main loop

## Sandbox Modules (`.dll`)

Hot-reloadable experimental modules such as:

- Experimental renderer
- Experimental physics
- Gameplay prototypes
- Other experimental systems

---

# Executables / Hosts

The engine contains multiple executable hosts (`.exe`) serving different purposes.

All hosts are built on the same foundational layers:

- Base
- Engine
- Runtime

Hosts can share modules and services where appropriate.

Each host is responsible for:

- Managing lifecycle
- Loading appropriate services
- Loading appropriate modules
- Maintaining context-specific execution

This architecture provides:

- Modular development
- Flexible workflows
- Hot-reload support
- Clear separation of concerns
- Shared foundational systems

---

# Hosts

A collection of executable entry points.

## Editor Host

Responsible for launching and managing the editor lifecycle.

## Game Host

Responsible for launching and managing the game lifecycle.

## Tools Host

Responsible for launching and managing tool lifecycles.

## Sandbox Host

Responsible for launching and managing the sandbox lifecycle.