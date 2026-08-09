/*==============================================================================================

    runtime_service/gui/chrome/dock/gui_dock.c -- Docking: the public build API.

    The programmatic verbs a host uses to build a dock layout in code: dockspace_over_viewport (lay
    out + chrome a viewport's tree once per frame), dock_split / dock_split_root (carve new leaves),
    dock_window / dock_undock / window_is_docked (tab a window in or out).  How a docked window
    actually renders is gui_window_free.c's job (window_begin_ex routes through the route seam,
    chrome/dock/gui_dock_route.c, into window_begin_docked); the node pool + per-frame layout live in gui_dock_core.c; the mouse
    drag-to-dock / undock-by-tab-drag gestures + tab-strip chrome live in gui_dock_drag.c; layout
    save/load lives in gui_dock_serialize.c.

    This programmatic path is the foundation: a host builds a whole layout in code and the windows
    tile + tab into it.  The mouse drag-to-dock gestures (gui_dock_drag.c) and layout persistence
    (gui_dock_serialize.c) build on the same node tree -- the drag path drives these very verbs, while
    the loader rebuilds the tree straight from the gui_dock_core.c node pool.

    Included by gui_chrome.c after gui_dock_core.c + gui_dock_drag.c (and before gui_dock_serialize.c, though
    that ordering doesn't matter -- this file and gui_dock_serialize.c don't call each other).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Public API
==============================================================================================*/

/* Publish the host-reserved top band above viewport vp's dock area -- the height of a main menu
   bar and/or toolbar strip the host draws itself.  The dock tree lays out below it (in addition
   to any native caption band).  Sticky until re-published; pass 0 to reclaim the space. */
void
gui_dockspace_inset( i32 vp, f32 top )
{
    if ( vp < 0 || vp >= GUI_MAX_VIEWPORTS )
        return;
    f32 inset = ( top > 0.0f ) ? top : 0.0f;
    if ( s_vp_pool[ vp ].dock_inset == inset )
        return;                  /* native shells republish the caption band EVERY frame -- an
                                    unconditional raise here would defeat idle skip permanently. */
    s_vp_pool[ vp ].dock_inset = inset;
    redraw_request();            /* a changed inset retiles the whole dockspace */
}

/* Ensure viewport vp hosts a dock tree, lay it out over the surface (below any native caption band),
   draw + interact the splitters and empty-leaf placeholders, and return the tree root's id.  Call
   once per frame at the TOP of the build for each dockspace viewport, before the docked windows'
   window_begin run (they read their node's resolved content rect). */
gui_dock_id_t
gui_dockspace_over_viewport( i32 vp, gui_dockspace_flags_t flags )
{
    if ( !g_ctx->dock.pool ) return GUI_DOCK_NONE;   /* pool disabled for this context (max_dock_nodes == 0) */
    if ( vp < 0 || vp >= GUI_MAX_VIEWPORTS )
        return GUI_DOCK_NONE;

    /* Mixed DPI: the tree's node rects (strip heights, splitter thickness) resolve from s_style,
       so land this surface's bake before the layout below.  No-op when already landed. */
    gui_dpi_land( vp );

    gui_viewport_t*  v    = &s_vp_pool[ vp ];
    v->dock_flags      = flags;   /* re-published each frame; NO_SPLIT gates the chips + split verbs */
    v->dock_seen_frame = gui_frame_index();   /* tree ACTIVE this build; unstamped = dormant */
    gui_dock_node_t* root = dock_at( v->dock_root );
    if ( !root )
    {
        root = dock_node_alloc( vp );
        if ( !root )
            return GUI_DOCK_NONE;
        v->dock_root = dock_ref( root );
    }

    f32 dw  = vp_w( vp );
    f32 dh  = vp_h( vp );
    f32 top = v->caption_inset + v->dock_inset;
    gui_rect_t area = { 0.0f, top, dw, dh - top };
    if ( area.h < 0.0f ) area.h = 0.0f;
    dock_node_layout( root, area );

    /* Chrome on this surface, below the free-floating windows (z 0), clipped to the surface.
       Skipped while a maximized leaf's cover is settled: everything it would draw sits fully
       obscured under the cover (during the tween the siblings still peek out, so it keeps
       drawing then -- from the PURE tree rects, before the override below moves the leaf). */
    if ( !v->dock_max_settled )
    {
        draw_set_viewport ( vp );
        draw_set_sort_key ( 0 );
        draw_set_root_clip( dw, dh );
        dock_tree_placeholders( root );
        dock_tree_splitters   ( root, vp );

        /* Restore the ambient build state for the windows emitted next. */
        draw_set_sort_key ( 0 );
        draw_set_viewport ( 0 );
        draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );
    }

    /* Maximized leaf: override its tree rect with the eased cover.  Runs AFTER the splitter
       chrome so the gutters keep their tree positions while the cover slides over them.  The
       target re-aims every frame -- the full dock area while maximized (so it tracks a live
       surface resize), the tree rect the layout above just resolved while restoring -- and the
       timer forces t == 1 on its final frame, landing the lerp exactly on the target.  Once a
       restore settles the node is a plain tile again: z back to the dock floor, id cleared. */
    gui_dock_node_t* mx = dock_max_node( vp );
    if ( mx )
    {
        gui_rect_t target = v->dock_max_on ? area : mx->rect;
        bool       active = false;
        f32        t      = gui_anim_timer( id_combine( mx->id, DOCK_MAX_SALT ), feat_ease, &active );

        mx->rect.x = f32_lerp( v->dock_max_from.x, target.x, t );
        mx->rect.y = f32_lerp( v->dock_max_from.y, target.y, t );
        mx->rect.w = f32_lerp( v->dock_max_from.w, target.w, t );
        mx->rect.h = f32_lerp( v->dock_max_from.h, target.h, t );

        /* Re-carve the tab strip off the moved rect, exactly like dock_node_layout's leaf case. */
        dock_leaf_carve_content( mx );

        v->dock_max_settled = v->dock_max_on && !active;
        if ( !v->dock_max_on && !active )
        {
            mx->z          = 0;   /* restore settled: back among the tiles */
            v->dock_max_id = 0;
        }
    }

    return root->id;
}

/* Wire `internal` as a GUI_DOCK_SPLIT_X/Y node dividing `new_node` and `other` along `dir`, `ratio` the
   fraction of the axis `new_node` receives.  child[0] is always the left/top side, so when the new
   side is child[1] (RIGHT / DOWN) `other` gets the complementary ratio.  Links both children's
   parent to `internal`; leaves internal->parent untouched (a converted leaf keeps its own parent, a
   fresh wrapper node sets it explicitly) -- the one difference between dock_split's and
   dock_split_root's otherwise identical wiring. */
static void
dock_node_wire_split( gui_dock_node_t* internal, gui_dir_t dir, f32 ratio,
                      gui_dock_node_t* new_node, gui_dock_node_t* other )
{
    bool horizontal = ( dir == GUI_DIR_LEFT || dir == GUI_DIR_RIGHT );
    bool new_first  = ( dir == GUI_DIR_LEFT || dir == GUI_DIR_UP );   /* new on child[0] side */
    f32  r          = clampf( ratio, 0.05f, 0.95f );

    internal->split    = horizontal ? GUI_DOCK_SPLIT_X : GUI_DOCK_SPLIT_Y;
    internal->child[ 0 ] = dock_ref( new_first ? new_node : other );
    internal->child[ 1 ] = dock_ref( new_first ? other    : new_node );
    internal->ratio      = new_first ? r : ( 1.0f - r );
    new_node->parent = dock_ref( internal );
    other->parent    = dock_ref( internal );
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
    if ( !n || n->split != GUI_DOCK_SPLIT_NONE )
        return GUI_DOCK_NONE;
    if ( n->floating )
        return GUI_DOCK_NONE;   /* a floating tab group is tabs-only by definition */
    if ( s_vp_pool[ n->viewport ].dock_flags & GUI_DOCKSPACE_NO_SPLIT )
        return GUI_DOCK_NONE;   /* tab docking only on this dockspace */

    gui_dock_node_t* a = dock_node_alloc( n->viewport );
    gui_dock_node_t* b = dock_node_alloc( n->viewport );
    if ( !a || !b )
    {
        if ( a ) dock_node_free( a );
        if ( b ) dock_node_free( b );
        return GUI_DOCK_NONE;
    }

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

    /* Convert n into an internal split; n keeps whatever parent it already had. */
    dock_node_wire_split( n, dir, ratio, new_node, remain );

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
gui_dock_split_root( i32 vp, gui_dir_t dir, f32 ratio )
{
    if ( !g_ctx->dock.pool ) return GUI_DOCK_NONE;
    if ( vp < 0 || vp >= GUI_MAX_VIEWPORTS )
        return GUI_DOCK_NONE;

    gui_viewport_t*  v    = &s_vp_pool[ vp ];
    if ( v->dock_flags & GUI_DOCKSPACE_NO_SPLIT )
        return GUI_DOCK_NONE;   /* tab docking only on this dockspace */
    gui_dock_node_t* root = dock_at( v->dock_root );
    if ( !root )
        return GUI_DOCK_NONE;

    /* An empty single-leaf root: nothing to wrap, divide it directly. */
    if ( root->split == GUI_DOCK_SPLIT_NONE && root->tab_count == 0 )
        return gui_dock_split( root->id, dir, ratio, NULL );

    gui_dock_node_t* leaf  = dock_node_alloc( vp );   /* the new edge pane          */
    gui_dock_node_t* inner = dock_node_alloc( vp );   /* the wrapper split          */
    if ( !leaf || !inner )
    {
        if ( leaf )  dock_node_free( leaf );
        if ( inner ) dock_node_free( inner );
        return GUI_DOCK_NONE;
    }

    dock_node_wire_split( inner, dir, ratio, leaf, root );
    inner->parent = GUI_DOCK_REF_NONE;   /* inner is a fresh wrapper -- it has no parent of its own */

    v->dock_root = dock_ref( inner );   /* the wrapper is the new tree root */
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
    if ( !n || n->split != GUI_DOCK_SPLIT_NONE || n->tab_count >= GUI_DOCK_TABS_MAX )
        return;

    dock_leaf_tab_add( n, id_hash( title ), title );
    redraw_request();   /* the retiled layout appears next build (the interactive drop path already
                           raises this in gui_dock_core.c; the programmatic verb must match) */
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
    dock_node_remove_window( n, wid );
    redraw_request();   /* undock retiles the tree: next build, same as dock_window above */
}

bool
gui_window_is_docked( const char* title )
{
    return title != NULL && dock_find_window_node( id_hash( title ) ) != NULL;
}

/* Maximize (or restore) a docked window's node over its whole dockspace -- the programmatic twin
   of the tab strip's maximize button (GUI_WIN_DOCK_MAXIMIZE gates only the button; this verb works
   regardless, so a host can bind fullscreen to a hotkey without offering the chrome).  Maximizing
   also makes the window its node's active tab -- fullscreening a background tab should show it.
   No-op if the window is not tree-docked (free-floating and floating tab groups have the floater
   maximize instead). */
void
gui_dock_window_maximize( const char* title, bool on )
{
    if ( !title )
        return;
    gui_id_t         wid = id_hash( title );
    gui_dock_node_t* n   = dock_find_window_node( wid );
    if ( !n || n->floating )
        return;

    if ( on )
        for ( u32 i = 0; i < n->tab_count; ++i )
            if ( n->tabs[ i ] == wid ) { n->active_tab = i; break; }

    gui_viewport_t* v = &s_vp_pool[ n->viewport ];
    if ( ( v->dock_max_id == n->id && v->dock_max_on ) != on )
        dock_max_set( n, on );
}

/* True while the window's node holds (or is entering) the dockspace maximize. */
bool
gui_window_is_dock_maximized( const char* title )
{
    if ( !title )
        return false;
    gui_dock_node_t* n = dock_find_window_node( id_hash( title ) );
    if ( !n || n->floating )
        return false;
    const gui_viewport_t* v = &s_vp_pool[ n->viewport ];
    return v->dock_max_id == n->id && v->dock_max_on;
}

// clang-format on
/*============================================================================================*/
