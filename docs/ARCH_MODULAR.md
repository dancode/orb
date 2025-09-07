# MODULAR ARCHITECTURE

## Goals

	- We want to avoid the problems of monolithic engines.
	- We want fast builds so we only rebuild changed modules.
	- We want lean builds that do not load unused features.
	- We want extensability without modfiying the core engine.
	- We want portability with easy to swap platform layers.
	- We want hot reload to update code while running.
	- We want flexibility to load different modules on the same platform (renderer).
	- We want clarity and communication through well-defined APIs

## Native loader (engine.exe)

- A minimal runtime bootstrap loader (~50KB)

	- A small native loader acts as the entry point.
	- Modules are declared in external manifest files (module_name.mf).
	- It loads only the modules necessary for the game to run.
		- game_module.mf will list the dependencies and the modules it needs.
		- engine.exe --game_module
	
- Responsibilities:

	- Parse manifest files
	- Load and initialize modules in dependency order
	- Provide module registry and communication layer.
	- Handle module lifecycle (load/unload/reload)
	- Manage memory allocators shared across modules
	
	- The loader only handles module loading logic
	- All actual engine functionality lives in modules
	- The same loader works for both game and editor modes
	- Platform differences are handled by modules, not the loader
	
## Editor loader (editor.exe)

- The minimal development bootstrap loader (a superset of engine.exe)

	- It loads everything from engine.exe plus the editor modules and editor plugins.
	- It loads the editor framework for UI with viewport, browsers, inspectors.
	- It contains hot-reload functionality and more overhead for debugging.	
	- The editor doens't just run game modules, it wraps it and injects tools.
	- This is so you can click play in the editor and see your game running live.
	- engine.exe ships to players and editor.exe never ships to players.
	- removes the need for #ifdef EDITOR behavior in engine modules.
	- The editor is essentially a "debugging" runtime. 
	- If a module crashes it doesn't bring down the entire editor.
	- You can hot-reload the fixed code.
	
## Custom loader (my_loader.exe)

	- The modularity of the loader allows for the creation of custom bootstrappers.
	- For instance a dedicated server executable, or a VR experience launcher, etc.
	- This is done as easy as creating a new .mf file that acts as a runtime.

## Module System: 

	- All functionality delivered via dynamic libraries (.dll/.so/.dylib)
	- Each module exposes a standardized interface through generated API bindings
	- Hot-reload of modules available during development.
	- Dependency injection exists between modules.
	- A game project be can run from the engine or editor bootstrap loader.

## Shared Loader Functionality

	- All loaders use shared code to bootstrap itself.
	- A "base" static library and "core" dynamic library are always loaded first.
	- The loader is just glue that ensures "base" + "core" are loaded corectly.
	
	## Loader
	
	- The minimal loader shim as a shared single file library.
	- The loader module also includes the main function as well.
	
		loader/
		├─ loader.c    		# main() portably entry points
		├─ loader.h			# shared functions.
		└─ api_registry.c	# code for manage global api registry.
		
		1. Call base_init(argc, argv) from static library.
		2. Locate core.dll and load it using base dll load function.
		4. Find symbols in core, load_plugin.
		5. Call load_plugin( &api_registry), 
			-- Do this to register core api into global registry.
			-- Sets up reflection system.
			-- Initailizes objet and plugin managers.
		6. Transfer control to core -- which lodas the other plugins.

	## Base.lib
	
	- base.lib is a minimal static library linked into the loader (exe).
		- It depends on nothing and can be unit test by itself.	
		- Think of it as a libc replacement layer.
		- Authoritive instance of allocators, logging, and threading, etc.
		- Exposes a base_api struct then passes a ppointer into each DLLL entry point.
		- So, Core and other modules will access base.lib functions through the api pointer.
			- They include the headers only.
			- One allocator, one log system, one thread pool.
			- Plugins can hot-reload safely.
			- No duplicate state.

		// TODO: explain further later.
		- The code generator will generate the API shim and create wrappers.
		- The generated wrappers will change if dynamic of monolithic build is chosen.
		
		struct base_api {
			void* (*alloc)(size_t size); 
			void  (*free)(void* ptr);
			void  (*log)(const char* fmt, ...); // etc.
		};
		
		base/
		├─ api/
		│   ├─ assert.h         # asserts
		│   ├─ atomic.h         # atomics
		│   ├─ memory.h         # allocators, arenas, pool, heap wrappers.
		│   ├─ string.h         # string utilities, utf8, builder.
		│   ├─ hash.h           # hash tables, sets
		│   ├─ array.h          # dynamic array helpers
		│   ├─ thread.h         # threads, mutexes, semaphores
		│   ├─ io.h             # basic file ops, paths
		│   └─ log.h        	# logging macros, error channels.
		├─ os/
		│   ├─ win32.c			# timers, file I/O, DLL loading.
		│   ├─ posix.c
		│   └─ platform.h
		├─ error.c				# error handling and reporting.
		├─ memory.c				# memory allocation and management.
		├─ thread.c				# threading and sycronization primitives
		├─ plugin.c				# functions for managing plugins.
		├─ log.c				# log utilities for output and debug
		└─ init.c				# 
  
	## Core.dll
	
	- The first real plugin loaded is called "core", and has core systems.
	- This contains the services and systems for the engine, depends on base.lib.
	
		core/
		├─ api/
		│   ├─ core.h           # plugin manager, init entry point
		│   ├─ object.h         # object system, handles
		│   ├─ reflection.h     # reflection/serialization API
		│   ├─ plugin.h         # module ABI 
		│   ├─ event.h          # pub/sub system
		│   └─ job.h         	# higher-level job system
		├─ reflection.c			# access to descriptors, fiels, metadata, etc.
		├─ object.c				# handles, instances, component-style data.
		├─ event.c				# core pub/sub, async dispatch.
		#├─ plugin.c				# load/unload shared libs, maintain registry.
		├─ hot_reload.c			# unload/reload modules without crashing.
		├─ job_system.c			# scheduleing and depeendency graph.
		├─ serialize.c			# read/write using reflection JSON/binary.
		└─ core_init.c			# setup the module.
  	
	## Bootstrapping
	
	1. The shared loader shim calls base.lib's base_init() function.
	
		- The absolute minimal bootstrapper.	
		- It loads the base.lib a shared library used by all loaders.

	2. It finds the "core" dynamic library and loads it.
				
	1. Init the base library.
	2. The are critically different on what modules they load.
		engine.exe will use runtime_modules
		editor.exe will use runtime_modules + editor_modules.

	3. They both use a shared static library (base.lib) to do the actual work.
	4. So a shared function called bootstrap_run() with different inputs.
	
	- The first module loaded is always "Core", to setup basic OS services.

- Module Category Types:

	1. Runtime (Engine) Modules
	
	- Core engine systems (renderer, audio, physics, input)
	- Game system modules.
	- Platform abstraction layers
	- Available in both runtime and editor modes.
	
	2. Editor Modules 
	
	- Asset pipeline and importers
	- Scene editor, inspector panels
	- Debugging and profiling tools
	- Build system integration
	- Only loaded in editor mode!

	3. Plugin Modules

	- Third-party integrations.
	- Custom tools and extensions.
	- Game-specific systems.
	- Optional loading based on project configuration.

	4. Project Modules
	
	- User game project modules
	- A game project can have many sub modules.


- Module Directory Structure

	module_name/
	├── module_name.mf          # Manifest file
	├── source/                 # Source code
	├── include/        		# API definitions
	└── assets/                 # Module-specific assets

- Module Types 

	- Global manifest "engine.mf"
		- Contains user specfied search paths:
			- "modules/", "plugins/", "project/modules/"
			
		- Consistent load sequence.
			- runtime = core_modules -> platform_moudles -> engine_modules -> game_moduls
			- editor = runtime -> editor_modules -> tool_modules
			
		- conditions:
			- mode: 	["runtime, "editor"]
			- platfomr: ["windows","linux", "macos"]
			- renderer: ["vulkan", "dx12", "metal" ]
		
	- Module manifest "module_name.mf"
		- ...TODO...
			
- Manifest Format (file contents).
	
	module name: 		"renderer_vulkan"			
	module version: 	"1.2.0"
	module type: 		"runtime" or "editor" or "plugin"

		
## Build System Integration

Module Build Process

1. API Generation Phase		
	* Parse .api files
	* Generate interface headers and stubs
	* Create registration code

2. Compilation Phase
	* Compile module source against generated interfaces
	* Link with engine core (minimal interface only)

3. Packaging Phase

	* Bundle module binary with manifest
	* Validate dependencies and exports
	* Generate module metadata

Development Workflow
	
	// Generate APIs for all modules
	engine_gen --project myproject --generate_api

	// Build specific module
	engine_build --module renderer_vulkan --config debug

	// Launch in editor mode
	engine_loader --project myproject --mode editor

	// Package for runtime distribution
	engine_package --project myproject --target windows-x64
	
## Runtime Operation

Initialization Sequence

1. Bootstrap Phase
	* Parse global manifest (engine.mf)
	* Resolve module dependencies
	* Initialize shared allocators and core services

2. Loading Phase

	* Load modules in dependency order
	* Call module initialize() functions
	* Register services and event handlers
	* Game selection looks in projects/, then in samples/ for game_name.mf.

3. Runtime Phase

	* Execute main loop (game or editor)
	* Handle hot-reload requests
	* Process inter-module communication

# Game Project Integration

Project Structure 

my_game/
├── project.mf              # Project manifest
├── modules/                # Game-specific modules
│   ├── gameplay/
│   ├── ui/
│   └── audio/
├── assets/                 # Game assets
└── build/                  # Build outputs

# Directory map
	
modular_game_engine/
├── bootstrap/         # Main executable
├── core/              # Core systems and API registry (shared by all modules)
├── modules/           # Engine modules
│   ├── renderer/      # Vulkan renderer
│   ├── input/         # Input system
│   ├── entity/        # ECS (future)
│   └── asset/         # Asset pipeline (future)
├── plugins/           # Game plugins
│   └── sample_plugin/ # Example game
├── projects/          # Game projects using engine.
│   └── game/          # Example game
└── bin/               # Output directory
    ├── engine.exe
    ├── core.dll
    ├── modules/
    │   ├── renderer.dll
    │   └── input.dll
    ├── plugins/
    │   └── sample_plugin.dll
	│
	└── projects/
		└── game.dll
		
	

### Standalone Tool Programs To Support Engine
	
	 
	 build tool			# Creates build structure for project from manifests.
	 reflect tool		# Scans for reflection annotation and generates source files.
	 
	 asset tool			# Used to compile assets into engine formats.
	 package tool		# Exports executable module into optimized release format.	 
	 reload tool		# Compiles and updates hot-reload scripts.