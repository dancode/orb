/*==============================================================================================

    runtime_service/gui/chrome/dock/gui_dock_float.c -- Floating tab groups: windows sharing one frame.

    Tabbing WITHOUT split panes: several windows merged onto a single free-floating frame with a
    tab strip -- the other half of what "docking" bundles, split out on its own.  A group is a
    dock LEAF that lives outside any viewport tree (node->floating): the shared tab machinery
    (strip chrome, live reorder, undock-by-tab-drag, window_begin_docked) all works on it
    unchanged, while THIS file supplies what the tree normally provides -- per-frame geometry.
    The node's rect is its persisted frame: the strip's empty band drags it (like a title bar),
    the edge band resizes it (the shared record-agnostic resize helpers), and node->z stacks it
    among the free windows (a tree node draws at z 0 behind them).

    Groups form by gesture or by verb:

      Gesture -- a free window title-dragged over another free window's TITLE BAR (or an existing
      group's strip) shows a center tab chip; the release enqueues s_dock_float_req, and the
      TARGET window's next window_begin services it (dock_float_service_request) -- the only
      moment the target's title string is in hand for its tab name.  Detection + commit ride the
      drag-to-dock path in gui_dock_drag.c (dock_float_hit below is its probe).

      Verb -- gui_window_tab( title, onto_title ) groups two windows programmatically; both
      titles are in hand, so the group forms immediately.

    A group dissolves when one tab remains: the survivor inherits the frame and returns to
    free-floating (dock_node_remove_window, gui_dock_core.c).

    Included by gui_chrome.c after gui_dock_core.c (node pool + leaf edits in scope) and before
    gui_dock_drag.c (which calls dock_float_hit / dock_float_request directly).
    window/ reaches dock_float_resolve / dock_float_service_request only through the route seam
    (chrome/dock/gui_dock_route.c, included after this file).

==============================================================================================*/
// clang-format off

/* Strip-drag salt: the group's move-grab gets a stable widget id distinct from its tabs
   (DOCK_TAB_SALT), its resize (GUI_RESIZE_SALT), and any window id. */
#define DOCK_FLOAT_SALT 0xF10A7B02u

/*==============================================================================================
    Group-creation request -- one slot, filled on the drop release, serviced one frame later.

    Creating a group needs BOTH windows' display names, but at commit time only the DRAGGED
    window's title is in hand (window_end passes it); the target is known by id alone.  So the
    dragged name is copied here and the group forms when the TARGET next begins -- its title is
    then the parameter.  frame stamps the request so a target that never begins again cannot
    resurrect a stale group much later.
==============================================================================================*/

static struct
{
    bool       active;
    u32        frame;                              /* request frame, for expiry               */
    gui_id_t target;                             /* window to grow a group around           */
    gui_id_t dragged;                            /* window dropped onto it                  */
    char       dragged_name[ GUI_DOCK_NAME_CAP ];  /* its tab name, copied at commit          */

} s_dock_float_req;

/* A fresh one-tab group wrapped around `target`, inheriting its exact frame and a fresh top z --
   the group appears exactly where the window was, now wearing a strip instead of a title bar. */
static gui_dock_node_t*
dock_float_group_create( gui_window_t* target, gui_id_t target_id, const char* target_title )
{
    gui_dock_node_t* n = dock_node_alloc( target->viewport );
    if ( !n )
        return NULL;
    n->floating = true;
    n->z        = surface_z_raise( n->z );
    n->rect     = ( gui_rect_t ){ target->x, target->y, target->w, target->h };
    dock_leaf_carve_content( n );

    dock_leaf_tab_add( n, target_id, target_title );
    return n;
}

/*==============================================================================================
    Drop-target probe -- which free window's title bar (or group's strip) is under the cursor.

    Called by dock_drag_detect while a free window is title-dragged: a hit means "tab onto this"
    and pre-empts the dockspace chips underneath (the strip is visually above them).  Front-most
    candidate by z wins.  A window tabbed in a floating group is probed as its GROUP (one probe,
    via the active tab); a tree-docked window is skipped -- the dockspace center chip owns that
    drop.  Targets must be real interactive windows: alive this frame, titled, not an overlay in
    the popup band, not a native frame shell, not click-through, and not pinned (NOMOVE -- a
    group drags as one, which would quietly un-pin it).
==============================================================================================*/

static bool
dock_float_hit( gui_id_t drag_id, i32 vp, gui_dock_node_t** out_node, gui_id_t* out_win )
{
    *out_node = NULL;
    *out_win  = GUI_ID_NONE;
    if ( !g_ctx->dock.pool )
        return false;   /* pool disabled -- no groups can form */

    u32  best_z = 0;
    bool found  = false;
    for ( u32 i = 0; i < g_ctx->win.count; ++i )
    {
        gui_window_t* w = &g_ctx->win.pool[ i ];
        if ( w->id == GUI_ID_NONE || w->id == drag_id ) continue;
        if ( w->viewport != vp || w->closed )             continue;
        if ( w->last_frame + 1 < gui_frame_index() )       continue;   /* not begun this frame */
        if ( w->overlay )                   continue;   /* popup / tooltip overlay */
        if ( w->flags & ( GUI_WIN_NATIVE | GUI_WIN_NO_INPUT | GUI_WIN_NOMOVE
                          | GUI_WIN_NO_TAB_TARGET ) )     continue;   /* opted out of hosting tabs */

        gui_dock_node_t* n = dock_find_window_node( w->id );
        if ( n && !n->floating )
            continue;   /* tree-docked: the dockspace chips own that drop */

        gui_rect_t band;
        u32          z;
        if ( n )
        {
            if ( n->tab_count == 0 || n->tabs[ n->active_tab ] != w->id )
                continue;   /* probe each group once, through its active tab */
            if ( n->tab_count >= GUI_DOCK_TABS_MAX )
                continue;   /* group full -- never offer a join the drop would refuse */
            band = ( gui_rect_t ){ n->rect.x, n->rect.y, n->rect.w, WIN_TITLE_H };
            z    = n->z;
        }
        else
        {
            if ( w->flags & GUI_WIN_NOTITLEBAR )
                continue;   /* no title bar to land the drop on */
            band = ( gui_rect_t ){ w->x, w->y, w->w, WIN_TITLE_H };
            z    = w->z;
        }

        if ( !gui_rect_contains( band, s_io.mouse_x, s_io.mouse_y ) ) continue;
        if ( found && z <= best_z )                                     continue;
        found     = true;
        best_z    = z;
        *out_node = n;
        *out_win  = w->id;
    }
    return found;
}

/* Enqueue a group creation for dock_float_service_request -- the release edge of a drop on a
   free window's title bar (dock_drag_commit).  Joining an EXISTING group needs no request: the
   dragged title is in hand there, so gui_dock_window tabs it in directly. */
static void
dock_float_request( gui_id_t target_win, gui_id_t dragged, const char* dragged_title )
{
    s_dock_float_req.active  = true;
    s_dock_float_req.frame   = gui_frame_index();
    s_dock_float_req.target  = target_win;
    s_dock_float_req.dragged = dragged;

    u32 vis = label_vis_len( dragged_title );
    if ( vis >= GUI_DOCK_NAME_CAP ) vis = GUI_DOCK_NAME_CAP - 1;
    memcpy( s_dock_float_req.dragged_name, dragged_title, vis );
    s_dock_float_req.dragged_name[ vis ] = '\0';
}

/* Service the pending request when the TARGET window begins -- its title (the missing tab name)
   is the parameter.  Called by window_begin_ex just before the dock lookup, so the group formed
   here routes the target straight into window_begin_docked the same frame. */
static void
dock_float_service_request( gui_id_t id, const char* title, gui_window_t* win )
{
    if ( !s_dock_float_req.active || s_dock_float_req.target != id )
        return;
    s_dock_float_req.active = false;
    if ( !title || gui_frame_index() > s_dock_float_req.frame + 2 )
        return;   /* untitled target, or the target skipped frames -- drop the stale request */

    /* The target may have been tabbed somewhere itself since the commit -- then just join it. */
    gui_dock_node_t* n = dock_find_window_node( id );
    if ( !n )
    {
        if ( win->flags & GUI_WIN_NO_TAB_TARGET )
            return;   /* opted out (re-checked here: flags may have changed since the drop) */
        n = dock_float_group_create( win, id, title );
    }
    if ( n )
        dock_leaf_tab_add( n, s_dock_float_req.dragged, s_dock_float_req.dragged_name );
}

/*==============================================================================================
    Per-frame frame resolve -- the group's stand-in for dock_node_layout.

    Called by window_begin_docked for the group's ACTIVE tab, before any geometry is read: apply
    the in-flight strip drag / edge resize onto node->rect, keep the strip reachable, refresh the
    content rect, and resolve this frame's resize hover-and-grab (returned for
    s_scope.resize_hot, so widgets defer to a hot edge exactly as in a free window).
==============================================================================================*/

/* Keep the group reachable, mirroring window_clamp: the strip may not slide under the host's
   native caption band or the main menu bar (vp_work_top), or fully off the surface. */
static void
dock_float_clamp( gui_dock_node_t* node )
{
    i32 vp = node->viewport;
    f32 dw = vp_w( vp );
    f32 dh = vp_h( vp );
    const f32 margin = WIN_TITLE_H;
    const f32 top    = vp_work_top( vp );

    if ( node->rect.x > dw - margin )            node->rect.x = dw - margin;
    if ( node->rect.y > dh - margin )            node->rect.y = dh - margin;
    if ( node->rect.x < margin - node->rect.w )  node->rect.x = margin - node->rect.w;
    if ( node->rect.y < top )                    node->rect.y = top;
}

static u8
dock_float_resolve( gui_dock_node_t* node, gui_id_t active_win_id )
{
    gui_id_t gid = id_combine( node->id, DOCK_FLOAT_SALT );    /* strip move grab  */
    gui_id_t rid = id_combine( node->id, GUI_RESIZE_SALT );    /* edge resize grab */

    /* In-flight strip drag: the grabbed strip point stays pinned under the cursor. */
    move_track( gid, s_io.mouse_x, s_io.mouse_y, &node->rect.x, &node->rect.y );

    /* In-flight edge resize: shared raw edge math + the window min-size policy (a moving edge
       stops against the pinned far edge). */
    if ( s_interaction.active_id == rid )
    {
        gui_rect_t r = node->rect;
        resize_apply_edges( &r, resize_edges() );
        const f32 min_w = window_min_w();
        const f32 min_h = window_min_h( WIN_TITLE_H );
        if ( r.w < min_w )
        {
            if ( resize_edges() & GUI_RESIZE_L ) r.x = resize_fix_x() - min_w;
            r.w = min_w;
        }
        if ( r.h < min_h )
        {
            if ( resize_edges() & GUI_RESIZE_T ) r.y = resize_fix_y() - min_h;
            r.h = min_h;
        }
        node->rect = r;
    }

    dock_float_clamp( node );
    dock_leaf_carve_content( node );

    /* Resize hover-and-grab through the resize_item protocol, with hover gated on the active tab
       (the group's hover nominee) -- the same shape as window_resolve_resize_hot, minus the
       autosize grip a group never has.  The service also drives the directional cursor. */
    bool dragging = false;
    u8   hot      = resize_item( node->id, active_win_id, node->rect,
                                 GUI_RESIZE_L | GUI_RESIZE_R | GUI_RESIZE_T | GUI_RESIZE_B,
                                 false, &dragging );
    return hot;
}

/*==============================================================================================
    Public API
==============================================================================================*/

/* Tab window `title` onto window `onto_title`'s frame -- the programmatic form of the drop
   gesture.  A free target grows a fresh floating group around itself (it must have been begun at
   least once, so a frame exists to inherit); a target already tabbed somewhere -- a floating
   group OR a dockspace leaf -- simply receives the window as a new tab.  Undo with
   gui_dock_undock( title ); a group dissolves on its own when one tab remains. */
void
gui_window_tab( const char* title, const char* onto_title )
{
    if ( !title || !onto_title )
        return;
    if ( !g_ctx->dock.pool )
        return;   /* tab groups ride the dock-node pool */

    gui_id_t wid = id_hash( title );
    gui_id_t tid = id_hash( onto_title );
    if ( wid == tid )
        return;

    gui_dock_node_t* n = dock_find_window_node( tid );
    if ( !n )
    {
        gui_window_t* tw = window_find( tid );
        if ( !tw )
            return;   /* target never begun -- no frame to host a group */
        if ( tw->flags & GUI_WIN_NO_TAB_TARGET )
            return;   /* target opted out of hosting tabs */
        n = dock_float_group_create( tw, tid, onto_title );
    }
    if ( n )
        dock_leaf_tab_add( n, wid, title );
}

// clang-format on
/*============================================================================================*/
