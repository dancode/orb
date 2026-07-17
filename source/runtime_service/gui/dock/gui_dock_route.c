/*==============================================================================================

    runtime_service/gui/dock/gui_dock_route.c -- The window <-> dock route seam.

    The five verbs window/ is allowed to call into dock/ -- one per protocol point of the
    window lifecycle (see the seam contract in gui_internal.h).  Thin dispatch onto the dock
    internals: dock_find_window_node (gui_dock_core.c), dock_float_service_request /
    dock_float_resolve (gui_dock_float.c), dock_drag_detect / dock_drag_commit /
    dock_window_chrome (gui_dock_drag.c).  window/ names none of those directly; a build
    without docking is a stub implementation of this file.

    Included by gui.c last of the dock/ files, so every internal it dispatches to is already
    in scope; the callers in window/ reach it through the gui_internal.h forward declarations.

==============================================================================================*/
// clang-format off

/* Answer "who places this window?" at window_begin.  Services any pending tab-group creation
   first (the only moment both window titles are in hand), then looks the window up in the node
   pool.  A floating group's active tab also resolves the group's frame for this frame (strip
   drag / edge resize applied, resize grab armed) and carries the hot edge mask back for
   s_scope.resize_hot. */
static gui_win_route_t
window_route_resolve( gui_id_t id, const char* title, gui_window_t* win )
{
    gui_win_route_t route = { 0 };

    dock_float_service_request( id, title, win );

    route.node = dock_find_window_node( id );
    if ( !route.node )
        return route;

    /* Dormant tree (dockspace not emitted this build): the window keeps its tab membership but
       renders nothing -- inactive-tab semantics (begin returns false, end early-outs) -- rather
       than free-floating away or drawing pinned to a rect that no longer lays out.  Floating tab
       groups are not part of the viewport tree and self-manage; they are exempt. */
    if ( !route.node->floating && !dock_vp_emitted( route.node->viewport ) )
        return route;   /* route.active stays false */

    /* A settled dockspace-maximized leaf covers the whole tree: every OTHER node's windows are
       fully obscured, so they suppress exactly like tabs in a dormant tree.  Only once SETTLED --
       while the cover tween is still in flight the siblings peek out around it and keep emitting
       (dock_max_settled, stamped by dockspace_over_viewport's step).  Floating groups stack above
       the tiles at their own z and are exempt, like everywhere else. */
    if ( !route.node->floating )
    {
        const gui_viewport_t* v = &g_ctx->vp.pool[ route.node->viewport ];
        if ( v->dock_max_settled && v->dock_max_id != route.node->id )
            return route;   /* obscured -- route.active stays false */
    }

    /* Re-appearing after absence (menu re-shown, X re-opened): become the leaf's active tab --
       the revival must be visible, not buried behind whichever tab took the pane over while this
       window was hidden (dock_hidden_refresh moved active off it).  First-ever begins
       (last_frame 0) keep the stored selection, so a loaded layout's active tab survives
       startup.  Reads the pre-begin stamp: window_begin_docked refreshes last_frame after. */
    if ( win && win->last_frame != 0u && win->last_frame + 1u < g_ctx->retained.frame )
        for ( u32 i = 0; i < route.node->tab_count; ++i )
            if ( route.node->tabs[ i ] == id ) { route.node->active_tab = i; break; }

    route.active = ( route.node->active_tab < route.node->tab_count
                     && route.node->tabs[ route.node->active_tab ] == id );

    if ( route.node->floating && route.active )
        route.resize_hot = dock_float_resolve( route.node, id );

    return route;
}

/* Preview a drop target (per-node 5-way overlay) while a free window is title-dragged over a
   dockspace on its own surface. */
static void
window_route_drag( gui_id_t id, gui_window_t* win )
{
    dock_drag_detect( id, win );
}

/* Execute the drop computed by window_route_drag on the release edge.  Gating lives inside --
   s_dock_drag is private to the dock files -- so the free path calls it unconditionally; it
   no-ops unless this is the dragged window releasing. */
static void
window_route_commit( gui_id_t id, const char* title )
{
    dock_drag_commit( id, title );
}

/* Tab strip + tabs + node border, drawn by window_end's docked path in place of a title bar.
   Reads the current window rect from s_build. */
static void
window_route_chrome( gui_dock_node_t* node )
{
    dock_window_chrome( node );
}

/* The dock exception to raise-on-press: a docked tile is placed by its node and a click must
   never reorder it, while a FLOATING tab group stacks like a free window and raises as a whole
   (its node carries the z all its tabs draw at).  Returns true when the window is dock-managed
   either way -- the caller then leaves the window's own z untouched. */
static bool
window_route_raise( gui_id_t id )
{
    gui_dock_node_t* node = dock_find_window_node( id );
    if ( !node )
        return false;

    if ( node->floating )
        node->z = surface_z_raise( node->z );

    return true;
}

// clang-format on
/*============================================================================================*/
