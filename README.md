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
  base/ 			# standard library without state (mostly pure functions)
  core/				# essential systems other modules rely on and allocated shared memory.
  platform/			# lower level cross platform system code.
  system/			# engine systems each in its own module.
    render/
    audio/
    physics/
    animation/
  test/				# engine tests.
  runtime/			# the executable logic to bootstrap the engine (future planning)

projects/
	sample_game/	# game dll loaded by module system (sample engine game project)
```
