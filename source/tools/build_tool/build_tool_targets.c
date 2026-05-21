/*==============================================================================================

    build_tool_targets.c -- Central registry for all buildable artifacts.

    This file contains the "data" portion of the build system. It defines the 
    entire dependency graph and source layout for the engine.

    To add a new library or executable to the project, simply add a new 
    target_info_t entry to the g_targets array below.

==============================================================================================*/

target_info_t g_targets[] = {
    // --- 01_BASE (Foundation) ---
    // The lowest layer. Stateless and dependency-free.
    {
     .name       = "base",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/base",
     .sln_folder = "01_BASE",
     .units      = { "base.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,
     },

    // --- 02_ENGINE (Stateful Systems) ---
    
    // OS Abstractions (Files, Threads, DLLs).
    {
     .name       = "sys",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/sys",
     .sln_folder = "02_ENGINE",
     .units      = { "sys.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,
     },

    // Reflection System (Metadata registry).
    {
     .name       = "rs",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/rs",
     .sln_folder = "02_ENGINE",
     .units      = { "rs.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,
     },

    // Module System (Hot-reloading).
    {
     .name       = "mod",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/mod",
     .sln_folder = "02_ENGINE",
     .units      = { "mod.c" },
     .unit_count = 1,
     .deps       = { "sys" },
     .dep_count  = 1,
     },

    // Application Layer (Windows, Events).
    {
     .name       = "app",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/app",
     .sln_folder = "02_ENGINE",
     .units      = { "app.c" },
     .unit_count = 1,
     .deps       = { "sys" },
     .dep_count  = 1,
     },

    // Core Engine Services (Logging, Memory Arenas).
    // Note: This target uses 'has_reflect' which triggers the reflection generator.
    {
     .name           = "core",
     .type           = TARGET_STATIC_LIB,
     .root_dir       = "source/engine/core",
     .sln_folder     = "02_ENGINE",
     .units          = { "core.c" },
     .unit_count     = 1,
     .deps           = { "sys", "rs" },
     .dep_count      = 2,
     .tool_deps      = { "build_reflect" },
     .tool_dep_count = 1,
     .has_reflect    = true,
     .reflect_name   = "engine_core",
     },
    // --- 03_RUNTIME_MODULES (Hot-Reloadable DLLs) ---
    
    // An example module to verify hot-reloading.
    {
     .name       = "example",
     .type       = TARGET_DYNAMIC_LIB,
     .root_dir   = "source/runtime_modules/example",
     .sln_folder = "03_RUNTIME_MODULES",
     .units      = { "example.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,
     },

    // --- 02_SANDBOX (Verification Targets) ---

    // A minimal executable to test the base library.
    {
     .name       = "sb_base_custom",
     .type       = TARGET_EXECUTABLE,
     .root_dir   = "source/base",
     .sln_folder = "02_SANDBOX",
     .units      = { "base_main.c" },
     .unit_count = 1,
     .deps       = { "base" },
     .dep_count  = 1,
     },

    // --- 08_TOOL (Development Utilities) ---

    // The build tool itself (this program!).
    {
     .name       = "build_tool",
     .type       = TARGET_EXECUTABLE,
     .root_dir   = "source/tools/build_tool",
     .sln_folder = "08_TOOL",
     .units      = { "build_tool.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,     
     },

    // The reflection generator. Must be built first!
    {
     .name       = "build_reflect",
     .type       = TARGET_EXECUTABLE,
     .root_dir   = "source/tools/rs_gen",
     .sln_folder = "08_TOOL",
     .units      = { "rs_gen.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,
     },
};

int g_target_count = sizeof( g_targets ) / sizeof( g_targets[ 0 ] );

// --- Solution Registry ---

// Main engine workspace.
static const char* g_sln_main_targets[] = {
    "base", "sys", "rs", "mod", "app", "core", "example", "sb_base_custom", NULL
};

// Standalone build tools workspace.
static const char* g_sln_tools_targets[] = {
    "build_tool", "build_reflect", NULL
};

solution_info_t g_solutions[] = {
    {
     .name         = "orb_make",
     .target_names = g_sln_main_targets,
     .nav_dir      = "source", // Includes everything in source/ for navigation.
     },
    {
     .name         = "orb_build",
     .target_names = g_sln_tools_targets,
     .nav_dir      = NULL, // No extra navigation project needed for tools.
     },
};

int g_solution_count = sizeof( g_solutions ) / sizeof( g_solutions[ 0 ] );

/*============================================================================================*/