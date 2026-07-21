#ifndef GUI_FRAME_H
#define GUI_FRAME_H
/*==============================================================================================

    runtime_service/gui/frame/gui_frame.h -- the frame orchestrator's internals.

    The render surfaces the orchestrator manages: one gui viewport rides one OS window + one
    rhi context.  The orchestrator boots both servers, pumps io into the interact server, and
    hands contexts to the render server -- this header carries the surface record both sides
    are wired through.  Included by gui_internal.h after chrome/gui_chrome.h (a viewport
    embeds the dock-tree root).

==============================================================================================*/

// clang-format off

/* Render-surface ceiling: one gui viewport rides one OS window + one rhi context, so the pool
   default, the per-context cap, and the GPU buffer regions (allocated once at init, before any
   config) are all sized by the platform pair -- derived, not repeated.  The per-context runtime
   limit is still g_ctx->vp.max. */

#define GUI_MAX_VIEWPORTS APP_WIN_MAX       // one viewport per OS window / rhi context

ORB_STATIC_ASSERT( APP_WIN_MAX == RHI_CTX_MAX,
                   "a gui viewport pairs an OS window with an rhi context; the maxes must agree" );

/*==============================================================================================
    Render viewport (behavior in render/pipeline/gui_submit.c + frame/gui_viewport.c)

    One render surface a context drives: GPU buffers + a color target, the OS window hosting it, and
    the routing/ownership bookkeeping for host-provided vs gui-owned (torn-off floater) surfaces.
    [0] is the main swapchain; the rest are floaters.  Held by value in gui_context_t vp.pool.
==============================================================================================*/

typedef struct
{
    rhi_buffer_t  vb;           // CPU_TO_GPU vertex buffer, one region per frame-in-flight
    rhi_buffer_t  ib;           // CPU_TO_GPU index buffer (u16), one region per frame-in-flight

    /* Color target flush paints into: RHI_SWAPCHAIN_COLOR for the main viewport, a floater's own
       swapchain image otherwise.  Held per viewport so flush is target-agnostic. */
    rhi_texture_t target;

    /* OS window this surface is hosted by (app win_id_t), or -1 (APP_WIN_INVALID) if unassociated.
       Input routing maps a mouse event's win_id to this surface so the cursor's host viewport is
       known -- a window only hover-tests when the cursor is in the OS window hosting its viewport. */
    i32 win_id;

    /* rhi context driving this surface's swapchain (RHI_CTX_INVALID if none).  Only set for an
       gui-OWNED surface (a torn-off floater gui spawned): flush of a host-provided surface
       resolves RHI_SWAPCHAIN_COLOR from the host's cmd, so the host viewport leaves this invalid.
       An owned surface has no host driving it -- gui runs frame_begin/end on this ctx itself. */
    i32 rhi_ctx;

    /* true when gui created this surface's OS window + rhi context (tear-off floater) and must
       therefore destroy them.  false for the host-provided main surface (index 0) and any surface
       the host opened via viewport_open -- gui frees only the GPU buffers for those, never the
       window/context it does not own. */
    bool owned;

    /* Set when the user closes an owned floater's OS window (APP_EV_WIN_CLOSE): the surface is torn
       down at the next viewport_update, a safe point between the build and the present, so
       no in-flight draw list references a surface being freed.  Ignored for non-owned surfaces. */
    bool pending_close;

    /* Drawable size of this surface in pixels.  Set by the host (viewport 0 from frame_begin, floaters
       via viewport_resize) BEFORE the build so window_begin clips its windows against THIS surface's
       extent, not the main window's.  0 = unset -> window_begin falls back to the main display size
       (single-window behavior).  Distinct from the win_w/win_h passed to flush, which only sets the
       GPU viewport/scissor clamp at submit time; the clip baked into each draw command is built here. */
    i32 disp_w, disp_h;

    /* Top band (pixels) drawn by this surface's native host caption (the GUI_WIN_NATIVE shell
       window's title bar height), published each frame by that shell.  window_clamp keeps non-native
       windows' top edge at or below this inset so their title bars stay grabbable above the drawn
       chrome band.  0 until first published (no native shell or default OS-chrome main window).
       Sticky: NOT cleared each frame -- persists from the last frame the native shell was active so
       viewport_update always has a valid top bound regardless of build ordering. */
    f32 caption_inset;

    /* Main-menu-bar band on this surface: its height plus the frame it last emitted.  Emit-gated
       like a dockspace (bar_seen_frame against the current frame, one-frame tolerance) so a host
       code path that stops emitting the bar releases the band; window_work_top
       (gui_window_free.c) adds it to caption_inset to bound a maximized window's work area. */
    f32 bar_inset;
    u32 bar_seen_frame;

    /* Additional top band (pixels) the host reserves above the dock area -- a main menu bar, a
       toolbar strip -- published via gui()->dockspace_inset() before dockspace_over_viewport.
       Adds to caption_inset when the dock tree lays out.  Sticky like caption_inset: persists
       until the host publishes a new value (0 to reclaim).  Free-floating windows are
       unaffected -- only the dock tree's layout area shrinks. */
    f32 dock_inset;

    /* Per-surface dock tree root.  GUI_DOCK_REF_NONE = free-float placement (overlapping windows,
       including the main viewport); otherwise a ref into the context dock-node pool that tiles/tabs
       the windows on this surface.  Driven by dock/ (dock.c builds it, dock_drag.c re-tiles on drag,
       dock_serialize.c saves/loads it). */
    gui_dock_ref_t dock_root;

    /* Dockspace policy bits (gui_dockspace_flags_t), re-published by every dockspace_over_viewport
       call.  NO_SPLIT restricts the tree to tab docking: no split drop chips, split verbs refuse. */
    gui_dockspace_flags_t dock_flags;

    /* Frame stamp (g_ctx->retained.frame) of this viewport's last dockspace_over_viewport call.
       A dockspace is emit-gated like every immediate-mode element: the tree is ACTIVE only on
       frames the host emits it; on other frames it is DORMANT -- retained but inert.  Windows
       tabbed in a dormant tree suppress (inactive-tab semantics) instead of rendering into rects
       that no longer lay out, and drag-to-dock offers no chips (dock_vp_emitted, gui_dock_core.c).
       A host code path that stops running its dockspace thus parks the layout instead of
       corrupting it; only dock_clear destroys it. */
    u32 dock_seen_frame;

    /* Dockspace maximize: one LEAF pinned over the whole dock area (dock_max_id; 0 = none), fully
       obscuring the other tree nodes.  dock_max_on is the logical state -- false while the restore
       tween eases the node back to its tree rect, after which the id clears.  dock_max_settled is
       stamped by dockspace_over_viewport's tween step each emitted frame: only once the cover has
       SETTLED do the obscured nodes' windows suppress (inactive-tab semantics via the route seam)
       and the splitter / placeholder chrome stop emitting -- during the tween the siblings are
       still partially visible and keep drawing.  dock_max_from is the rect tween's FROM (captured
       at toggle); the target re-aims every frame, so a live surface resize is tracked mid-flight.
       Driven by dock_max_set / dock_max_node (gui_dock_core.c). */
    gui_dock_id_t dock_max_id;
    bool          dock_max_on;
    bool          dock_max_settled;
    gui_rect_t    dock_max_from;

} gui_viewport_t;

/* viewport drawable size with the s_io fallback (core/gui_ctx.c). */
f32 vp_w( const gui_viewport_t* vp );
f32 vp_h( const gui_viewport_t* vp );

/* The mouse-input path (core/gui_io.c) resolves an event's app win_id to the viewport hosting it,
   but the viewport pool lives on g_ctx (gui_ctx.c) included later.  Defined after g_ctx. */
static u32 viewport_index_for_window( i32 win_id );

/* OS resize / close events for an gui-OWNED floater are serviced against the viewport pool, so
   gui_event (core/gui_io.c) delegates them here.  Defined in gui_frame.c after g_ctx; returns
   true when win_id is an owned viewport (event consumed). */
static bool gui_owned_window_event( const app_event_t* ev );

/* forwarded capability flags (gui.c root) -- table / dock / nav feature gates. */
extern gui_forward_caps_t s_fwd_caps;

// clang-format on
/*============================================================================================*/
#endif    // GUI_FRAME_H
