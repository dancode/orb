#ifndef GAME_PROJECT_H
#define GAME_PROJECT_H
/*==============================================================================================

    game/game_project.h -- the PROJECT CONTRACT: the vtable every game project DLL exposes
    as its mod_desc_t func_api.

    A project DLL is the user's game (source/project/<name> in-tree, or a child project
    created with `build_tool -create <name> -type project`).  It is a Tier-3 FORCE-DYNAMIC
    module: always a DLL, even under BUILD_STATIC -- hosts load it at runtime with
    mod_dynamic_load_dir( name, dir ) from -project / -module launch args and drive it
    through this struct:

        const game_project_api_t* proj = mod_get_api( name );   // stable across hot-reload
        proj->on_start();
        proj->on_update( dt, &ctx );        // every frame
        proj->on_stop();

    Projects declare deps { "core", "game" } minimum, plus "render" if they draw.  The
    canonical implementation (and the -create scaffold template) is
    source/project/sample_game/sample_game.c.

    Rules (enforced by the module loader):
      - Every slot must be non-NULL (api_validate rejects NULL function pointers).
      - Adding/removing functions changes func_api_size: fine on a full restart, but a
        hot-reload with a changed size is rejected -- restart the host instead.

    The ctx struct is passed EVERY update rather than cached at on_start so a hot-reloaded
    DLL can never hold a stale handle.  It is versioned so fields can be appended without
    touching func_api_size: check ctx->version before reading fields newer than the
    version you compiled against.  This is also the play-in-editor seam -- the editor
    hands the project a viewport's render context instead of the main window's.

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

#define GAME_PROJECT_CTX_VERSION 1

typedef struct game_project_ctx_s
{
    i32 version;      // GAME_PROJECT_CTX_VERSION of the writing host
    i32 render_ctx;   // rhi context id to submit scene draws into; -1 = headless
    i32 surface_w;    // drawable surface size in pixels; 0 when headless.  Handed in so
    i32 surface_h;    // projects can place scene draws without depending on rhi directly.

} game_project_ctx_t;

typedef struct game_project_api_s
{
    void ( *on_start  )( void );                                    // play begins (host start / editor Play)
    void ( *on_update )( f32 dt, const game_project_ctx_t* ctx );   // per-frame sim + scene submission
    void ( *on_stop   )( void );                                    // play ends (close / editor Stop)

} game_project_api_t;

/*============================================================================================*/
#endif    // GAME_PROJECT_H
