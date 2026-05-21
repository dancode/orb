/*==============================================================================================

    build_tool_targets.c -- Central registry for all buildable artifacts.

==============================================================================================*/

target_info_t g_targets[] = {
    {
        .name       = "orb_base",
        .type       = TARGET_STATIC_LIB,
        .root_dir   = "source/base",
        .sln_folder = "01_Library",
        .units      = { "base.c" },
        .unit_count = 1,
    },
    {
        .name       = "sb_base_custom",
        .type       = TARGET_EXECUTABLE,
        .root_dir   = "source/base",
        .sln_folder = "02_Sandbox",
        .units      = { "base_main.c" },
        .unit_count = 1,
    },
    {
        .name       = "build_tool",
        .type       = TARGET_EXECUTABLE,
        .root_dir   = "source/tools/build_tool",
        .sln_folder = "00_Build",
        .units      = { "build_tool.c" },
        .unit_count = 1,
    },
};

int g_target_count = sizeof( g_targets ) / sizeof( g_targets[ 0 ] );

/*============================================================================================*/