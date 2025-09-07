# The Glowing Orb (game engine)

A modular game engine written in strict C11 that has robust auto generated reflection and uses C as its scripting language.

## Directory Structure

ORB/
├─ CMakeLists.txt
├─ build_vs.bat                 # generates Visual Studio solution
├─ game.toml                    # project manifest
│
├─ loader/                      # ultra minimal runtime loader
│  ├─ CMakeLists.txt
│  ├─ loader.c
│  └─ toml.h                    # tiny TOML reader (header-only)
│
├─ engine/
│  └─ modules/
│	  └─ core/
│	     ├─ CMakeLists.txt
│	     ├─ core.c
│	     └─ module.h
│
└─ projects/
   └─ demo_game/
      ├─ CMakeLists.txt
      └─ demo_game.c
	  
