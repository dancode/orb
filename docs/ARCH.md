# Modular Game Engine Design

	A lightweight, modular plugin-based game engine architecture where a minimal native loader bootstraps a fully modular system. The core philosophy is "everything is a module" - from renderingto physics to editor tools.
	
	The system uses reflection, code generation, and configuration files to build projects.
	
	Written in strict C11 x64 for Windows (but designed with cross platfrom abstraction for future portability).
	
	Designed to be small and lean towards simplicity in implementation and complexity.
	Use less memory allocation and lean on static generated data for fast access.
	
# Major Features

	1. Modular plugin-based game engine architecture.
	
		- Built from hot-swappable modules that can also link statically/monolithically.
		- A minimal native loader bootstraps a fully modular system.
		- Stable C ABI for all module/plugin boundaries (no forced function tables)
	
	1. Reflection Tool
	
		- Pre-build tool for reflection/codegen (fast, deterministic, C-friendly)		
		- Run before project compilation to generate required files.
		- Macro based tagging in source files that used by generator but compiled away in builds.
	
	3. Scripting Language
	
		- C as a scripting language that compiles to native code and hot-reloads.
		- Uses an optional interpreter for C as a scripting language (slower, but crash proof).
	
	4. ...
	
	
# Major Features Documents

	1. ARCH_MODULAR.md for modular architecutre design.
	
	2. ARCH_REFLECTION.md for reflection architecture design.
	
	3. ARCH_SCRIPTING.md for scripting architecture design.
	
	4. 
	
	
# Engine 

	engine.c 	# main source for engine logic
	runtime.c 	# code for managing runtime modules 
	game.c 		# game speficic logic and entry point
	platform.c 	# platform specific intialization and shutdown.
	