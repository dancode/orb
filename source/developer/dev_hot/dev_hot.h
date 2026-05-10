#ifndef DEV_HOT_H
#define DEV_HOT_H
/*==============================================================================================

    dev_hot.h : Developer hot-reload convenience service.

    Wraps dev_build + mod_reload into a single call any host can use.
    Automatically selects Debug or Release from the compile-time flags in orb.h.

    Typical host setup:

        dev_hot_init( NULL, NULL );                 // auto-detect cmake + build dir

    Typical input loop:

        if ( key_pressed( KEY_C ) ) dev_hot_recompile( "example" );
        if ( key_pressed( KEY_R ) ) dev_hot_reload_all();

    How To Use

    3. Wire to whatever input the host uses

        dev_hot_recompile( "render" );   // build + reload
        dev_hot_reload( "render" );      // reload only (built externally)
        dev_hot_reload_all();            // reload everything

    Includes future work such as:
   
    - Pausing the tick loop before a reload so modules don't get ticked mid-swap.
    - Surfacing build output to a dev console or overlay rather than stdout.
    - Respecting module dependency order when reloading several modules at once.
    - Potentially rolling back to the previous shadow DLL if the new one fails to initialize. 

==============================================================================================*/
#include "orb.h"

/* Boot the service.
   build_dir  : absolute path to the cmake build directory, or NULL to auto-detect.
   cmake_path : absolute path to cmake.exe, or NULL to use "cmake" on PATH.
   Both NULL is the normal case. */
bool dev_hot_init( const char* build_dir, const char* cmake_path );

/* Build a module with cmake, then enqueue a hot-reload.
   The actual DLL swap happens at the next mod_system_flush_reloads() call —
   typically the end of the host's main-loop iteration. */
bool dev_hot_recompile( const char* module_name );

/* Reload a module that was already rebuilt externally — skips the build step. */
bool dev_hot_reload( const char* module_name );

/* Reload every dynamic module without rebuilding.
   Returns the number of modules successfully reloaded. */
int dev_hot_reload_all( void );

/*============================================================================================*/
#endif    // DEV_HOT_H