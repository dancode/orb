#ifndef RUN_PROJECT_H
#define RUN_PROJECT_H
/*==============================================================================================

    runtime/run_project.h -- the PROJECT CONTRACT: the vtable every project DLL exposes
    as its mod_desc_t func_api.

    A project DLL is the user's program built on the engine -- usually a game
    (source/project/<name> in-tree, or a child project created with
    `build_tool -create <name> -type project`), but the contract is deliberately
    runtime-level: a project may skip the game framework entirely and depend on just
    core + render (a visualizer, a tool, a demo scene).  It is a Tier-3 FORCE-DYNAMIC
    module: always a DLL, even under BUILD_STATIC -- the runtime loads it at boot with
    mod_dynamic_load_dir( name, dir ) from the -project / -module launch args and
    hot-reloads it, but never calls into it.  A DRIVER does:

        const run_project_api_t* proj = mod_get_api( name );   // stable across hot-reload
        proj->on_start();
        while ( running )
        {
            for ( acc += dt; acc >= fixed_dt; acc -= fixed_dt )   // fixed-step sim
                proj->on_sim( fixed_dt );
            proj->on_frame( dt, &view );
            proj->on_draw( acc / fixed_dt, &view );               // alpha interpolates
        }
        proj->on_stop();

    The game framework module (game/game_api.h) is the STANDARD driver -- it owns the
    play state machine and the fixed-step clock, and hosts drive it with one call per
    frame.  A custom host may equally drive the vtable directly as above (or simpler:
    one on_sim( dt ) per frame and alpha = 1 when determinism does not matter).

    Phases
    ------
    on_start          play begins (host boot / editor Play).
    on_sim( fdt )     fixed-step simulation -- 0..N calls per frame, always the same fdt.
                      The ONLY place play state advances; keep it deterministic (no wall
                      clock, no surface size) and netcode/replay stay possible.
    on_frame( dt, v ) once per frame, variable dt -- cameras, effects, input polling,
                      anything framerate-bound.
    on_draw( a, v )   scene submission.  a = accumulator fraction [0,1): lerp prev->cur
                      sim states by a for smooth motion at any sim rate (or ignore it).
    on_stop           play ends (close / editor Stop).

    Rules (enforced by the module loader):
      - Every slot must be non-NULL (api_validate rejects NULL function pointers).
      - Adding/removing functions changes func_api_size: fine on a full restart, but a
        hot-reload with a changed size is rejected -- restart the host instead.

    The view struct is passed EVERY call rather than cached at on_start so a hot-reloaded
    DLL can never hold a stale handle.  It is versioned so fields can be appended without
    touching func_api_size: check view->version before reading fields newer than the
    version you compiled against.  This is also the play-in-editor seam -- the editor
    hands the project a viewport's render context instead of the main window's.

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

#define RUN_VIEW_VERSION 1

typedef struct run_view_s
{
    i32 version;      // RUN_VIEW_VERSION of the writing driver
    i32 render_ctx;   // rhi context id to submit scene draws into; -1 = headless
    i32 surface_w;    // drawable surface size in pixels; 0 when headless.  Handed in so
    i32 surface_h;    // projects can place scene draws without depending on rhi directly.

} run_view_t;

typedef struct run_project_api_s
{
    void ( *on_start )( void );                               // play begins
    void ( *on_sim   )( f32 fixed_dt );                       // 0..N per frame, deterministic
    void ( *on_frame )( f32 dt, const run_view_t* view );     // once per frame, variable rate
    void ( *on_draw  )( f32 alpha, const run_view_t* view );  // scene submission, interpolated
    void ( *on_stop  )( void );                               // play ends

} run_project_api_t;

/*============================================================================================*/
#endif    // RUN_PROJECT_H
