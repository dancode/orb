/*==============================================================================================

    build_tool_targets.c -- Central registry for all buildable artifacts.

==============================================================================================*/

target_info_t g_targets[] = {
    // --- 01_Library (Engine Foundation) ---
    {
     .name       = "orb_base",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/base",
     .sln_folder = "01_BASE",
     .units      = { "base.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,
     },
    {
     .name       = "orb_sys",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/sys",
     .sln_folder = "02_ENGINE",
     .units      = { "sys.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,
     },
    {
     .name       = "orb_rs",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/rs",
     .sln_folder = "02_ENGINE",
     .units      = { "rs.c" },
     .unit_count = 1,
     .deps       = {},
     .dep_count  = 0,
     },
    {
     .name       = "orb_mod",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/mod",
     .sln_folder = "02_ENGINE",
     .units      = { "mod.c" },
     .unit_count = 1,
     .deps       = { "orb_sys" },
     .dep_count  = 1,
     },
    {
     .name       = "orb_app",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/app",
     .sln_folder = "02_ENGINE",
     .units      = { "app.c" },
     .unit_count = 1,
     .deps       = { "orb_sys" },
     .dep_count  = 1,
     },
    {
     .name       = "orb_core",
     .type       = TARGET_STATIC_LIB,
     .root_dir   = "source/engine/core",
     .sln_folder = "02_ENGINE",
     .units      = { "core.c" },
     .unit_count = 1,
     .deps       = { "orb_sys", "orb_rs" },
     .dep_count  = 2,
     },

    // --- 02_Library (Developer Tools) ---
    // {
    //  .name       = "orb_dev_hot",
    //  .type       = TARGET_STATIC_LIB,
    //  .root_dir   = "source/developer/dev_hot",
    //  .sln_folder = "01_Library",
    //  .units      = { "dev_hot.c" },
    //  .unit_count = 1,
    //  .deps       = { "orb_base", "orb_sys", "orb_core", "orb_mod" },
    //  .dep_count  = 4,
    //  },

    // --- 03_Modules ---
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

    // --- 02_Sandbox ---
    {
     .name       = "sb_base_custom",
     .type       = TARGET_EXECUTABLE,
     .root_dir   = "source/base",
     .sln_folder = "02_SANDBOX",
     .units      = { "base_main.c" },
     .unit_count = 1,
     .deps       = { "orb_base" },
     .dep_count  = 1,
     },
    // {
    //  .name       = "sb_engine_mod",
    //  .type       = TARGET_EXECUTABLE,
    //  .root_dir   = "source/sandbox/engine/engine_mod",
    //  .sln_folder = "02_Sandbox",
    //  .units      = { "sb_engine_mod.c" },
    //  .unit_count = 1,
    //  .deps       = { "orb_base", "orb_sys", "orb_core", "orb_mod", "orb_dev_hot" },
    //  .dep_count  = 5,
    //  },

    // --- 00_Build ---
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
};

int g_target_count = sizeof( g_targets ) / sizeof( g_targets[ 0 ] );

/*============================================================================================*/