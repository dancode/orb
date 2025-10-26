# Orb Engine - Overview

	* Engine.exe is the thin runtime host.
	* Game.dll is the default gameplay framework.
	* Editor.exe is the dev shell (kept separate).
	* Projects slot into this pipeline via DLLs + assets.

## Repository Root

```
/engine/         		# Core runtime systems (data processors)
/game/                  # Gameplay framework (data orchestrators)
/editor/                # Tooling built on engine + game
/projects/         		# Actual user/game projects
/tools/                 # Standalone utility executables
	
```

## Engine Folder (Cook: Data Processing)

```
engine/
  core/
    memory.h/.c         # Allocators, pools, arenas
    threading.h/.c      # Threading, jobs, fibers
    logging.h/.c        # Logging and diagnostics
    serialization.h/.c  # Binary/JSON/TOML serializer
    reflection.h/.c     # Runtime type info

  platform/
    input.h/.c          # Keyboard, mouse, controller
    window.h/.c         # Window/context creation
    fs.h/.c             # Filesystem abstraction
    net.h/.c            # Socket layer, transport

  system/
    render/
      renderer.h/.c     # Render API abstraction (Vulkan/GL/DX)
      mesh.h/.c         # Vertex/index buffer handling
      shader.h/.c       # Shader modules
      texture.h/.c      # Texture formats
    audio/
      mixer.h/.c        # DSP mixer, spatialization
      stream.h/.c       # Audio buffer streaming
    physics/
      physics.h/.c      # Physics solver interface
      collision.h/.c    # Collision mesh/shape handling
    animation/
      anim_runtime.h/.c # Skeleton, blend trees, IK solvers

  test/
	test_core.c
	test_platform.c
	test_system_render.c
	test_system_mesh.c	
	
  runtime/
    module.h/.c         # Module/plugin loader
    resource.h/.c       # Resource manager (engine-level)
    engine_main.c       # engine.exe entry point


```
			
## (Chef: Data Orchestration)(Cook: Data Processing)

```
    game/
      ecs/
        entity.h/.c         # Entity IDs, lifetimes
        component.h/.c      # Component registry
        system.h/.c         # System scheduling
      world/
        world.h/.c          # Scene graph, spatial partition
        prefab.h/.c         # Entity archetypes/prefabs
      gameplay/
        navmesh.h/.c        # Navigation meshes, pathfinding
        ai.h/.c             # AI state machines
        replication.h/.c    # Gameplay-level networking
        scripting.h/.c      # Script bindings (Lua/C#/etc.)
        events.h/.c         # Gameplay event dispatch
      animation/
        anim_graph.h/.c     # State machines, transitions
      game_main.c           # Middle-layer init for engine.exe
  
```

## Editor Folder (Tools on Top of Engine+Game)

```

    editor/
      ui/
        panels/
          inspector_panel.h/.c  # Entity/component inspector
          world_panel.h/.c      # Scene/world view
          asset_panel.h/.c      # Asset browser
        gizmos.h/.c             # Transform gizmos, handles
        editor_ui.h/.c          # Docking, menu system
      tools/
        importer.h/.c           # Asset import/conversion pipeline
        hot_reload.h/.c         # Script/asset hot reload
        profiler_panel.h/.c     # Performance visualization
      editor_main.c             # editor.exe entry point
  
```
		
## Projects Folder (Game-Specific Content & Logic)
	
```
    projects/
      demo_game/                # Example project
        assets/
          models/knight_model.mesh
          anims/knight_skeleton.anim
          audio/sword_swing.wav
          world/level01.scene
        scripts/
          attack_ability.lua    # Game-specific attack script
          quest_system.lua      # Game-specific quest logic
        prefabs/
          knight.toml           # Defines knight entity composition
          goblin.toml           # Defines goblin entity composition
        game/
          combat_system.h/.c    # Project-specific gameplay system
          quest_system.h/.c     # Project-specific quest rules
        demo_game.toml          # Project definition file
	
```
## Tools Folder

	* By bootstrapping them on the minimal engine runtime, you get:
	* Multiplatform support “for free” (filesystem, threading, serialization).
	* Consistency (tools behave like the engine because they share the same scaffolding).
	* Lower maintenance cost (no duplicated platform abstraction code).

### Execution Modes 


	* Tools bootstrapped on engine runtime:
		- They link against engine/core + engine/platform (but not rendering/windowing unless needed).
		- Tools can strip down modules (e.g. shader_tool doesn’t need physics/audio).
		- Run as CLI (shader_tool --in foo.vert --out foo.spv) or be launched by editor.

	* Engine runtime supports CLI mode:
		- No window or render loop, just a job scheduler + filesystem + serialization.
		- That way you can run on CI, headless servers, or batch pipelines.

## Tools Folder

```

    /engine/                  	# Core runtime systems
    /game/                    	# Gameplay framework
    /editor/                  	# Editor GUI + tools integration
    /tools/                   	# Standalone utility executables
      build_tool/	
        build_tool_main.c     	# Project build orchestrator
      reflect_tool/	
        reflect_main.c        	# Codegen/reflection parser
      asset_tool/	
        asset_main.c          	# Asset import/conversion
      package_tool/	
        package_main.c        	# Packaging, distribution
      reload_tool/	
        reload_main.c         	# Hot reload manager
      shader_tool/	
        shader_main.c         	# Shader compiler / cross-compiler
    
```
		
## Dependency Direction

```

	engine/ → standalone, no dependencies on game/editor/projects.
	game/ → depends on engine/.
	editor/ → depends on engine/ + game/.
	projects/ → depends on game/ (optionally engine directly).
	
	This way, you can walk through the repo and instantly know whether something is:
	
	a raw data processor (engine),
	a gameplay framework feature (game),
	a tool/editor utility (editor),
	or a project-specific addition (projects).
	
```

## Build Output Layout

```

    /bin/                      	# Built executables
      engine.exe               	# Core runtime (engine main loop)
      editor.exe               	# Editor (depends on engine + game)
      demo_game.exe            	# Project-specific game binary (optional)
    tools/
		build_tool.exe        # Build orchestrator
		reflect_tool.exe      # Reflection/codegen
		asset_tool.exe        # Asset converter
		package_tool.exe      # Packaging
		reload_tool.exe       # Hot reload CLI
		shader_tool.exe       # Shader compilation
	projects/
		demo_game.exe         # Optional direct game binary
	
    /lib/                     	# Compiled libraries / modules
      engine/	
        core.lib/.dll         	# Core runtime (memory, threading, logging)
        platform.lib/.dll     	# OS abstraction
        render.lib/.dll       	# Renderer backend
        audio.lib/.dll        	# Audio backend
        physics.lib/.dll      	# Physics backend
        animation.lib/.dll    	# Animation runtime
      game/	
        ecs.lib/.dll          	# Entity/component framework
        world.lib/.dll        	# World/scene graph
        nav.lib/.dll          	# Navigation system
        gameplay.lib/.dll     	# Generic gameplay systems (AI, replication)
        animgraph.lib/.dll    	# Animation graphs
      editor/	
        editor_ui.lib/.dll    	# UI framework, panels
        editor_tools.lib/.dll 	# Importers, profilers, etc.
	  tools/	
		tool_core.lib/.dll  	# shared code between tools.
      projects/	
        demo_game.lib/.dll    	# Project-specific systems (quests, combat)
   
```	 

## Runtime Folder Layout (For Shipping / Deployment)

```
    runtime/
      engine.exe
      game.dll
      editor.exe
      tools/
        build_tool.exe
        reflect_tool.exe
        asset_tool.exe
        package_tool.exe
        reload_tool.exe
        shader_tool.exe
      modules/
        render_vk.dll
        render_dx12.dll
        physics_bullet.dll
        audio_fmod.dll
      projects/
        demo_game/
          demo_game.dll
          assets/
            knight_model.bin
            knight_skeleton.bin
            level01.bin
		
```		

## Target Dependency Graph

```

[engine_core] ──┐
[engine_platform]│
[engine_systems] │
                  ├─> [engine_runtime] ──┐
[engine_runtime] ─┘                       │
                                         │
                                ┌────────┼─────────┐
                                │                  │
                             [game]            [tools_*]
                                │                  │
                                │             (build, reflect,
                                │              asset, shader, etc.)
                                │
                                v
                             [editor]
                                │
                                v
                            [projects_*]
							
```

## Engine Systems

```

engine_core  		<-- foundational utilities    │
engine_platform 	<-- platform abstraction for OS / filesystem / input     
engine_systems  	<-- processes data (rendering, physics, audio, animation)

	* Tools and game can link only the parts of engine they need:
	* A CLI tool might only link engine_core + engine_platform.
	* Game layer links engine_core + engine_platform + engine_systems.
	* Editor links all three plus game.

```

## Engine Breakdown

```

ENGINE_CORE                 (lowest-level foundation)
  ├─ memory/                # Allocators, arenas, stack/pool memory
  ├─ threading/             # Threads, job system, atomics
  ├─ containers/            # Array, hashmap, string buffer, object pools
  ├─ logging/               # Log levels, assertions
  ├─ time/                  # Timers, delta time, profiling utilities
  ├─ serialization/         # Binary/JSON/TOML serialization, endianness
  ├─ reflection/            # Runtime type info, type/module registry
  └─ config/                # Compile-time feature/config flags

DEPENDENCIES: None
USED BY: engine_platform, engine_systems, game, tools, editor

-------------------------------------------------------------

ENGINE_PLATFORM             (OS abstraction layer)
  ├─ fs/                    # Filesystem I/O, path utilities, optional watcher
  ├─ window/                # Window creation, graphics context, monitor info
  ├─ input/                 # Keyboard, mouse, controller, input state
  ├─ threads_os/            # OS threading primitives (mutex, cond)
  ├─ time_os/               # High-resolution OS timers, sleep/yield
  ├─ dynamic_library/       # Load/unload DLLs or shared libraries
  ├─ network/               # TCP/UDP sockets, IP utilities, packets
  └─ platform_config.h      # Conditional compilation flags

DEPENDENCIES: engine_core
USED BY: engine_systems, tools, game, editor

-------------------------------------------------------------

ENGINE_SYSTEMS              (data processing / runtime systems)
  ├─ render/                # Renderer abstraction, meshes, textures, shaders, pipelines
  ├─ audio/                 # Audio mixer, DSP effects, streaming, spatialization
  ├─ physics/               # Rigid body simulation, collisions, constraints, queries
  ├─ animation/             # Skeletons, animation graphs, blending, IK
  ├─ resource/              # Resource management, caching, GPU/audio buffers
  └─ module/                # System/module registration, hot-reload support

DEPENDENCIES: engine_core, engine_platform
USED BY: game, editor, tools

-------------------------------------------------------------

HIGHER LAYERS DEPENDENCY CHAIN:

engine_core  ──┐
engine_platform ─┼─> engine_systems ──┐
                                      │
                                      v
                                   game layer
                                      │
                                      v
                                   editor
                                      │
                                      v
                                  projects/tools

Notes:
- engine_core = utilities, completely platform-independent
- engine_platform = OS services, provides scaffolding to engine_systems and tools
- engine_systems = actual runtime processing of data (rendering, physics, audio, animation)
- game layer = builds logic on top of engine_systems
- editor/tools = use engine_systems + engine_platform + engine_core as needed


```

## Engine Parts

```

1. engine_core

	Purpose: The true low-level foundation. Provides essential services that everything else depends on, but has no platform-specific or system-specific code.
  
	* No platform dependencies — completely portable.
	* No graphics, audio, or OS API calls.
	* Engine systems and game code depend on this library heavily.
	* Provides shared utilities for memory, containers, and reflection that other libraries consume.
	
engine_core/
  memory.h/.c           # Allocators, arenas, pools, custom malloc/free, scratch memory
  threading.h/.c        # Job system, thread pool, atomic operations, synchronization primitives
  logging.h/.c          # Logging, debug output, error handling
  time.h/.c             # High-resolution timers, delta time calculations
  serialization.h/.c    # Core serializers (binary, JSON, TOML), reflection support
  reflection.h/.c       # Minimal runtime type info, type IDs, module registration
  container/            # Core containers: dynamic array, hash map, string buffer
    array.h/.c
    hashmap.h/.c
    string_buffer.h/.c
  config.h/.c           # Configuration macros, compile-time options


2. engine_platform

	Purpose: Platform abstraction layer. Provides OS-level services, handles differences between Windows, Linux, macOS, consoles, etc.

	* Depends on engine_core for memory, containers, logging.
	* Pure abstraction — no “game logic” inside.
	* Provides the scaffolding that all higher-level modules (engine_systems, tools, game, editor) use.

engine_platform/
  fs.h/.c               # Filesystem abstraction, path utilities, file I/O
  window.h/.c           # Window creation, context setup (OpenGL/Vulkan/DX)
  input.h/.c            # Keyboard, mouse, controller input
  threads_os.h/.c       # OS-specific threading primitives, condition variables, mutexes
  time_os.h/.c          # OS timers, sleep functions
  dynamic_library.h/.c  # Loading DLLs/shared libraries at runtime
  network.h/.c          # Socket abstraction, TCP/UDP, basic networking
  platform_config.h/.c  # Conditional compilation, defines per OS
  
3. engine_systems
  
	Purpose: Actual runtime processing systems — the parts of the engine that handle data and processing (rendering, audio, physics, animation, etc.)
  
	* Depends on engine_core + engine_platform.
	* No gameplay-specific logic — e.g., physics knows how to simulate forces but doesn’t  know about “player health” or “enemy AI.”
	* Processes raw data — meshes, audio buffers, collision shapes, skeletons.

engine_systems/
  render/
    renderer.h/.c       # Abstract rendering interface (Vulkan/GL/DX12 backend)
    mesh.h/.c           # GPU mesh buffers, vertex/index management
    texture.h/.c        # Texture upload, format conversion
    shader.h/.c         # Shader compilation and management
  audio/
    mixer.h/.c          # Mixing audio streams
    dsp.h/.c            # Simple DSP effects, spatialization
    stream.h/.c         # Audio streaming from disk/memory
  physics/
    physics.h/.c        # Solver interface, rigid body simulation
    collision.h/.c      # Collider shapes, broadphase/narrowphase
    integrator.h/.c     # Physics integration steps
  animation/
    anim_runtime.h/.c   # Skeleton animation, IK solvers, blending
    anim_graph.h/.c     # Low-level runtime for animation graphs
  resource/
    resource.h/.c       # Loading and managing raw GPU/audio/physics resources
	
```

## Engine Core : Full Breakdown

```

engine_core/
  ├─ memory/
  │   ├─ memory.h/.c            # Allocators, pools, scratch memory, aligned allocations
  │   ├─ arena.h/.c             # Linear arena allocator
  │   └─ stack_allocator.h/.c   # Temporary/per-frame stack allocations
  │
  ├─ threading/
  │   ├─ threading.h/.c         # Thread creation, join, detach, mutex, condition
  │   ├─ job_system.h/.c        # Simple job system / task scheduler
  │   └─ atomic.h/.c            # Atomic operations (CAS, fetch_add, etc.)
  │
  ├─ containers/
  │   ├─ array.h/.c             # Dynamic arrays (vector equivalent)
  │   ├─ hashmap.h/.c           # Hash map/dictionary
  │   ├─ string_buffer.h/.c     # Growable string buffers
  │   ├─ string_view.h          # Lightweight string slices
  │   └─ pool.h/.c              # Object pools for small allocations
  │
  ├─ logging/
  │   ├─ logging.h/.c           # Log levels, formatting, output sinks
  │   └─ assert.h/.c            # Assertions, debug checks
  │
  ├─ time/
  │   ├─ time.h/.c              # High resolution timers, delta time
  │   └─ stopwatch.h/.c         # Simple timing utility for profiling
  │
  ├─ serialization/
  │   ├─ serializer.h/.c        # Binary/JSON/TOML serialization helpers
  │   ├─ deserializer.h/.c      # Reading structured data
  │   └─ endian.h/.c            # Endianness helpers for cross-platform
  │
  ├─ reflection/
  │   ├─ reflection.h/.c        # Runtime type info, type IDs
  │   ├─ type_registry.h/.c     # Global type registry
  │   └─ module_registry.h/.c   # Register modules/components for introspection
  │
  ├─ config/
  │   ├─ config.h               # Compile-time options, feature flags
  │
  └─ engine_core.h              # Umbrella include for all core headers
  
  ```
##  Module Responsibilities

```

Module Responsibilities
1. Memory

Handles all low-level allocations in the engine.

Provides custom allocators for performance: arena, stack, pool.

Optional per-frame scratch memory to avoid repeated malloc/free.

All other engine code (platform, systems, game) uses these for memory safety and tracking.

2. Threading

Lightweight cross-platform threading abstractions (threads, mutexes, condition variables).

Atomic operations for lock-free structures.

Optional job system for multithreaded task scheduling.

3. Containers

Dynamic array, hash map, string buffers, pools.

Self-contained, no STL dependency.

Optimized for predictable memory usage and cache locality.

4. Logging

Logging levels: DEBUG, INFO, WARN, ERROR.

Optional logging sinks: console, file, platform debugger.

Lightweight assertion framework for development builds.

5. Time

High-resolution timers.

Utilities for measuring delta time per frame.

Stopwatches for profiling code blocks.

6. Serialization

Core routines for reading/writing structured data.

Binary and human-readable formats (JSON/TOML).

Handles endianness and alignment for multiplatform compatibility.

7. Reflection

Minimal runtime type information.

Type IDs, module/component registration.

Enables introspection for serialization, editor integration, or scripting.

8. Config

Central place for compile-time flags (debug/release, platform, feature toggles).

Enables conditional compilation across engine/core/platform.

9. engine_core.h

Single umbrella header for easy inclusion in engine, game, editor, tools.

Example:

```
 ```
 
 Dependencies

Standalone: Engine_core has no dependencies on engine_platform or engine_systems.

Used by everything else: engine_platform, engine_systems, game layer, tools, editor.

Ensures all foundational services (memory, threading, logging) are available everywhere.

Design Notes

Keep no platform-specific code here; that belongs in engine_platform.

Keep no high-level systems (rendering, physics, audio) here; that belongs in engine_systems.

This module should be fully testable on its own, as all other engine code depends on it.

Design with data-driven principles in mind — reflection, serialization, and memory systems are critical to support modular, hotloadable systems later.

```

## Engine Platform : Full Breakdown

```

engine_platform/
  ├─ fs/
  │   ├─ fs.h/.c               # File I/O abstraction, paths, directories
  │   ├─ path.h/.c             # Path utilities (join, normalize, extension)
  │   └─ watch.h/.c            # Optional file system watcher for hot reload
  │
  ├─ window/
  │   ├─ window.h/.c           # Window creation, destruction, swapchain init
  │   ├─ context.h/.c          # Graphics context management (Vulkan, OpenGL)
  │   └─ monitor.h/.c          # Monitor info, resolution, refresh rate
  │
  ├─ input/
  │   ├─ input.h/.c            # Keyboard, mouse, controller input abstraction
  │   ├─ gamepad.h/.c          # Gamepad/controller mapping
  │   └─ input_state.h/.c      # Current state of devices (key down/up, mouse pos)
  │
  ├─ threads_os/
  │   ├─ threads_os.h/.c       # OS-specific threading primitives (mutex, cond, semaphores)
  │   └─ atomic_os.h/.c        # Atomic operations using OS support if needed
  │
  ├─ time_os/
  │   ├─ timer.h/.c            # High-resolution timer (OS-specific)
  │   └─ sleep.h/.c            # Cross-platform sleep / yield functions
  │
  ├─ dynamic_library/
  │   ├─ dynamic_library.h/.c  # Load/unload shared libraries (.dll/.so/.dylib)
  │
  ├─ network/
  │   ├─ network.h/.c          # Socket abstraction (TCP/UDP)
  │   ├─ ip.h/.c               # IP address, host resolution utilities
  │   └─ packet.h/.c           # Packet serialization helpers
  │
  ├─ platform_config.h         # Conditional compilation for OS features
  └─ engine_platform.h         # Umbrella include for all platform headers
  
  ```
  
  ## Module Responsibilities
  
  ``` 
  1. fs/

Provides abstract file I/O: open, read, write, close files.

Directory iteration and utilities (list_dir, exists, create_dir).

Optional file watcher for editor/tools hot reload.

Ensures cross-platform compatibility for paths and separators.

2. window/

Creates windows and graphics contexts.

Handles swapchain setup, surface creation (Vulkan, OpenGL).

Exposes monitor info, resolution, DPI scaling.

Note: Minimal GUI logic here; this is only OS-level window management.

3. input/

Abstracts all user input from keyboard, mouse, and controllers.

Maintains current input state (key down/up, mouse position, button states).

Supports gamepads with a uniform mapping across platforms.

4. threads_os/

Wraps OS-level threads, mutexes, semaphores, condition variables.

Provides a portable interface to engine_core threading if platform-specific primitives are needed.

5. time_os/

Provides high-resolution timers for delta time, profiling.

Provides cross-platform sleep/yield.

Used by engine_core time utilities if needed.

6. dynamic_library/

Load and unload dynamic libraries at runtime.

Supports engine hot-reload, tools, plugins.

Wraps OS-specific functions like LoadLibraryA/dlopen.

7. network/

Abstracts TCP/UDP socket creation, sending, receiving.

Handles IP resolution, packet formatting.

Provides lightweight cross-platform networking for engine systems or tools.

8. platform_config.h

Conditional compilation for platform-specific features (Windows vs Linux vs macOS).

Defines platform macros for use in engine_systems, tools, and editor.

9. engine_platform.h

Umbrella include for all platform headers:

```
```

Dependencies

Depends on engine_core for memory, containers, logging, atomic primitives.

No dependencies on engine_systems or game.

Used by engine_systems, game, tools, editor, and any CLI apps.

Design Notes

Platform abstraction only: No rendering, physics, audio, or game logic here.

Must support headless operation for tools.

Keep OS-specific details confined to .c files; headers remain cross-platform.

This is the layer that allows tools to bootstrap on engine runtime without duplicating filesystem, threading, or dynamic library code.

```

## Engine Systems : Full Breakdown

```
engine_systems/
  ├─ render/
  │   ├─ renderer.h/.c       # Abstract rendering interface
  │   ├─ mesh.h/.c           # Mesh buffers (vertex/index) and GPU uploads
  │   ├─ texture.h/.c        # Texture creation, format conversion, upload
  │   ├─ shader.h/.c         # Shader compilation, linking, management
  │   ├─ framebuffer.h/.c    # Offscreen render targets
  │   └─ pipeline.h/.c       # Graphics pipelines (blend, rasterization state)
  │
  ├─ audio/
  │   ├─ mixer.h/.c          # Mixing multiple audio streams
  │   ├─ dsp.h/.c            # DSP effects (filters, reverb, panning)
  │   ├─ stream.h/.c         # Streaming audio from disk or memory
  │   └─ audio_device.h/.c   # Low-level platform audio interface
  │
  ├─ physics/
  │   ├─ physics.h/.c        # Physics solver interface
  │   ├─ collision.h/.c      # Collider shapes, broadphase/narrowphase
  │   ├─ integrator.h/.c     # Rigid body integration (velocity, position)
  │   ├─ constraints.h/.c    # Joints, springs, limits
  │   └─ query.h/.c          # Raycast, overlap, AABB queries
  │
  ├─ animation/
  │   ├─ anim_runtime.h/.c   # Skeletons, pose blending, IK solvers
  │   ├─ anim_graph.h/.c     # Animation graph runtime
  │   ├─ anim_clip.h/.c      # Animation clip playback, timelines
  │   └─ skeleton.h/.c       # Skeleton hierarchy management
  │
  ├─ resource/
  │   ├─ resource.h/.c       # Resource loader/manager (GPU buffers, audio data)
  │   ├─ material.h/.c       # Material definition, shader binding
  │   └─ cache.h/.c          # Resource caching and lifetime management
  │
  ├─ module/
  │   ├─ module.h/.c         # Module/plugin registration for runtime systems
  │
  └─ engine_systems.h        # Umbrella include for all systems headers
  
  ```
## Engine System Responsibilities
  
  ```
  
  Module Responsibilities
1. render/

Abstracts graphics API (Vulkan/OpenGL/DX12).

Handles GPU resource creation and management: meshes, textures, shaders, framebuffers.

Manages pipelines, state objects, and submission to the GPU.

Engine systems don’t know about “game logic” — just render this data.

2. audio/

Mixes audio streams and applies DSP effects.

Streams audio from disk/memory and schedules playback.

Exposes spatialization info (position, velocity, attenuation).

Low-level platform output uses engine_platform/audio_device.

3. physics/

Simulates rigid bodies and constraints.

Supports broadphase/narrowphase collision detection.

Provides query system: raycasts, overlap tests.

Purely data-driven: does not know about “player health” or “enemy AI.”

4. animation/

Handles skeleton pose computation, blending multiple animation clips.

Evaluates animation graphs and updates final transforms.

Supports inverse kinematics and hierarchical skeletons.

5. resource/

Manages raw data for rendering, audio, and physics.

Responsible for loading resources from disk (via engine_platform/fs) or network.

Caches resources to avoid redundant uploads to GPU/audio system.

Exposes handles/pointers to engine systems.

6. module/

Manages registration of systems as modules, enabling hot-reload or selective system initialization.

7. engine_systems.h

Umbrella header to include all systems:

```

```

Dependencies

Depends on engine_core (memory, containers, threading, logging).

Depends on engine_platform for OS-level services: windowing, GPU context, audio output, timing.

Does not depend on game layer or editor.

Exposes pure data processing APIs to game, editor, and tools.

Design Notes

All systems are modular and can be selectively initialized.

Supports data-driven architecture: all input/output is data buffers, handles, and IDs.

Enables hot-reloading: resources, shaders, or entire modules can be swapped without touching game logic.

Clear separation ensures that physics, audio, animation, and rendering can be tested independently.

```
