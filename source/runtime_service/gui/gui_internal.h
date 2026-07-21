#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/gui_internal.h -- the internal umbrella, one header per unit.

    Every unit .c includes this once, up front, before its unity constituents.  Under the
    hood it is SEPARATE per-unit headers pulled in stack order (GUI_SERVER_PLAN.md) -- each
    header may assume the ones below it and never the ones above, so the include list here
    IS the dependency graph, lowest to highest:

        rect      (via gui_host.h)   leaf geometry + color primitives + stateless inlines
        core      gui_core.h         INTERACT SERVER: io, ids, keyed state, item protocol, panes
        style     gui_style.h        state flags in, colors / metrics out
        draw      gui_draw.h         drawing routines over the render server's primitives
        interact  gui_interact.h     gesture mechanisms (resize / move / drag / select / feat)
        flow      gui_flow.h         layout composition: the rect producer
        chrome    gui_chrome.h       managed windowing: window / nav / popup / dock records
        frame     gui_frame.h        render surfaces (viewports) the orchestrator manages
        ctx       gui_ctx.h          the context aggregate -- closes the stack (embeds most units)
        debug     gui_debug.h        server introspection (severable)

    Each unit's header holds ONLY what crosses a unit boundary: shared record types, service
    seam declarations, the externs for the ambient records its .c files define.  File-private
    types and statics stay in their owning .c.  Misplaced entries are marked with the
    increment that moves them (e.g. the label grammar's id half -> core in R4).

    Include chain: gui_internal.h -> gui_host.h -> gui_api.h -> gui.h -> rect/gui_rect.h.
    Also pulls rhi_api.h (gui_viewport_t holds GPU buffers/targets) and app_api.h (gui_io_t
    indexes app keys; the OS-event forwarders take an app_event_t).

==============================================================================================*/

#include "runtime_service/gui/gui_host.h"   // public gui types: gui_rect_t, gui_id_t, flags, enums
#include "runtime_service/rhi/rhi_api.h"    // rhi_buffer_t / rhi_texture_t for gui_viewport_t
#include "engine/app/app_api.h"             // app_key_t / app_event_t for gui_io_t + event forwarders

#include "runtime_service/gui/core/gui_core.h"
#include "runtime_service/gui/style/gui_style.h"
#include "runtime_service/gui/draw/gui_draw.h"
#include "runtime_service/gui/interact/gui_interact.h"
#include "runtime_service/gui/flow/gui_flow.h"
#include "runtime_service/gui/chrome/gui_chrome.h"
#include "runtime_service/gui/frame/gui_frame.h"
#include "runtime_service/gui/core/gui_ctx.h"
#include "runtime_service/gui/debug/gui_debug.h"

/*============================================================================================*/
#endif    // GUI_INTERNAL_H
