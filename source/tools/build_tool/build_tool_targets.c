/*==============================================================================================

    build_tool_targets.c -- Central registry for all buildable artifacts.

    This file contains the "data" portion of the build system. It defines the
    entire dependency graph, source layout, and IDE solution mappings.

    Structure:
    1. Target Pool (g_targets): The master list of everything the orchestrator can build.
    2. Solution Registry (g_solutions): Groups of targets mapped to specific .sln files.

    How to add a new library:
    - Add an entry to g_targets[].
    - Add its name to the relevant solution target list (e.g. g_sln_main_targets).

==============================================================================================*/

target_info_t g_targets[] = {

    // --- 01_BASE (Foundation) ---

    // The lowest layer. Stateless and dependency-free.
    // Produces bin/base.lib.
    {
     .name       = "base",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/base",
     .sln_folder = "01_BASE",
     .units      = { "base.c" },
     .deps       = {},
     },

    // --- 02_ENGINE (Stateful Systems) ---

    // OS Abstractions (Files, Threads, DLLs).
    {
     .name       = "sys",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/sys",
     .sln_folder = "02_ENGINE",
     .units      = { "sys.c" },
     .deps       = {},
     },

    // Reflection System (Metadata registry).
    {
     .name       = "rs",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/rs",
     .sln_folder = "02_ENGINE",
     .units      = { "rs.c" },
     .deps       = {},
     },

    // Module System (Hot-reloading).
    {
     .name       = "mod",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/mod",
     .sln_folder = "02_ENGINE",
     .units      = { "mod.c" },
     .deps       = { "sys" },
     },

    // Application Layer (Windows, Events).
    {
     .name       = "app",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/app",
     .sln_folder = "02_ENGINE",
     .units      = { "app.c" },
     .deps       = { "sys" },
     },

    // Core Engine Services (Logging, Memory Arenas).
    // This target requires reflect_tool.exe to run before it can compile.
    {
     .name        = "core",
     .type        = TARGET_STATIC_LIB,
     .root_dir    = "source/engine/core",
     .sln_folder  = "02_ENGINE",
     .units       = { "core.c" },
     .deps        = { "sys", "rs" },
     .has_reflect = true,
     // .reflect_name   = "engine_core",
    },

    // --- 03_RUNTIME_MODULES (Hot-Reloadable DLLs) ---

    // An example module to verify hot-reloading.
    // Produces bin/example.dll and bin/example.lib (implib).
    {
     .name       = "example",
     .type       = TARGET_DYNAMIC_LIB,
     .root_dir   = "source/runtime_modules/example",
     .sln_folder = "03_RUNTIME_MODULES",
     .units      = { "example.c" },
     .deps       = {},
     },

    // --- 02_SANDBOX (Verification Targets) ---
    // A minimal executable to test the base library.
    {
     .name       = "sb_base_main",
     .type       = TARGET_EXECUTABLE,
     .root_dir   = "source/base",
     .sln_folder = "02_SANDBOX",
     .units      = { "base_main.c" },
     .deps       = { "base" },
     },

    // --- 08_TOOL (Development Utilities) ---
    // The build tool itself (this program!).
    {
     .name          = "build_tool",
     .type          = TARGET_EXECUTABLE,
     .root_dir      = "source/tools/build_tool",
     .sln_folder    = "08_TOOL",
     .units         = { "build_tool.c" },
     .is_build_tool = true,
     .deps          = {},
     .is_tool       = true,
     },

    // The reflection generator. Scans source and writes generated .c/.h files.
    {
     .name            = "reflect_tool",
     .type            = TARGET_EXECUTABLE,
     .root_dir        = "source/tools/reflect_tool",
     .sln_folder      = "08_TOOL",
     .units           = { "reflect_tool.c" },
     .deps            = {},
     .is_tool         = true,
     .is_reflect_tool = true,
     },
};

int g_target_count = sizeof( g_targets ) / sizeof( g_targets[ 0 ] );

// =============================================================================
// --- Solution Registry --- These lists are NULL-terminated.
// =============================================================================

// Main engine workspace. Includes core libraries and sandboxes.
static const char* g_sln_main_targets[] = { "base",        "sys", "rs", "mod", "app", "core", "example",
                                            "sb_base_main", NULL };

// Standalone build tools workspace. For modifying the build system itself.
static const char* g_sln_tools_targets[] = { "build_tool", "reflect_tool", NULL };

// Map solutions to their target lists and navigation scope.
solution_info_t g_solutions[] = {
    {
     .name          = "orb",
     .target_names  = g_sln_main_targets,
     .nav_dir       = "source",
     .out_dir       = "build\\proj",
     .is_monolithic = false,
    },
    {
     .name          = "orb_mono",
     .target_names  = g_sln_main_targets,
     .nav_dir       = "source",
     .out_dir       = "build\\proj_mono",
     .is_monolithic = true,
    },
    {
     .name          = "orb_build",
     .target_names  = g_sln_tools_targets,
     .nav_dir       = NULL,
     .out_dir       = "build\\proj",
     .is_monolithic = false,
    },
};

int g_solution_count = sizeof( g_solutions ) / sizeof( g_solutions[ 0 ] );

// =============================================================================
// --- Warning Suppression Table ---
// =============================================================================

// Applied globally after the base flag set in build_target_compile().
// Each entry fires only when the active config AND compiler match the masks.
// Platform specificity is implicit in compiler_mask: MSVC flags are Windows-only
// by definition; add COMPILE_CLANG entries for clang-cl variants as needed.

warn_suppress_t g_warn_suppressions[] = {

    // Release: assert() and similar macros compile out, leaving unreferenced
    // locals and parameters that were only referenced in the debug expression.

    {"/wd4101", CONFIG_COUNT, COMPILE_MSVC}, // C4101: unreferenced local variable
    {"/wd4189", CONFIG_COUNT, COMPILE_MSVC}, // C4189: local variable initialized but not  referenced
    {"/wd4100", CONFIG_COUNT, COMPILE_MSVC}, // C4100: unreferenced formal parameter
};

int g_warn_suppression_count = sizeof( g_warn_suppressions ) / sizeof( g_warn_suppressions[ 0 ] );

// =============================================================================
// --- Compile Define Tables ---
//
// Single source of truth for preprocessor defines. Both build_tool_cc.c
// (cl.exe invocation) and build_tool_gen.c (IntelliSense vcxproj emission)
// iterate these arrays so the two consumers can never silently diverge.
//
// Do NOT put platform/compiler identity defines here (OS_WINDOWS, COMPILER_MSVC,
// ARCH_X64). Those are owned by source/orb.h and detected at compile time via
// MSVC predefined macros. They belong in source, not in the build tool.
// =============================================================================

const char* g_defines_always[] = {
    "_CRT_SECURE_NO_WARNINGS",
    NULL,
};

const char* g_defines_debug[] = {
    "_DEBUG",
    NULL,
};

const char* g_defines_release[] = {
    "NDEBUG",
    NULL,
};

// Subset of compile flags that the IntelliSense parser needs to match cl.exe's
// language and conformance behavior. Excludes pure-build flags (/c /nologo /W4
// /WX /Zi /Od /O2 /MD etc.) that have no effect on IntelliSense parsing.

const char* g_intellisense_flags[] = {
    "/std:c11",
    "/Zc:preprocessor",
    NULL,
};

/*============================================================================================*/