/*==============================================================================================

    runtime_service/gui/dock/gui_dock.c -- Docking: the public build API.

    The programmatic verbs a host uses to build a dock layout in code: dockspace_over_viewport (lay
    out + chrome a viewport's tree once per frame), dock_split / dock_split_root (carve new leaves),
    dock_window / dock_undock / window_is_docked (tab a window in or out).  How a docked window
    actually renders is gui_widget_window.c's job (window_begin_ex routes through dock_find_window_node
    / window_begin_docked); the node pool + per-frame layout live in gui_dock_core.c; the mouse
    drag-to-dock / undock-by-tab-drag gestures + tab-strip chrome live in gui_dock_drag.c; layout
    save/load lives in gui_dock_serialize.c.

    Phase 1 is programmatic only: a host builds a layout in code and the windows tile + tab into it.
    Mouse drag-to-dock (gui_dock_drag.c) and layout persistence (gui_dock_serialize.c) are the later
    phases built on top.

    Included by gui.c after gui_dock_core.c + gui_dock_drag.c (and before gui_dock_serialize.c, though
    that ordering doesn't matter -- this file and gui_dock_serialize.c don't call each other).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Public API
----------------------------------------------------------------------------------------------*/

/* Ensure viewport vp hosts a dock tree, lay it out over the surface (below any native caption band),
   draw + interact the splitters and empty-leaf placeholders, and return the tree root's id.  Call
   once per frame at the TOP of the build for each dockspace viewport, before the docked windows'
   window_begin run (they read their node's resolved content rect). */
gui_dock_id_t
gui_dockspace_over_viewport( gui_vp_t vp, gui_dockspace_flags_t flags )
{
    UNUSED( flags );
    if ( !s_dock_nodes ) return GUI_DOCK_NONE;   /* docking disabled */
    if ( vp < 0 || vp >= (gui_vp_t)g_ctx->max_viewports )
        return GUI_DOCK_NONE;

    gui_viewport_t* v = &g_ctx->viewports[ vp ];
    if ( !v->dock_root )
    {
        gui_dock_node_t* root = dock_node_alloc( (u32)vp );
        if ( !root )
            return GUI_DOCK_NONE;
        v->dock_root = root;
    }

    f32 dw  = vp_w( v );
    f32 dh  = vp_h( v );
    f32 top = v->caption_inset;
    gui_rect_t area = { 0.0f, top, dw, dh - top };
    if ( area.h < 0.0f ) area.h = 0.0f;
    dock_node_layout( v->dock_root, area );

    /* Chrome on this surface, below the free-floating windows (z 0), clipped to the surface. */
    draw_set_viewport ( (u32)vp );
    draw_set_sort_key ( 0 );
    draw_set_root_clip( dw, dh );
    dock_tree_placeholders( v->dock_root );
    dock_tree_splitters   ( v->dock_root, (u32)vp );

    /* Restore the ambient build state for the windows emitted next. */
    draw_set_sort_key ( 0 );
    draw_set_viewport ( 0 );
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );

    return v->dock_root->id;
}

/* Split a LEAF node in two: the original node becomes an internal split, its windows move to the
   "remaining" child, and a new empty leaf is created on the `dir` side.  Returns the new leaf's id
   (dock windows into it); writes the remaining child's id to *out_remain (may be NULL), so a caller
   can keep splitting the shrinking remainder -- the DockBuilder idiom.  `ratio` is the fraction of
   the axis the NEW side receives.  A no-op (returns GUI_DOCK_NONE) if node is not a leaf or the pool
   is full. */
gui_dock_id_t
gui_dock_split( gui_dock_id_t node_id, gui_dir_t dir, f32 ratio, gui_dock_id_t* out_remain )
{
    if ( out_remain ) *out_remain = node_id;

    gui_dock_node_t* n = dock_node_find( node_id );
    if ( !n || n->split != DOCK_SPLIT_NONE )
        return GUI_DOCK_NONE;

    gui_dock_node_t* a = dock_node_alloc( n->viewport );
    gui_dock_node_t* b = dock_node_alloc( n->viewport );
    if ( !a || !b )
    {
        if ( a ) dock_node_free( a );
        if ( b ) dock_node_free( b );
        return GUI_DOCK_NONE;
    }

    bool horizontal = ( dir == GUI_DIR_LEFT || dir == GUI_DIR_RIGHT );
    bool new_first  = ( dir == GUI_DIR_LEFT || dir == GUI_DIR_UP );   /* new on child[0] side */
    gui_dock_node_t* new_node = new_first ? a : b;
    gui_dock_node_t* remain   = new_first ? b : a;

    /* Move the original node's tabs onto the remaining child. */
    remain->tab_count  = n->tab_count;
    remain->active_tab = n->active_tab;
    for ( u32 i = 0; i < n->tab_count; ++i )
    {
        remain->tabs[ i ] = n->tabs[ i ];
        memcpy( remain->names[ i ], n->names[ i ], GUI_DOCK_NAME_CAP );
    }

    f32 r = clampf( ratio, 0.05f, 0.95f );

    /* Convert n into an internal split.  child[0] is the left / top side; ratio is its fraction, so
       when the NEW side is child[1] (RIGHT / DOWN) the remaining child[0] gets the complement. */
    n->split    = horizontal ? DOCK_SPLIT_X : DOCK_SPLIT_Y;
    n->child[ 0 ] = new_first ? new_node : remain;
    n->child[ 1 ] = new_first ? remain   : new_node;
    n->ratio      = new_first ? r : ( 1.0f - r );
    new_node->parent = n;
    remain->parent   = n;

    /* n no longer holds windows directly. */
    n->tab_count  = 0;
    n->active_tab = 0;
    memset( n->tabs,  0, sizeof n->tabs  );
    memset( n->names, 0, sizeof n->names );

    if ( out_remain ) *out_remain = remain->id;
    return new_node->id;
}

/* Split the whole viewport tree, carving a new empty leaf along a FULL edge (`dir`) of the dockspace.
   Unlike dock_split (which only divides a single leaf), this wraps the current root in a fresh internal
   node so the new pane spans the entire side -- the way to put, say, a full-height left column beside an
   existing top/bottom stack.  `ratio` is the new edge pane's fraction of the axis.  Returns the new
   leaf's id, or GUI_DOCK_NONE if the viewport has no tree or the pool is full.  A bare empty root leaf
   is just split in place (no wrapper needed). */
gui_dock_id_t
gui_dock_split_root( gui_vp_t vp, gui_dir_t dir, f32 ratio )
{
    if ( !s_dock_nodes ) return GUI_DOCK_NONE;
    if ( vp < 0 || vp >= (gui_vp_t)g_ctx->max_viewports )
        return GUI_DOCK_NONE;

    gui_viewport_t*  v    = &g_ctx->viewports[ (u32)vp ];
    gui_dock_node_t* root = v->dock_root;
    if ( !root )
        return GUI_DOCK_NONE;

    /* An empty single-leaf root: nothing to wrap, divide it directly. */
    if ( root->split == DOCK_SPLIT_NONE && root->tab_count == 0 )
        return gui_dock_split( root->id, dir, ratio, NULL );

    gui_dock_node_t* leaf  = dock_node_alloc( (u32)vp );   /* the new edge pane          */
    gui_dock_node_t* inner = dock_node_alloc( (u32)vp );   /* the wrapper split          */
    if ( !leaf || !inner )
    {
        if ( leaf )  dock_node_free( leaf );
        if ( inner ) dock_node_free( inner );
        return GUI_DOCK_NONE;
    }

    bool horizontal = ( dir == GUI_DIR_LEFT || dir == GUI_DIR_RIGHT );
    bool new_first  = ( dir == GUI_DIR_LEFT || dir == GUI_DIR_UP );   /* new on child[0] side */
    f32  r          = clampf( ratio, 0.05f, 0.95f );

    inner->split    = horizontal ? DOCK_SPLIT_X : DOCK_SPLIT_Y;
    inner->child[ 0 ] = new_first ? leaf : root;
    inner->child[ 1 ] = new_first ? root : leaf;
    inner->ratio      = new_first ? r : ( 1.0f - r );
    inner->parent     = NULL;
    leaf->parent      = inner;
    root->parent      = inner;

    v->dock_root = inner;   /* the wrapper is the new tree root */
    return leaf->id;
}

/* Add a window (matched to window_begin by id_hash(title)) as a tab in a LEAF node, removing it from
   any node it was previously docked in.  The display name is the title's visible span (any "##" id
   suffix stripped), copied so the tab bar is self-sufficient.  The newly docked window becomes the
   active tab.  No-op if node is not a leaf or its tab list is full. */
void
gui_dock_window( const char* title, gui_dock_id_t node_id )
{
    if ( !title )
        return;
    gui_dock_node_t* n = dock_node_find( node_id );
    if ( !n || n->split != DOCK_SPLIT_NONE || n->tab_count >= GUI_DOCK_TABS_MAX )
        return;

    gui_id_t wid = id_hash( title );

    gui_dock_node_t* prev = dock_find_window_node( wid );
    if ( prev == n )
        return;   /* already here */
    if ( prev )
    {
        for ( u32 i = 0; i < prev->tab_count; ++i )
            if ( prev->tabs[ i ] == wid ) { dock_leaf_remove_tab( prev, i ); break; }
        if ( prev->tab_count == 0 )
            dock_collapse( prev );
    }

    u32 idx = n->tab_count++;
    n->tabs[ idx ] = wid;
    u32 vis = label_vis_len( title );
    if ( vis >= GUI_DOCK_NAME_CAP ) vis = GUI_DOCK_NAME_CAP - 1;
    memcpy( n->names[ idx ], title, vis );
    n->names[ idx ][ vis ] = '\0';
    n->active_tab = idx;
}

/* Remove a window from its node, returning it to free-floating.  A node emptied by this is collapsed
   (its sibling takes its place).  No-op if the window is not docked. */
void
gui_dock_undock( const char* title )
{
    if ( !title )
        return;
    gui_id_t wid = id_hash( title );
    gui_dock_node_t* n = dock_find_window_node( wid );
    if ( !n )
        return;
    for ( u32 i = 0; i < n->tab_count; ++i )
        if ( n->tabs[ i ] == wid ) { dock_leaf_remove_tab( n, i ); break; }
    if ( n->tab_count == 0 )
        dock_collapse( n );
}

bool
gui_window_is_docked( const char* title )
{
    return title != NULL && dock_find_window_node( id_hash( title ) ) != NULL;
}

// clang-format on
/*============================================================================================*/
