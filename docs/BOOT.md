## Boot Sequence Diagram

```
┌──────────────────────┐
│   orb_runtime.exe    │ ← ultra-minimal loader (shim)
└───────────┬──────────┘
            │
            ▼
    [ Step 1: Read Config ]
   ┌──────────────────────────┐
   │  Open game.oml           │
   │  Parse with oml.h        │
   │  Extract:                │
   │    search_paths[]        │
   │    modules[]             │
   └───────────┬──────────────┘
               │
               ▼
    [ Step 2: Resolve Paths ]
   ┌──────────────────────────┐
   │ For each search_path:    │
   │   Try locating module.dll│
   │   If found → use path    │
   └───────────┬──────────────┘
               │
               ▼
    [ Step 3: Dynamic Load ]
   ┌──────────────────────────┐
   │ LoadLibrary(module.dll)  │
   │ GetProcAddress(init)     │
   │ GetProcAddress(tick)     │
   │ GetProcAddress(shutdown) │
   └───────────┬──────────────┘
               │
               ▼
    [ Step 4: Module Init ]
   ┌──────────────────────────┐
   │ core.dll → core_init()   │
   │ mygame.dll → game_init() │
   └───────────┬──────────────┘
               │
               ▼
    [ Step 5: Main Loop ]
   ┌──────────────────────────┐
   │ while(running) {         │
   │   tick all modules       │
   │ }                        │
   └───────────┬──────────────┘
               │
               ▼
    [ Step 6: Shutdown ]
   ┌──────────────────────────┐
   │ Call shutdown on modules │
   │ Unload DLLs              │
   │ Exit process             │
   └──────────────────────────┘
   
   ```
   
## Key Insight

```
  
* Loader knows nothing about the engine/game. It only:

	1. Reads TOML config.
	2. Resolves search paths.
	3. Loads DLLs dynamically.
	4. Calls known entry points (init, tick, shutdown).
	
* Modules provide all functionality:

	* core.dll → minimal runtime API (logging, memory, etc.).
	* game.dll → game logic built against core.

Separation of concerns:

	* Loader = 100 lines of C.
	* TOML parser = ~200 lines of C.
	* Core + Game = independent, swappable DLLs.


