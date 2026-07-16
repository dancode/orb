#ifndef EDITOR_API_H
#define EDITOR_API_H
/*==============================================================================================

    editor/editor_api.h -- editor service API struct and gateway macro.

    The editor framework: a STATIC SERVICE (RUN_SERVICE in the host's module list) that
    owns the editor shell -- main menu bar, dockspace, the official editor windows -- and
    the scene viewport's play-in-editor plumbing.  The host stays policy: it forwards
    update/build_gui and builds the project's run_view_t from view_ctx/view_size, keeping
    session verbs (game()->play/stop) host-visible.

        editor()->set_project( name );          // on_ready, after game()->project_bind
        editor()->update( dt );                 // on_update, AFTER game()->tick
        editor()->build_gui( dt );              // on_gui
        view.render_ctx = editor()->view_ctx(); // scene viewport target, or -1 headless

==============================================================================================*/

#include "editor/editor.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct editor_api_s
{
    void ( *set_project )( const char* name );   /* bound project display name; NULL = none  */
    void ( *update      )( f32 dt );             /* pre-gui maintenance (scene viewport)     */
    void ( *build_gui   )( f32 dt );             /* menu bar + dockspace + editor windows    */

    /* The play-in-editor seam: what the host writes into the project's run_view_t each
       frame.  view_ctx is the scene viewport's render target id, or -1 (headless) while
       no target exists or the viewport window is hidden. */
    i32  ( *view_ctx    )( void );
    void ( *view_size   )( i32* w, i32* h );

    /* Release GPU-backed editor resources (the scene viewport target).  Call on every
       host quit path BEFORE run_host teardown destroys the device -- the module-exit
       backstop runs after rhi shutdown, too late to free cleanly.  Idempotent. */
    void ( *shutdown    )( void );

} editor_api_t;

#if defined( BUILD_STATIC ) || defined( EDITOR_STATIC )
MOD_GATEWAY_STATIC( editor_api_t, editor )
    #define MOD_USE_EDITOR    /* static build */
    #define MOD_FETCH_EDITOR  true
#else
MOD_GATEWAY_DYNAMIC( editor_api_t, editor )
    #define MOD_USE_EDITOR    MOD_DEFINE_API_PTR( editor_api_t, editor )
    #define MOD_FETCH_EDITOR  MOD_FETCH_API( editor_api_t, editor )
#endif

// clang-format on
/*============================================================================================*/
#endif    // EDITOR_API_H
