/*==============================================================================================

    editor.c -- Unity build entry for the editor framework static service.

    The editor framework sits above the runtime services and the game runner: it owns
    the editor shell (menu bar, dockspace, official windows) and the scene viewport's
    play-in-editor plumbing.  The host stays policy -- it forwards update/build_gui and
    builds the project's run_view_t from editor()->view_ctx/view_size.

    Layering
    --------
        gui / render / game            <- runtime services + runner, consumed via gateways
            ^
            | gui()->... render()->... game()->...
            |
        editor (this static service)   <- shell + editor windows + viewport plumbing
            ^
            | editor()->...
            |
        host_editor on_update/on_gui   <- forwards; owns the loop and the session policy

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#define LOG_CH "editor"

#include "engine/mod/mod_export.h"
#include "engine/sys/sys_host.h"    /* sys_exe_dir + sys_process_spawn (Play Standalone) */
#include "engine/core/core_api.h"

#include "runtime_service/gui/gui_api.h"
#include "runtime_modules/render/render_api.h"

#include "game/game_api.h"

#include "runtime/run_api.h"
#include "runtime/run_host.h"

#include "editor_service/viewport/viewport.h"
#include "editor/editor_api.h"

/*==============================================================================================
    Unity build
==============================================================================================*/

/* Implementation files go here:
   #include "editor/editor_feature.c" */

/*==============================================================================================
    Public API wiring  (must be last -- all implementations must be in scope)
==============================================================================================*/

#ifndef EDITOR_API_C_PRELUDE
#include "editor/editor_api.c"
#endif

/*============================================================================================*/
    