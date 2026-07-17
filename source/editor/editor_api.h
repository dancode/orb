#ifndef EDITOR_API_H
#define EDITOR_API_H
/*==============================================================================================

    editor/editor_api.h -- editor service API struct and gateway macro.

    The editor framework: a STATIC SERVICE (RUN_SERVICE in the host's module list) that
    owns the editor shell -- main menu bar, dockspace, the official editor windows -- and
    ALL interaction with the game runner: project bind, the per-frame session tick (whose
    run_view_t carries the scene viewport's render target), and the session verbs behind
    the Game window.  The host is a configuration/launch shell: it boots the runtime,
    forwards the loop callbacks here, and never touches game() itself -- keeping
    host_editor shaped like host_game with the editor service layered on top.

        editor()->project_bind( name, dir );  // on_ready, from -project/-module args
        editor()->update( dt );           // on_update: session tick + viewport + pacing
        editor()->build_gui( dt );        // on_gui
        editor()->shutdown();             // every quit path, before run_host teardown

==============================================================================================*/

#include "editor/editor.h"
#include "engine/mod/mod_import.h"

// clang-format off
/*==============================================================================================
    API Struct
==============================================================================================*/

typedef struct editor_api_s
{
    /* Bind the loaded project DLL to the game runner and adopt it as the editor's
       session subject.  `dir` is the directory holding <name>.dll ("" or NULL = host exe
       dir) -- kept so Play Standalone can hand the same project to host_game.exe.
       false = the project has no api (not loaded / wrong contract). */
    bool ( *project_bind )( const char* name, const char* dir );

    /* Per-frame, from the host's on_update: ticks the session (run_view_t built here --
       render_ctx is the scene viewport's target, the play-in-editor seam), maintains the
       viewport target, and drives the pacing gates (run_host realtime, gui force-redraw). */
    void ( *update       )( f32 dt );

    void ( *build_gui    )( f32 dt );            /* menu bar + dockspace + editor windows */

    /* Stop any live session and release GPU-backed editor resources (the scene viewport
       target).  Call on every host quit path BEFORE run_host teardown destroys the
       device -- the module-exit backstop runs after rhi shutdown, too late.  Idempotent. */
    void ( *shutdown     )( void );

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
