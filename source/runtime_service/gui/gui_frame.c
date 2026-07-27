/*==============================================================================================

    runtime_service/gui/gui_frame.c -- GUI_FRAME translation unit: THE FRAME ORCHESTRATOR.

    The top of the stack and nothing else: boots both servers, owns the viewports and
    the app/sys wiring, pumps io into the interact server, hands each surface's GPU pieces to
    the render server at flush.  Everything with a role of its own is one of the carved units
    at this directory's root (the roster is the `unit` list under `target gui` in orb.targets)
    -- the model, the dependency graph, and the role map live in GUI_ARCHITECTURE.md, not here.
    The module face (vtable + descriptor + the app/rhi API pointer storage) is the SEPARATE
    gui.c unit; this unit is pure orchestration.

    The frame unit owns NO unit header: its public face IS gui.h / gui_api.h / gui_host.h, and
    its few internal seams are forward-declared below the includes.  It is the only unit that
    includes every unit header -- the orchestrator sees the whole stack; the stack never sees
    the orchestrator (the units call up only through the documented upward seams in their own
    headers).

    THIS UNIT's constituents, in include order (each carved unit lists its own):

    frame/gui_frame_overlay.c    -- built-in perf / state HUD overlays + the frame-timing helpers they read
                                      (home -- conductor code, never part of the debug unit)
    frame/gui_frame_loop.c       -- frame lifecycle: init/shutdown, frame_begin/end, ctx_begin/end, render, clip
    frame/gui_frame_font.c       -- font API (load/use/push/pop/active_id) + the font -> layout bridge (gui_style_apply)
    frame/gui_pane.c             -- the pane bracket: pane_tag + gui_pane_begin/end stamp BOTH servers
    frame/gui_context.c          -- public multi-context lifecycle + the context block allocation
    frame/gui_viewport.c         -- surface record lifecycle (viewport_create/destroy) + viewport open/resize/
                                      close + gui-owned floater lifecycle (spawn/update/render_floaters)
    frame/gui_boot.c             -- THE BOOT PATH: boot + boot_poll + the boot_present pair (plus
                                      frame_pace, shared with the runtime path)

    gui_ui_mem.c                 -- frontend memory accounting (gui_ui_memory) + the gui_mem_stats
                                      aggregation; must be the last constituent include so it sees them all

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>           // va_list / va_start -- printf-style textf() widget
#include <math.h>             // floorf / ceilf -- pixel-grid snapping in draw + scissor

#include "orb.h"
#include "base/fmt.h"         // fmt_snprintf / fmt_vsnprintf -- CRT-free formatting on the per-frame text paths
#include "base/math.h"        // f32_lerp -- from/to interpolation for the animation service
#include "base/math_ease.h"   // f32_ease_* shapers -- the easing curves the animation service applies

#include "engine/sys/sys_host.h"   // sys_root_dir -- disk assets (load_icon, asset_path) resolve root-relative

/* The orchestrator's world -- everything, in stack order (each carved unit includes only
   the headers at or below its layer; this unit sits on top and includes them all). */
#include "runtime_service/gui/render/gui_render.h"   /* THE RENDER SERVER's surface
                                                        (pulls gui_host.h + rhi/app APIs)   */
#include "runtime_service/gui/core/gui_core.h"       /* THE INTERACT SERVER's services      */
#include "runtime_service/gui/core/gui_ctx.h"        /* ... and its retained-mode storage   */
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/component/gui_component_internal.h"
#include "runtime_service/gui/stock/gui_stock_internal.h"
#include "runtime_service/gui/chrome/gui_chrome.h"
#include "runtime_service/gui/debug/gui_debug.h"

// API function headers -- the rhi()/app() accessors the frame code calls.  The API pointer
// STORAGE (MOD_USE_RHI / MOD_USE_APP) lives in the gui.c module face, next to the module init
// that fetches it; this unit only reads through the extern accessors these headers declare.
#include "runtime_service/rhi/rhi_api.h"
#include "engine/app/app_api.h"

// clang-format off
/*============================================================================================*/
/* Frame-unit internal seams -- both ends live in THIS translation unit, so the declarations
   live here rather than in any unit header (the frame unit owns no header of its own: its
   public face IS gui.h / gui_api.h / gui_host.h).  Forward-declared because the frame group's
   include order has callers (gui_frame_loop.c) before definers (gui_context.c, gui_viewport.c). */

gui_context_t* ctx_alloc_slot ( const gui_ctx_config_t* c, u32 slots, i32 slot );           /* gui_context.c */
void           ctx_pool_init  ( void );                                                     /* gui_context.c */

bool           viewport_create ( gui_vp_t vp, rhi_texture_t target, i32 win_id );             /* gui_viewport.c */
void           viewport_destroy( gui_vp_t vp );                                               /* gui_viewport.c */

/* The theme registry, base/active style state (s_style_base, s_style, s_font_size), the style
   stacks, and metrics_compute live in the STYLE UNIT (gui_style.c); this unit reads
   s_style and the resolvers through the style/gui_style.h externs + seams.

   The shared stateless helpers (saturate, clampf, rect_intersect) are static inline in
   rect/gui_rect.h -- every unit reaches them through the public gui.h chain. */

/*==============================================================================================
    Unity build
==============================================================================================*/

/* THE RENDER SERVER is its OWN translation unit (gui_render.c): the shared atlas, the emit ->
   build -> submit pipeline, the debug overlay, and the captures.  This unit calls into it
   through the draw_* / gui_render_* declarations in render/gui_render.h. */

/*----------------------------------  LIBRARY: GUI_CORE  ----------------------------------*/
// THE INTERACT SERVER is its OWN translation unit (gui_core.c): io, ids, keyed
// state, ambient interaction records, the surface service, the item protocol, anim, and the
// query readers.  This unit reaches it through the core/gui_core.h + core/gui_ctx.h seams.
// THE STYLE UNIT is its own translation unit (gui_style.c): theme registry, stacks,
// resolution, projections -- reached through the style/gui_style.h seams.

/*----------------------------------  LIBRARY: GUI_FLOW  ----------------------------------*/

// GUI_FLOW is its OWN translation unit (gui_flow.c): composition -- spacing
// metrics in, rects out.  It reaches core through the flow/gui_flow.h + core seams; its
// upward calls (scrollbar_widget, the child box paint trio) are the documented block in
// flow/gui_flow.h.  This unit calls INTO it through the same seam declarations.

/*----------------------------------  LIBRARY: GUI_INTERACT  ----------------------------------*/
/* GUI_INTERACT is its OWN translation unit (gui_interact.c): move/resize/drag
   gestures, the feat_* kit, and the public behavior verbs.  This unit reaches them through
   the interact/gui_interact.h seams (frame_begin drives drag_new_frame; the viewport
   tear-off reads move_grab_offset). */

/*----------------------------------  LIBRARY: GUI_CHROME  ----------------------------------*/
// GUI_CHROME is its OWN translation unit (gui_chrome.c): the six folders under chrome/
// -- widgets, table, window, dock, popup, nav (nav is core-classified but reads
// the popup stack, so it lives with chrome).  It composes the core services + the flow emit surface through the
// unit headers below it; this unit's upward calls into it (the frame lifecycle's window /
// popup / dock / nav steps) resolve through chrome/gui_chrome.h's frame-step declarations.

// stock/ -- GUI_STOCK is its OWN translation unit (root gui_stock.c): the stock_* renders
// plus the absorbed styled painters (per-item wrappers, system adornments, the styled
// symbol half), reached through stock/gui_stock_internal.h.
// gui_style_apply (frame/, below) drives a style landing at every theme/font change.

/*----------------------------------  LIBRARY: GUI_DEBUG  ----------------------------------*/

// GUI_DEBUG is its OWN translation unit (root gui_debug.c): the pipeline dashboard
// + command stepper reach gui only through the public surface, the backend capture API, and
// the unit-header seams.  gui_frame_overlay.c stays in THIS unit (frame group below): it
// carries the frame-timing helpers the lifecycle calls -- conductor code, not severable
// tooling -- and lives in frame/.

/*----------------------------------  LIBRARY: GUI_FRAME  ----------------------------------*/

// Orchestration -- sits above every tier, drives whichever are compiled in.  The overlay file
// carries the perf/state HUDs plus the frame-timing helpers the lifecycle in gui_frame_loop.c
// calls, so it must precede gui_frame_loop.c in the unity build.  The font API + the font ->
// layout bridge follow in gui_frame_font.c (the loop's gui_font_flush_deferred reaches
// gui_style_apply through its gui.h prototype, so include order between the two does not matter).
#include "runtime_service/gui/frame/gui_frame_overlay.c"
#include "runtime_service/gui/frame/gui_frame_loop.c"
#include "runtime_service/gui/frame/gui_frame_font.c"

// The pane bracket -- the go-between verb stamping BOTH servers; and the public
// multi-context lifecycle -- context destruction tears down GPU surfaces, orchestrator work.
#include "runtime_service/gui/frame/gui_pane.c"
#include "runtime_service/gui/frame/gui_context.c"

// Viewport lifecycle + gui-owned floater surfaces -- separated from gui_frame_loop.c because it is
// a distinct concern (OS window / rhi context ownership) from the frame lifecycle proper.  Included
// after gui_frame_loop.c: gui_viewport_render_floaters calls gui_render(), defined there.
#include "runtime_service/gui/frame/gui_viewport.c"

// The boot path -- one-call setup (boot), its pump (boot_poll), and its render pair
// (boot_present_*).  Last: it composes the lifecycle, viewport, and window layers above.
#include "runtime_service/gui/frame/gui_boot.c"

// MEMORY ACCOUNTING: sizeof-sums this unit's fixed statics for gui_mem_stats (cpu_frontend_bytes).
// MUST stay the last constituent include -- unity visibility only flows downward, and the
// full-accounting contract is that every static aggregate above is in scope here.
#include "runtime_service/gui/gui_ui_mem.c"

/*============================================================================================*/
// clang-format on
