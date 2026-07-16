/*==============================================================================================

    runtime_service/gui/window/gui_window_docked.c -- The docked branch of window_begin.

    A window whose id is in a dock leaf is placed and chromed by its node, not free-floated:
    geometry is pinned to the node, the title bar is replaced by the node's tab strip (drawn in
    window_end via the route seam's chrome verb), and only the node's active tab opens a body.  Drag,
    edge-resize, tear-off, collapse and boundary-clamp are all skipped -- the node and its
    splitters own placement.

    Split out of gui_window_free.c (the free-float path) because docked placement is a
    genuinely separate concern from it, sharing only the s_build.win / s_scope handoff both
    paths commit for window_end to read.  Mirrors the same-kind separation dock/ already
    keeps between its own core/drag/float/serialize files.

    Included by gui.c right before gui_window_free.c, whose window_begin_ex calls
    window_begin_docked as its dock branch.

==============================================================================================*/
// clang-format off

/* window_begin_docked -- the docked branch of window_begin.  A window whose id is in a dock leaf is
   placed and chromed by its node, not free-floated: geometry is pinned to the node, the title bar is
   replaced by the node's tab strip (drawn in window_end via the route seam's chrome verb), and only
   the node's active tab opens a body.  Drag, edge-resize, tear-off, collapse and boundary-clamp are
   all skipped -- the node and its splitters own placement.  The route (who places it, active tab,
   floating hot edge mask) was resolved by window_route_resolve; this function only consumes it.
   Returns true only for the active tab (its body is emitted); an inactive tab returns false and
   draws nothing, exactly like a collapsed window. */
static bool
window_begin_docked( gui_window_t* win, gui_id_t id, const char* title,
                     gui_win_flags_t flags, const gui_win_route_t* route )
{
    gui_dock_node_t* node   = route->node;
    bool             active = route->active;

    /* Geometry owned by the node; mirror it onto the record so a later undock resumes here. */
    win->viewport   = node->viewport;
    win->x          = node->rect.x;
    win->y          = node->rect.y;
    win->w          = node->rect.w;
    win->h          = node->rect.h;
    win->collapsed  = false;
    win->last_frame = g_ctx->retained.frame;

    f32 title_h = node->rect.h - node->content.h;   /* tab strip height (= WIN_TITLE_H, node-clamped) */

    /* Route to the node's surface: a tree node draws at a low z so docked content sits behind the
       free-floating windows; a floating group stacks among them at its own z. */
    draw_set_window( id );                  /* cache key: docked windows share z=0 but not their id */
    draw_set_sort_key( node->floating ? node->z : 0 );
    draw_set_viewport( node->viewport );
    draw_set_band( ( flags & GUI_WIN_DEBUG_BAND ) ? 1u : 0u );
    s_build.win.viewport = node->viewport;

    /* Commit the docked window context window_end reads. */
    s_build.win.id          = id;
    s_scope.win             = id;      /* interaction scope: this window owns the items that follow */
    s_build.win.title       = title;
    s_build.win.collapsed   = false;
    s_build.win.flags       = flags;
    s_build.win.title_h     = title_h;
    s_scope.resize_hot      = route->resize_hot;   /* 0 for a tree node; a floating group resizes like a window */
    s_build.win.rec         = win;
    s_build.win.dock_node   = node;
    s_build.win.dock_active = active;
    s_build.win.x = win->x;  s_build.win.y = win->y;
    s_build.win.w = win->w;  s_build.win.h = win->h;

    if ( !active )
        return false;   /* behind another tab -- no body, no clip; window_end early-outs */

    /* The active tab nominates hover over the whole node (strip + body) so its tab-strip widgets and
       body widgets all resolve under one hover_win.  A floating group competes at its own z and
       expands the nominee rect by the outer resize band, exactly like a resizeable free window --
       that is what keeps an edge hot as the cursor crosses just outside the border. */
    if ( !( flags & GUI_WIN_NO_INPUT ) )
    {
        if ( node->floating )
        {
            f32 o = RESIZE_BAND_OUTER;
            surface_hover_nominate( id, ( gui_rect_t ){ node->rect.x - o, node->rect.y - o,
                                                         node->rect.w + 2.0f * o, node->rect.h + 2.0f * o },
                                   node->z, node->viewport );
        }
        else
        {
            surface_hover_nominate( id, node->rect, 0u, node->viewport );
        }
    }

    /* Clip against the node's surface, then the node rect; the body region reuses this clip. */
    {
        const gui_viewport_t* vp = &g_ctx->vp.pool[ node->viewport ];
        draw_set_root_clip( vp_w( vp ), vp_h( vp ) );
    }
    item_flags_chrome_reset();

    draw_push_clip_rect( win->x, win->y, win->w, win->h );
    s_scope.clip = ( gui_rect_t ){ win->x, win->y, win->w, win->h };

    /* Body background fills the whole node; the tab strip is overpainted by the node chrome (window_route_chrome) last.
       Docked nodes tile against each other at right angles, so the node draws square -- a rounded
       corner here would cut a gap into the seam between neighbours. */
    draw_set_rounding( 0.0f );
    draw_push_rect_filled( win->x, win->y, win->w, win->h, 0.0f, 0.0f, 1.0f, 1.0f, 0, COL_WIN_BG );

    /* FUTURE: docked windows reserve no menu-bar row -- GUI_WIN_MENUBAR is ignored on the docked
       path (the free-float path in window_begin_ex honors it via mb_h). */
    s_build.win.menubar_rect = ( gui_rect_t ){ win->x, win->y + title_h, win->w, 0.0f };

    /* Open the body over the node's content rect -- the same region machinery a free window uses. */
    layout_push_region( id, node->content, REGION_PAD_DEFAULT, flags, &win->scroll, /* own_clip */ false );
    return true;
}

// clang-format on
/*============================================================================================*/
