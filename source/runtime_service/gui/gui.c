/*==============================================================================================

    runtime_service/gui/gui.c -- GUI_FRAME translation unit: THE FRAME ORCHESTRATOR.

    The top of the stack and nothing else (R11): boots both servers, owns the viewports and
    the app/sys wiring, pumps io into the interact server, hands each surface's GPU pieces to
    the render server at flush, and assembles the module vtable.  Everything with a role of
    its own is one of the ten carved units at this directory's root -- the model, the
    dependency graph, and the role map live in GUI_ARCHITECTURE.md, not here.

    The frame unit owns NO unit header: its public face IS gui.h / gui_api.h / gui_host.h,
    and its few internal seams are forward-declared below the includes.  It is the only unit
    that includes every unit header -- the orchestrator sees the whole stack; the stack never
    sees the orchestrator (the units call up only through the documented upward seams in
    their own headers).

    THIS UNIT's constituents (each carved unit lists its own):

    frame/gui_frame_overlay.c    -- built-in perf / state HUD overlays + the frame-timing helpers they read
                                      (home since R10 -- conductor code, never part of the debug unit)

    frame/gui_frame.c            -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, font, clip
    frame/gui_viewport.c         -- surface record lifecycle (viewport_create/destroy, R11) + viewport open/resize/
                                      close + gui-owned floater lifecycle (spawn/update/render_floaters)
    frame/gui_boot.c             -- one-call host front end: boot, frame_poll, present_begin/present
    frame/gui_pane.c             -- the pane bracket: pane_tag + gui_pane_begin/end stamp BOTH servers (R4)
    frame/gui_context.c          -- public multi-context lifecycle + the context block allocation (R4, R11)

    gui_ui_mem.c                 -- frontend memory accounting (gui_ui_memory) + the gui_mem_stats
                                      aggregation; must be the last constituent include so it sees them all
    gui_api.c                    -- vtable, mod_desc, MOD_DEFINE_EXPORTS

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h> /* va_list / va_start -- printf-style textf() widget       */
#include <math.h>   /* floorf / ceilf -- pixel-grid snapping in draw + scissor */

#include "orb.h"
#include "base/fmt.h"         // fmt_snprintf / fmt_vsnprintf -- CRT-free formatting on the per-frame text paths
#include "base/math.h"        // f32_lerp -- from/to interpolation for the animation service
#include "base/math_ease.h"   // f32_ease_* shapers -- the easing curves the animation service applies

#include "engine/sys/sys_host.h"   // sys_root_dir -- disk assets (load_icon, asset_path) resolve root-relative

/* The orchestrator's world -- everything, in stack order (R11: each carved unit includes only
   the headers at or below its layer; this unit sits on top and includes them all). */
#include "runtime_service/gui/render/gui_render.h"   /* THE RENDER SERVER's surface
                                                        (pulls gui_host.h + rhi/app APIs)   */
#include "runtime_service/gui/core/gui_core.h"       /* THE INTERACT SERVER's services      */
#include "runtime_service/gui/core/gui_ctx.h"        /* ... and its retained-mode storage   */
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/element/gui_element_internal.h"
#include "runtime_service/gui/chrome/gui_chrome.h"
#include "runtime_service/gui/debug/gui_debug.h"

// API function headers + access pointers -- wired at startup.
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"
MOD_USE_RHI;
MOD_USE_APP;

// clang-format off

/* Frame-unit internal seams -- both ends live in THIS translation unit, so the declarations
   live here rather than in any unit header (the frame unit owns no header of its own: its
   public face IS gui.h / gui_api.h / gui_host.h).  Forward-declared because the frame group's
   include order has callers (gui_frame.c) before definers (gui_context.c, gui_viewport.c). */
gui_context_t* ctx_alloc_slot ( const gui_ctx_config_t* c, u32 slots, i32 slot );  /* gui_context.c */
void           ctx_pool_init  ( void );                                           /* gui_context.c */
bool           viewport_create ( gui_viewport_t* vp, rhi_texture_t target, i32 win_id ); /* gui_viewport.c */
void           viewport_destroy( gui_viewport_t* vp );                                   /* gui_viewport.c */

/*==============================================================================================
    Capability flags -- latched by gui_init_config_front (gui_frame.c), read directly (same TU)
    by any file below that owns an optional feature boundary: gui_table.c (tables),
    gui_dock*.c (docking), gui_nav.c (keyboard_nav).  Declared here, before every tier include, so
    all of them see it -- the gui_render.c s_caps placement, mirrored for this unit.  A compound
    literal is not a valid static initializer (see gui_frame.c's s_init_caps comment), so this
    repeats GUI_FORWARD_CAPS_DEFAULT's fields by hand; gui_init_config_front overwrites it before
    init().
==============================================================================================*/

gui_forward_caps_t s_fwd_caps = { .tables = true, .docking = true, .keyboard_nav = true };

/* The theme registry, base/active style state (s_style_base, s_style, s_font_size), the style
   stacks, and layout_compute live in the STYLE UNIT (gui_style.c) since R5; this unit reads
   s_style and the resolvers through the style/gui_style.h externs + seams.

   The shared stateless helpers (saturate, clampf, rect_intersect) are static inline in
   rect/gui_rect.h (R1b) -- every unit reaches them through the public gui.h chain. */

/*==============================================================================================
    Unity build
==============================================================================================*/

/* The render backend (render/resource/gui_atlas, gui_font, gui_icon; render/pipeline/gui_shader,
   gui_emit_draw, gui_emit_path, gui_build_tess, gui_build_volatile, gui_build_cache, gui_render;
   render/gui_debug_overlay) is the SECOND unit -- compiled separately via gui_render.c.  This
   unit calls into it through the draw_* / font_* / gui_render_* declarations in gui_render.h. */

/*----------------------------------  LIBRARY: GUI_CORE  ----------------------------------*/
// THE INTERACT SERVER is its OWN translation unit since R4 (gui_core.c): io, ids, keyed
// state, ambient interaction records, the surface service, the item protocol, anim, and the
// query readers.  This unit reaches it through the core/gui_core.h + core/gui_ctx.h seams.
// THE STYLE UNIT is its own translation unit since R5 (gui_style.c): theme registry, stacks,
// resolution, projections -- reached through the style/gui_style.h seams.
// The present/ paint primitives (the last of the old core group) moved to the element unit
// at R8 (element/gui_adornment.c) and present/ is DISSOLVED.

/* gui_symbol.c moved to the draw unit (gui_draw.c, R3); the gesture services (gui_move.c,
   gui_resize.c, gui_drag.c, gui_feature.c, gui_behavior.c) moved to the interact unit
   (gui_interact.c, R6); window text selection was re-classified as chrome and moved to
   chrome/window/gui_select.c (the chrome unit) -- it reads the render capture + font metrics. */

/*----------------------------------  LIBRARY: GUI_FLOW  ----------------------------------*/

// GUI_FLOW is its OWN translation unit since R7 (gui_flow.c): composition -- spacing
// metrics in, rects out.  It reaches core through the flow/gui_flow.h + core seams; its
// upward calls (scrollbar_widget, the child box paint trio) are the documented block in
// flow/gui_flow.h.  This unit calls INTO it through the same seam declarations.

/*----------------------------------  LIBRARY: GUI_INTERACT  ----------------------------------*/
/* GUI_INTERACT is its OWN translation unit since R6 (gui_interact.c): move/resize/drag
   gestures, the feat_* kit, and the public behavior verbs.  This unit reaches them through
   the interact/gui_interact.h seams (frame_begin drives drag_new_frame; the viewport
   tear-off reads move_grab_offset). */

/*----------------------------------  LIBRARY: GUI_CHROME  ----------------------------------*/
// GUI_CHROME is its OWN translation unit (gui_chrome.c): the six folders under chrome/
// since R9 -- widgets, table, window, dock, popup, nav (nav is core-classified but reads
// the popup stack, so it lives with chrome).  It composes the core services + the flow emit surface through the
// unit headers below it; this unit's upward calls into it (the frame lifecycle's window /
// popup / dock / nav steps) resolve through chrome/gui_chrome.h's frame-step declarations.

/* user/ dissolved at R6: gui_canvas.c -> draw (R3); gui_query.c -> the interact server (R4);
   gui_stacks.c -> the style unit (R5); gui_behavior.c -> the interact unit (R6). */

// element/ -- GUI_ELEMENT is its OWN translation unit since R8 (root gui_element.c): the
// el_* cores plus the absorbed styled painters (per-item wrappers, system adornments, the
// styled symbol half), reached through element/gui_element_internal.h.
// gui_style_apply (frame/, below) calls across to el_style_derive at every theme/font landing.

/*----------------------------------  LIBRARY: GUI_DEBUG  ----------------------------------*/

// GUI_DEBUG is its OWN translation unit (root gui_debug.c since R10): the pipeline dashboard
// + command stepper reach gui only through the public surface, the backend capture API, and
// the unit-header seams.  gui_frame_overlay.c stays in THIS unit (frame group below): it
// carries the frame-timing helpers the lifecycle calls -- conductor code, not severable
// tooling -- and lives in frame/ since R10.

/*----------------------------------  LIBRARY: GUI_FRAME  ----------------------------------*/

// Orchestration -- sits above every tier, drives whichever are compiled in.  The overlay file
// carries the perf/state HUDs plus the frame-timing helpers the lifecycle in gui_frame.c calls,
// so it must precede gui_frame.c in the unity build.
#include "runtime_service/gui/frame/gui_frame_overlay.c"
#include "runtime_service/gui/frame/gui_frame.c"

// The pane bracket -- the go-between verb stamping BOTH servers (R4); and the public
// multi-context lifecycle -- context destruction tears down GPU surfaces, orchestrator work.
#include "runtime_service/gui/frame/gui_pane.c"
#include "runtime_service/gui/frame/gui_context.c"

// Viewport lifecycle + gui-owned floater surfaces -- separated from gui_frame.c because it is a
// distinct concern (OS window / rhi context ownership) from the frame lifecycle proper.  Included
// after gui_frame.c: gui_viewport_render_floaters calls gui_render(), defined there.
#include "runtime_service/gui/frame/gui_viewport.c"

// Boot-tier host front end -- one-call setup (boot) + the canonical loop (frame_poll,
// present_begin/present).  Last: it composes the lifecycle, viewport, and window layers above.
#include "runtime_service/gui/frame/gui_boot.c"

// MEMORY ACCOUNTING: sizeof-sums this unit's fixed statics for gui_mem_stats (cpu_frontend_bytes).
// MUST stay the last constituent include -- unity visibility only flows downward, and the
// full-accounting contract is that every static aggregate above is in scope here.
#include "runtime_service/gui/gui_ui_mem.c"

#ifndef GUI_API_C_PRELUDE
    #include "engine/mod/mod_export.h"
    #include "runtime_service/gui/gui_api.c"
#endif

/*============================================================================================*/
// clang-format on