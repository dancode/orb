/*==============================================================================================

    dev_hot.c : Developer hot-reload convenience service.

    Selects build configuration from orb.h compile-time flags:
        DEBUG   1 / RELEASE 0  →  "Debug"
        DEBUG   0 / RELEASE 1  →  "Release"

    No other configuration is required from the host.

==============================================================================================*/

#include <stdio.h>

#include "orb.h"
#include "engine/mod/mod.h"
#include "developer/dev_build/dev_build.h"
#include "dev_hot.h"

/*==============================================================================================
    Build configuration resolved at compile time
==============================================================================================*/

#if RELEASE
    #define DEV_HOT_BUILD_CONFIG DEV_BUILD_RELEASE
    #define DEV_HOT_CONFIG_STR   "Release"
#else
    #define DEV_HOT_BUILD_CONFIG DEV_BUILD_DEBUG
    #define DEV_HOT_CONFIG_STR   "Debug"
#endif

/*==============================================================================================
    Init
==============================================================================================*/

bool
dev_hot_init( const char* build_dir, const char* cmake_path )
{
    dev_build_settings_t s = {
        .build_dir      = build_dir,  /* NULL  auto-detects from exe location */
        .cmake_path     = cmake_path, /* NULL  uses "cmake" on PATH           */
        .config         = DEV_HOT_BUILD_CONFIG,
        .capture_output = true,
    };

    if ( !dev_build_init( &s ) )
    {
        printf( "[dev_hot] init failed: %s\n", dev_build_last_error() );
        return false;
    }

    printf( "[dev_hot] ready (config: %s)\n", DEV_HOT_CONFIG_STR );
    return true;
}

/*==============================================================================================
    Build + reload
==============================================================================================*/

bool
dev_hot_recompile( const char* module_name )
{
    if ( !module_name || !*module_name )
    {
        printf( "[dev_hot] recompile: module name required\n" );
        return false;
    }

    dev_build_result_t r;
    if ( !dev_build_module( module_name, &r ) )
    {
        printf( "[dev_hot] '%s': could not launch build — %s\n", module_name, dev_build_last_error() );
        return false;
    }

    if ( !r.success )
    {
        printf( "[dev_hot] '%s': build FAILED (exit %d, %.2fs)\n%.*s\n", module_name, r.exit_code,
                r.elapsed_seconds, r.log_len, r.log );
        return false;
    }

    printf( "[dev_hot] '%s': built in %.2fs — reload queued for next frame\n", module_name, r.elapsed_seconds );

    if ( !mod_reload( module_name ) )
    {
        printf( "[dev_hot] '%s': could not queue reload — %s\n", module_name, mod_last_error() );
        return false;
    }

    return true;
}

bool
dev_hot_reload( const char* module_name )
{
    if ( !module_name || !*module_name )
    {
        printf( "[dev_hot] reload: module name required\n" );
        return false;
    }

    if ( !mod_reload( module_name ) )
    {
        printf( "[dev_hot] '%s': reload failed - %s\n", module_name, mod_last_error() );
        return false;
    }

    return true;
}

int
dev_hot_reload_all( void )
{
    int n = mod_reload_all();
    printf( "[dev_hot] reloaded %d module(s)\n", n );
    return n;
}

/*============================================================================================*/