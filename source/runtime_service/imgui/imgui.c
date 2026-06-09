/*==============================================================================================

    runtime_service/imgui/imgui.c -- Unity build entry for the imgui module.

    Include order matters: each file can reference statics from files included above it.
        imgui_shader.h  -- embedded SPIR-V arrays (s_imgui_vert_spirv, s_imgui_frag_spirv)
        imgui_font.c    -- font atlas: font_init/shutdown/glyph + s_atlas_idx
        imgui_draw.c    -- CPU draw list: draw_reset, draw_push_*, s_draw
        imgui_render.c  -- GPU flush: render_init/shutdown/flush
        imgui_input.c   -- app->IO snapshot: input_update, s_io
        imgui_ctx.c     -- hot/active/focused state: ctx_new_frame, id_hash, rect_hit, s_ctx
        imgui_widget.c  -- widget implementations: widget_*
        imgui_api.c     -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <string.h> /* for memset */

#include "orb.h"

// internal API headers
#include "engine/mod/mod_export.h"
#include "runtime_service/imgui/imgui_host.h"

// API function headers
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

// API access pointers -- wired at module init/reload time
MOD_USE_RHI;
MOD_USE_APP;

/*==============================================================================================
    Unity build
==============================================================================================*/

#include "runtime_service/imgui/imgui_shader.h"
#include "runtime_service/imgui/imgui_font.c"
#include "runtime_service/imgui/imgui_draw.c"
#include "runtime_service/imgui/imgui_render.c"
#include "runtime_service/imgui/imgui_input.c"
#include "runtime_service/imgui/imgui_ctx.c"
#include "runtime_service/imgui/imgui_widget.c"
#include "runtime_service/imgui/imgui_api.c"

/*============================================================================================*/
