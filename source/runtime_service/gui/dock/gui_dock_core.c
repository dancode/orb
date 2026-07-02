/*==============================================================================================

    runtime_service/gui/dock/gui_dock_core.c -- Dock-node tree: pool, layout, splitter chrome.

    The node pool (alloc/free/find), the per-frame layout pass that assigns every node a rect from
    the surface extent down, the leaf tab-list edit + tree-collapse helpers every mutation path
    (docking, undocking, drag-to-dock, load) shares, and the splitter interaction + empty-leaf
    placeholder chrome drawn once per dockspace.  Everything else that touches a dock tree -- the
    public build API (gui_dock.c), the mouse drag-to-dock / undock-by-tab-drag gestures + tab-strip
    chrome (gui_dock_drag.c), layout save/load (gui_dock_serialize.c) -- is built on these
    primitives and lives in the sibling dock/ files, included after this one.

    Included by gui.c first among the dock/ files: dock_node_alloc / _free / _find /
    dock_leaf_remove_tab / dock_collapse are plain statics, not forward-declared in gui_internal.h,
    so every other dock file needs this one already in scope.  (dock_find_window_node IS
    forward-declared there -- gui_widget_window.c, included earlier, calls it -- but still lives
    here alongside the rest of the node-pool lookups.)

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Constants

    The tab strip stands in for a window's title bar, so it shares the title-bar height (WIN_TITLE_H)
    -- a docked window thus reads at the same vertical rhythm as a free one.  The splitter is a thin
    grabbable gutter reserved BETWEEN sibling rects (not over them), so a press on it never collides
    with a docked window's content -- the cursor sits where no window nominates hover.
----------------------------------------------------------------------------------------------*/

#define DOCK_SPLITTER   6.0f                  /* splitter gutter thickness, in pixels            */
#define DOCK_MIN_PANE   ( WIN_TITLE_H * 2.0f) /* a pane never shrinks below this on the split axis */

/* Salt: a splitter gets a stable per-node widget id distinct from the windows + chrome.  The tab
   salt (DOCK_TAB_SALT) lives in gui_dock_drag.c, next to the tab-strip chrome that uses it. */
#define DOCK_SPLIT_SALT 0xD0C5B17u

/*----------------------------------------------------------------------------------------------
    Node pool

    Fixed per-context array so child / parent pointers stay valid across frames (no compaction).  A
    free slot has id == 0; alloc reuses the first freed hole or appends, and never returns id 0.
----------------------------------------------------------------------------------------------*/

static gui_dock_node_t*
dock_node_alloc( u32 viewport )
{
    if ( !s_dock_nodes ) return NULL;   /* docking disabled for this context */
    gui_dock_node_t* n = NULL;
    for ( u32 i = 0; i < s_dock_node_count; ++i )      /* reuse a freed hole first */
        if ( s_dock_nodes[ i ].id == 0 ) { n = &s_dock_nodes[ i ]; break; }
    if ( !n )
    {
        if ( s_dock_node_count >= g_ctx->max_dock_nodes )
            return NULL;
        n = &s_dock_nodes[ s_dock_node_count++ ];
    }
    memset( n, 0, sizeof *n );
    n->id       = ++s_dock_id_seq;   /* monotonic; never 0 */
    n->viewport = viewport;
    n->ratio    = 0.5f;
    return n;
}

/* Free a node -- zeroing its id returns the slot to the pool (the high-water count is left as is). */
static void
dock_node_free( gui_dock_node_t* n )
{
    if ( n ) memset( n, 0, sizeof *n );
}

static gui_dock_node_t*
dock_node_find( gui_dock_id_t id )
{
    if ( !id ) return NULL;
    for ( u32 i = 0; i < s_dock_node_count; ++i )
        if ( s_dock_nodes[ i ].id == id )
            return &s_dock_nodes[ i ];
    return NULL;
}

/* The lookup window_begin routes through: which LEAF tabs this window, or NULL.  Forward-declared in
   gui_internal.h so gui_widget_window.c (included earlier) can call it. */
static gui_dock_node_t*
dock_find_window_node( gui_id_t win )
{
    if ( !win ) return NULL;
    for ( u32 i = 0; i < s_dock_node_count; ++i )
    {
        gui_dock_node_t* n = &s_dock_nodes[ i ];
        if ( n->id == 0 || n->split != DOCK_SPLIT_NONE )
            continue;
        for ( u32 t = 0; t < n->tab_count; ++t )
            if ( n->tabs[ t ] == win )
                return n;
    }
    return NULL;
}

/*----------------------------------------------------------------------------------------------
    Leaf tab edits + tree collapse
----------------------------------------------------------------------------------------------*/

/* Remove tab `idx` from a leaf, sliding the rest down and keeping active_tab in range. */
static void
dock_leaf_remove_tab( gui_dock_node_t* n, u32 idx )
{
    for ( u32 i = idx; i + 1 < n->tab_count; ++i )
    {
        n->tabs[ i ] = n->tabs[ i + 1 ];
        memcpy( n->names[ i ], n->names[ i + 1 ], GUI_DOCK_NAME_CAP );
    }
    n->tab_count--;
    if ( n->active_tab >= n->tab_count )
        n->active_tab = n->tab_count ? n->tab_count - 1 : 0;
}

/* A leaf that lost its last tab is removed and its parent split replaced by the surviving sibling --
   the sibling takes the parent's slot in the grandparent (or becomes the new dock_root).  A root leaf
   that empties is left in place: a bare dockspace is valid (nothing tiled yet). */
static void
dock_collapse( gui_dock_node_t* leaf )
{
    gui_dock_node_t* parent = leaf->parent;
    if ( !parent )
        return;   /* root leaf emptied -- keep the bare dockspace */

    gui_dock_node_t* sib = ( parent->child[ 0 ] == leaf ) ? parent->child[ 1 ] : parent->child[ 0 ];
    gui_dock_node_t* gp  = parent->parent;

    sib->parent = gp;
    if ( !gp )
        g_ctx->viewports[ parent->viewport ].dock_root = sib;
    else
        gp->child[ gp->child[ 0 ] == parent ? 0 : 1 ] = sib;

    dock_node_free( leaf );
    dock_node_free( parent );
}

/*----------------------------------------------------------------------------------------------
    Layout -- assign every node a rect, top-down from the surface area.

    A leaf reserves a WIN_TITLE_H tab strip off its top; the remainder is `content` (where the active
    window's body draws).  A split divides its rect by `ratio` (child[0]'s fraction of the axis),
    reserving the DOCK_SPLITTER gutter between the two children.  Resolved fresh every frame so an OS
    resize or a splitter drag re-tiles immediately.
----------------------------------------------------------------------------------------------*/

static void
dock_node_layout( gui_dock_node_t* n, gui_rect_t r )
{
    if ( !n )   /* defensive: a corrupt/truncated loaded blob could leave a split child NULL */
        return;
    n->rect = r;

    if ( n->split == DOCK_SPLIT_NONE )
    {
        f32 th = WIN_TITLE_H;
        if ( th > r.h ) th = r.h;
        n->content = ( gui_rect_t ){ r.x, r.y + th, r.w, r.h - th };
        return;
    }

    f32 thick = DOCK_SPLITTER;
    if ( n->split == DOCK_SPLIT_X )
    {
        f32 avail = r.w - thick; if ( avail < 0.0f ) avail = 0.0f;
        f32 w0    = floorf( avail * n->ratio );
        dock_node_layout( n->child[ 0 ], ( gui_rect_t ){ r.x,             r.y, w0,               r.h } );
        dock_node_layout( n->child[ 1 ], ( gui_rect_t ){ r.x + w0 + thick, r.y, r.w - w0 - thick, r.h } );
    }
    else /* DOCK_SPLIT_Y */
    {
        f32 avail = r.h - thick; if ( avail < 0.0f ) avail = 0.0f;
        f32 h0    = floorf( avail * n->ratio );
        dock_node_layout( n->child[ 0 ], ( gui_rect_t ){ r.x, r.y,             r.w, h0               } );
        dock_node_layout( n->child[ 1 ], ( gui_rect_t ){ r.x, r.y + h0 + thick, r.w, r.h - h0 - thick } );
    }
}

/*----------------------------------------------------------------------------------------------
    Splitter interaction + draw (one internal node)

    The gutter sits between the children and over no window, so hover_win is NONE there unless a
    floating window covers it -- gating the grab on hover_win == NONE thus naturally yields to a
    floater drawn on top.  Grab sets active_id (released globally when the button lifts, like a window
    drag); while held, the ratio tracks the cursor, clamped so neither pane shrinks below DOCK_MIN_PANE.
----------------------------------------------------------------------------------------------*/

static void
dock_splitter( gui_dock_node_t* n, u32 vp )
{
    gui_rect_t r     = n->rect;
    f32          thick = DOCK_SPLITTER;
    gui_rect_t sr;
    if ( n->split == DOCK_SPLIT_X )
        sr = ( gui_rect_t ){ n->child[ 1 ]->rect.x - thick, r.y, thick, r.h };
    else
        sr = ( gui_rect_t ){ r.x, n->child[ 1 ]->rect.y - thick, r.w, thick };

    gui_id_t sid    = id_combine( n->id, DOCK_SPLIT_SALT );
    bool       active = ( s_interaction.active_id == sid );
    bool       hot    = false;

    if ( s_io.mouse_viewport == vp && s_interaction.active_id == GUI_ID_NONE
         && s_interaction.hover_win == GUI_ID_NONE && rect_hit( sr ) )
    {
        hot = true;
        if ( s_io.mouse_pressed[ 0 ] )
        {
            s_interaction.active_id     = sid;
            s_interaction.active_button = 0;   /* released globally when the left button lifts */
        }
    }

    if ( active )
    {
        hot = true;
        f32 ext  = ( n->split == DOCK_SPLIT_X ) ? ( r.w - thick ) : ( r.h - thick );
        if ( ext < 1.0f ) ext = 1.0f;
        f32 rel  = ( n->split == DOCK_SPLIT_X ) ? ( ( s_io.mouse_x - r.x ) / ext )
                                                : ( ( s_io.mouse_y - r.y ) / ext );
        f32 minr = DOCK_MIN_PANE / ext;
        if ( minr > 0.45f ) minr = 0.45f;     /* keep a usable range on a tiny node */
        n->ratio = clampf( rel, minr, 1.0f - minr );
    }

    /* Directional hardware cursor while the gutter is hot or being dragged: an X split divides
       horizontally (a left/right drag -> EW), a Y split vertically (up/down -> NS). */
    if ( hot || active )
        set_mouse_cursor( ( n->split == DOCK_SPLIT_X ) ? APP_CURSOR_RESIZE_EW : APP_CURSOR_RESIZE_NS );

    draw_push_rect_filled( sr.x, sr.y, sr.w, sr.h, 0, 0, 1, 1, 0, ( hot || active ) ? COL_RESIZE_HOT : COL_BORDER );
}

/* Post-order walk: lay splitters of the children before this node's own, so a parent gutter paints
   over the child borders it abuts. */
static void
dock_tree_splitters( gui_dock_node_t* n, u32 vp )
{
    if ( !n || n->split == DOCK_SPLIT_NONE )
        return;
    dock_tree_splitters( n->child[ 0 ], vp );
    dock_tree_splitters( n->child[ 1 ], vp );
    dock_splitter( n, vp );
}

/* Empty leaves (no window tabbed, or none emitted yet) get a placeholder fill + border so a bare
   dockspace region reads as a drop target rather than a hole.  A leaf with windows is painted by the
   active window's body + dock_window_chrome instead. */
static void
dock_tree_placeholders( gui_dock_node_t* n )
{
    if ( !n )
        return;
    if ( n->split == DOCK_SPLIT_NONE )
    {
        if ( n->tab_count == 0 )
        {
            draw_set_rounding( 0.0f );   /* empty node tiles flush in the dock grid -- keep it square */
            draw_push_rect_filled ( n->rect.x, n->rect.y, n->rect.w, n->rect.h, 0, 0, 1, 1, 0, COL_CHILD_BG );
            draw_push_rect_outline( n->rect.x, n->rect.y, n->rect.w, n->rect.h, WIN_BORDER, 0, COL_BORDER );
        }
        return;
    }
    dock_tree_placeholders( n->child[ 0 ] );
    dock_tree_placeholders( n->child[ 1 ] );
}

// clang-format on
/*============================================================================================*/
