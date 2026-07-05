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

/* dock_ref/dock_at convert between a live pointer and its pool index (gui_dock_ref_t).  The pool
   never moves or compacts a live slot, so an index survives exactly as long as the pointer it
   replaces would -- see gui_dock_ref_t in gui_internal.h for why this trades 8 bytes for 2. */
static gui_dock_ref_t
dock_ref( gui_dock_node_t* n )
{
    return n ? (gui_dock_ref_t)( n - s_dock_nodes ) : GUI_DOCK_REF_NONE;
}

static gui_dock_node_t*
dock_at( gui_dock_ref_t ref )
{
    return ( ref == GUI_DOCK_REF_NONE ) ? NULL : &s_dock_nodes[ ref ];
}

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
    /* GUI_DOCK_REF_NONE is 0xFFFF, not 0 -- index 0 is a real pool slot -- so the memset above does
       NOT leave these "unlinked"; they must be set explicitly. */
    n->parent   = GUI_DOCK_REF_NONE;
    n->child[ 0 ] = n->child[ 1 ] = GUI_DOCK_REF_NONE;
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
    gui_dock_node_t* parent = dock_at( leaf->parent );
    if ( !parent )
        return;   /* root leaf emptied -- keep the bare dockspace */

    gui_dock_ref_t   leaf_ref = dock_ref( leaf );
    gui_dock_node_t* sib      = dock_at( parent->child[ 0 ] == leaf_ref ? parent->child[ 1 ] : parent->child[ 0 ] );
    gui_dock_node_t* gp       = dock_at( parent->parent );

    sib->parent = dock_ref( gp );
    if ( !gp )
        g_ctx->viewports[ parent->viewport ].dock_root = dock_ref( sib );
    else
        gp->child[ gp->child[ 0 ] == dock_ref( parent ) ? 0 : 1 ] = dock_ref( sib );

    dock_node_free( leaf );
    dock_node_free( parent );
}

/* Remove window `wid`'s tab from `n` (if present) and collapse `n` should that empty it -- the
   shared undock step every removal path (gui_dock_window's re-dock, gui_dock_undock,
   dock_undock_by_id) performs identically before doing anything path-specific of its own. */
static void
dock_node_remove_window( gui_dock_node_t* n, gui_id_t wid )
{
    for ( u32 i = 0; i < n->tab_count; ++i )
        if ( n->tabs[ i ] == wid ) { dock_leaf_remove_tab( n, i ); break; }
    if ( n->tab_count == 0 )
        dock_collapse( n );
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
        dock_node_layout( dock_at( n->child[ 0 ] ), ( gui_rect_t ){ r.x,             r.y, w0,               r.h } );
        dock_node_layout( dock_at( n->child[ 1 ] ), ( gui_rect_t ){ r.x + w0 + thick, r.y, r.w - w0 - thick, r.h } );
    }
    else /* DOCK_SPLIT_Y */
    {
        f32 avail = r.h - thick; if ( avail < 0.0f ) avail = 0.0f;
        f32 h0    = floorf( avail * n->ratio );
        dock_node_layout( dock_at( n->child[ 0 ] ), ( gui_rect_t ){ r.x, r.y,             r.w, h0               } );
        dock_node_layout( dock_at( n->child[ 1 ] ), ( gui_rect_t ){ r.x, r.y + h0 + thick, r.w, r.h - h0 - thick } );
    }
}

/*----------------------------------------------------------------------------------------------
    Splitter drag -- move the gutter, do not rescale the rest of the tree.

    dock_node_layout stores a split as a RATIO, so a naive ratio drag rescales every descendant
    pane proportionally (an editor's side panels would all breathe as one divider moves).  The
    editor rule is: a splitter drag moves ONLY that gutter -- the two panes touching it give or
    take the pixels, every other pane keeps its exact pixel size.  Ratios stay the storage (so an
    OS viewport resize still scales the whole tree proportionally, the desired resize behavior);
    the drag simply REWRITES the ratios of the affected descendants so their gutters keep their
    absolute pixel positions.

    Within each child subtree the pixels are absorbed by the "near chain": the panes touching the
    dragged gutter.  side names which end of the subtree that is (1 = its right/bottom edge, i.e.
    the child[1]-most chain; 0 = left/top).  A same-axis descendant split re-ratios so its FAR
    child keeps its pixels and recurses into the near child; a cross-axis split spans the full
    extent on this axis, so both children absorb the same delta.
----------------------------------------------------------------------------------------------*/

/* Node extent in pixels along a split axis (this frame's resolved rect). */
static f32
dock_axis_ext( const gui_dock_node_t* n, u8 axis )
{
    return ( axis == DOCK_SPLIT_X ) ? n->rect.w : n->rect.h;
}

/* How many pixels subtree n can give up along `axis` before a pane on its `side` near chain hits
   DOCK_MIN_PANE -- the drag clamp, so a deep pane never collapses to nothing. */
static f32
dock_shrink_capacity( gui_dock_node_t* n, u8 axis, u32 side )
{
    if ( !n ) return 0.0f;
    if ( n->split == DOCK_SPLIT_NONE )
    {
        f32 c = dock_axis_ext( n, axis ) - DOCK_MIN_PANE;
        return c > 0.0f ? c : 0.0f;
    }
    if ( n->split == axis )   /* only the near child shrinks; the far one keeps its pixels */
        return dock_shrink_capacity( dock_at( n->child[ side ] ), axis, side );
    /* Cross split: both children shrink by the full delta -- the tighter one limits. */
    f32 a = dock_shrink_capacity( dock_at( n->child[ 0 ] ), axis, side );
    f32 b = dock_shrink_capacity( dock_at( n->child[ 1 ] ), axis, side );
    return a < b ? a : b;
}

/* Subtree n's extent along `axis` is about to change by `delta` (its `side` edge moves).  Rewrite
   descendant same-axis ratios so every pane NOT on the near chain keeps its pixel size. */
static void
dock_absorb_delta( gui_dock_node_t* n, u8 axis, u32 side, f32 delta )
{
    if ( !n || n->split == DOCK_SPLIT_NONE || delta == 0.0f )
        return;
    if ( n->split != axis )
    {
        dock_absorb_delta( dock_at( n->child[ 0 ] ), axis, side, delta );
        dock_absorb_delta( dock_at( n->child[ 1 ] ), axis, side, delta );
        return;
    }
    f32 avail_new = dock_axis_ext( n, axis ) - DOCK_SPLITTER + delta;
    if ( avail_new < 1.0f ) avail_new = 1.0f;

    /* The far child keeps its pixels; child[0]'s new extent follows from which side is far. */
    f32 far_px = dock_axis_ext( dock_at( n->child[ side ^ 1u ] ), axis );
    f32 c0_new = ( side == 1u ) ? far_px : ( avail_new - far_px );
    n->ratio   = clampf( c0_new / avail_new, 0.02f, 0.98f );

    dock_absorb_delta( dock_at( n->child[ side ] ), axis, side, delta );
}

/*----------------------------------------------------------------------------------------------
    Splitter interaction + draw (one internal node)

    The gutter sits between the children and over no window, so hover_win is NONE there unless a
    floating window covers it -- gating the grab on hover_win == NONE thus naturally yields to a
    floater drawn on top.  Grab sets active_id (released globally when the button lifts, like a window
    drag); while held, the gutter tracks the cursor as a pixel DELTA absorbed by the adjacent panes
    (see dock_absorb_delta above), clamped so no pane on either near chain shrinks below DOCK_MIN_PANE.
----------------------------------------------------------------------------------------------*/

static void
dock_splitter( gui_dock_node_t* n, u32 vp )
{
    gui_rect_t r     = n->rect;
    f32          thick = DOCK_SPLITTER;
    gui_rect_t sr;
    if ( n->split == DOCK_SPLIT_X )
        sr = ( gui_rect_t ){ dock_at( n->child[ 1 ] )->rect.x - thick, r.y, thick, r.h };
    else
        sr = ( gui_rect_t ){ r.x, dock_at( n->child[ 1 ] )->rect.y - thick, r.w, thick };

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
        u8  axis = n->split;
        gui_dock_node_t* c0 = dock_at( n->child[ 0 ] );
        gui_dock_node_t* c1 = dock_at( n->child[ 1 ] );

        f32 avail = ( ( axis == DOCK_SPLIT_X ) ? r.w : r.h ) - thick;
        if ( avail < 1.0f ) avail = 1.0f;

        /* Cursor-desired child[0] extent -> a pixel delta against this frame's resolved rect,
           clamped so no pane on either side's near chain shrinks below DOCK_MIN_PANE. */
        f32 old0  = dock_axis_ext( c0, axis );
        f32 want0 = ( axis == DOCK_SPLIT_X ) ? ( s_io.mouse_x - r.x ) : ( s_io.mouse_y - r.y );
        f32 delta = want0 - old0;
        f32 cap0  = dock_shrink_capacity( c0, axis, 1u );
        f32 cap1  = dock_shrink_capacity( c1, axis, 0u );
        if ( delta < -cap0 ) delta = -cap0;
        if ( delta >  cap1 ) delta =  cap1;

        n->ratio = clampf( ( old0 + delta ) / avail, 0.02f, 0.98f );

        /* Everything not touching this gutter keeps its pixel size (editor splitter rule). */
        dock_absorb_delta( c0, axis, 1u,  delta );
        dock_absorb_delta( c1, axis, 0u, -delta );
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
    dock_tree_splitters( dock_at( n->child[ 0 ] ), vp );
    dock_tree_splitters( dock_at( n->child[ 1 ] ), vp );
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
    dock_tree_placeholders( dock_at( n->child[ 0 ] ) );
    dock_tree_placeholders( dock_at( n->child[ 1 ] ) );
}

// clang-format on
/*============================================================================================*/
