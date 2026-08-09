/*==============================================================================================

    runtime_service/gui/chrome/dock/gui_dock_core.c -- Dock-node tree: pool, layout, splitter chrome.

    The node pool (alloc/free/find), the per-frame layout pass that assigns every node a rect from
    the surface extent down, the leaf tab-list edit + tree-collapse helpers every mutation path
    (docking, undocking, drag-to-dock, load) shares, and the splitter interaction + empty-leaf
    placeholder chrome drawn once per dockspace.  Everything else that touches a dock tree -- the
    public build API (gui_dock.c), the mouse drag-to-dock / undock-by-tab-drag gestures + tab-strip
    chrome (gui_dock_drag.c), layout save/load (gui_dock_serialize.c) -- is built on these
    primitives and lives in the sibling dock/ files, included after this one.

    Included by gui_chrome.c first among the dock/ files: dock_node_alloc / _free / _find /
    dock_leaf_remove_tab / dock_collapse are plain statics, not forward-declared in chrome/gui_chrome.h,
    so every other dock file needs this one already in scope.  (window/ reaches
    dock_find_window_node only through the route seam, chrome/dock/gui_dock_route.c.)

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Constants

    The tab strip stands in for a window's title bar, so it shares the title-bar height (WIN_TITLE_H)
    -- a docked window thus reads at the same vertical rhythm as a free one.  The splitter is a thin
    grabbable gutter reserved BETWEEN sibling rects (not over them), so a press on it never collides
    with a docked window's content -- the cursor sits where no window nominates hover.
==============================================================================================*/

#define DOCK_SPLITTER   6.0f                  /* splitter gutter thickness, in pixels            */
#define DOCK_MIN_PANE   ( WIN_TITLE_H * 2.0f) /* a pane never shrinks below this on the split axis */

/* Salt: a splitter gets a stable per-node widget id distinct from the windows + chrome.  The tab
   salt (DOCK_TAB_SALT) lives in gui_dock_drag.c, next to the tab-strip chrome that uses it. */
#define DOCK_SPLIT_SALT 0xD0C5B17u

/* Salt for a node's maximize-over-dockspace anim timer slot (see dock_max_set below). */
#define DOCK_MAX_SALT   0xD0C3A400u

/*==============================================================================================
    Node pool

    Fixed per-context array so child / parent pointers stay valid across frames (no compaction).  A
    free slot has id == 0; alloc reuses the first freed hole or appends, and never returns id 0.
==============================================================================================*/

/* dock_ref/dock_at convert between a live pointer and its pool index (gui_dock_ref_t).  The pool
   never moves or compacts a live slot, so an index survives exactly as long as the pointer it
   replaces would -- see gui_dock_ref_t in core/gui_ctx.h for why this trades 8 bytes for 2. */
static gui_dock_ref_t
dock_ref( gui_dock_node_t* n )
{
    return n ? (gui_dock_ref_t)( n - g_ctx->dock.pool ) : GUI_DOCK_REF_NONE;
}

static gui_dock_node_t*
dock_at( gui_dock_ref_t ref )
{
    return ( ref == GUI_DOCK_REF_NONE ) ? NULL : &g_ctx->dock.pool[ ref ];
}

static gui_dock_node_t*
dock_node_alloc( u32 viewport )
{
    if ( !g_ctx->dock.pool ) return NULL;   /* docking disabled for this context */
    gui_dock_node_t* n = NULL;
    for ( u32 i = 0; i < g_ctx->dock.count; ++i )      /* reuse a freed hole first */
        if ( g_ctx->dock.pool[ i ].id == 0 ) { n = &g_ctx->dock.pool[ i ]; break; }
    if ( !n )
    {
        if ( g_ctx->dock.count >= g_ctx->dock.max )
            return NULL;
        n = &g_ctx->dock.pool[ g_ctx->dock.count++ ];
    }
    memset( n, 0, sizeof *n );
    n->id       = ++g_ctx->dock.id_seq;   /* monotonic; never 0 */
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
    for ( u32 i = 0; i < g_ctx->dock.count; ++i )
        if ( g_ctx->dock.pool[ i ].id == id )
            return &g_ctx->dock.pool[ i ];
    return NULL;
}

/* True only on frames the host emitted dockspace_over_viewport for vp (its stamp matches this
   build's frame clock -- the clock advances at ctx_begin, so equality means "emitted in the
   current build").  A live tree without emission is DORMANT: retained, but it places no windows
   and offers no drops (see dock_seen_frame in core/gui_ctx.h). */
static bool
dock_vp_emitted( u32 vp )
{
    const gui_viewport_t* v = &s_vp_pool[ vp ];
    return v->dock_root != GUI_DOCK_REF_NONE && v->dock_seen_frame == gui_frame_index();
}

/* The lookup window_begin routes through: which LEAF tabs this window, or NULL.  Forward-declared in
   chrome/gui_chrome.h so gui_window_free.c (included earlier) can call it. */
static gui_dock_node_t*
dock_find_window_node( gui_id_t win )
{
    if ( !win ) return NULL;
    for ( u32 i = 0; i < g_ctx->dock.count; ++i )
    {
        gui_dock_node_t* n = &g_ctx->dock.pool[ i ];
        if ( n->id == 0 || n->split != GUI_DOCK_SPLIT_NONE )
            continue;
        for ( u32 t = 0; t < n->tab_count; ++t )
            if ( n->tabs[ t ] == win )
                return n;
    }
    return NULL;
}

/*==============================================================================================
    Hidden panes -- a leaf whose windows all stopped emitting collapses out of the layout.

    Tab membership is deliberately retained when a window stops being emitted (menu toggle, X
    close, conditional emission): the tree must revive intact when the window comes back.  But a
    leaf whose EVERY tab went silent should not keep reserving screen space as a hole -- its
    sibling absorbs the extent instead (dock_node_layout below), with structure and ratio frozen
    so re-emission restores the pane exactly.  The state is refreshed once per build at ctx_end
    (dock_hidden_refresh), after all windows have begun, from the window records' last_frame
    stamps; a transition forces a redraw so the collapsed tiling lands next frame.
==============================================================================================*/

/* True when tab window `wid` was begun this frame or the one before.  "Or the one before" keeps
   the state stable for a host that closes and re-opens the same context mid-frame (the second
   build's windows have not begun yet at the first ctx_end); the cost is one extra frame of
   latency on a hide.  A tab with no window record yet (layout built or loaded before the window's
   first begin) counts as seen -- its pane stays reserved until the window shows up. */
static bool
dock_tab_seen( gui_id_t wid )
{
    const gui_window_t* w = window_find( wid );
    return !w || w->last_frame + 1u >= gui_frame_index();
}

/* Post-order refresh of one subtree's hidden flags; returns the subtree's own state (a split is
   hidden only when BOTH children are).  An empty leaf (tab_count 0) is never hidden -- a bare
   pane awaiting a dock_window keeps its placeholder.  Any change requests a redraw so the next
   build re-tiles. */
static bool
dock_hidden_refresh_node( gui_dock_node_t* n )
{
    if ( !n )
        return false;

    bool hidden;
    if ( n->split == GUI_DOCK_SPLIT_NONE )
    {
        hidden = ( n->tab_count > 0 );
        u32 first_seen = n->tab_count;
        for ( u32 i = 0; i < n->tab_count; ++i )
            if ( dock_tab_seen( n->tabs[ i ] ) )
            {
                hidden = false;
                if ( first_seen == n->tab_count )
                    first_seen = i;
            }

        /* The active tab must be a window that renders: when the selected window hides while
           visible siblings remain, the first seen tab takes the pane over -- otherwise the node
           shows an empty body with every live tab buried inactive behind it. */
        if ( !hidden && n->tab_count > 0 && n->active_tab < n->tab_count
             && !dock_tab_seen( n->tabs[ n->active_tab ] ) )
        {
            n->active_tab                = first_seen;
            redraw_request();
        }
    }
    else
    {
        bool h0 = dock_hidden_refresh_node( dock_at( n->child[ 0 ] ) );
        bool h1 = dock_hidden_refresh_node( dock_at( n->child[ 1 ] ) );
        hidden  = h0 && h1;
    }

    if ( hidden != n->hidden )
    {
        n->hidden                    = hidden;
        redraw_request();
    }
    return hidden;
}

/* Refresh every viewport tree of the bound context.  Called from gui_ctx_end while the closing
   context is still bound -- the one point where every window's begin has run for this build.
   Floating groups are not reachable from any dock_root and are never marked. */
void
dock_hidden_refresh( void )
{
    if ( !g_ctx->dock.pool )
        return;
    for ( u32 vp = 0; vp < s_vp_count; ++vp )
        dock_hidden_refresh_node( dock_at( s_vp_pool[ vp ].dock_root ) );
}

/* The hidden pane a `dir` drop beside `n` should REVIVE instead of carving a new split: when the
   space a drop would take is exactly where a hidden pane already sits, the window tabs into that
   pane (and the hidden window, on re-emission, comes back as a sibling tab there).  `n` is the
   drop target -- the leaf under the cursor for an inner chip, the tree root for an edge chip.
   Matches one level: a split on the drop axis with `n` on the far side and a hidden subtree on
   the drop side (or, for the root itself, its own drop-side child).  Descends the hidden subtree
   toward the shared edge to a leaf.  NULL when there is nothing hidden there (or the leaf's tab
   list is full) -- the caller falls back to a real split. */
static gui_dock_node_t*
dock_hidden_reuse( gui_dock_node_t* n, gui_dir_t dir )
{
    u8  axis = ( dir == GUI_DIR_LEFT || dir == GUI_DIR_RIGHT ) ? GUI_DOCK_SPLIT_X : GUI_DOCK_SPLIT_Y;
    u32 side = ( dir == GUI_DIR_LEFT || dir == GUI_DIR_UP ) ? 0u : 1u;

    gui_dock_node_t* sib = NULL;
    if ( n->split == axis )   /* edge drop on the root: its own drop-side child may be hidden */
        sib = dock_at( n->child[ side ] );
    else
    {
        gui_dock_node_t* p = dock_at( n->parent );
        if ( p && p->split == axis && p->child[ side ^ 1u ] == dock_ref( n ) )
            sib = dock_at( p->child[ side ] );
    }
    if ( !sib || !sib->hidden )
        return NULL;

    while ( sib && sib->split != GUI_DOCK_SPLIT_NONE )   /* to the leaf nearest the shared edge */
        sib = dock_at( sib->child[ side ^ 1u ] );
    return ( sib && sib->tab_count < GUI_DOCK_TABS_MAX ) ? sib : NULL;
}

/*==============================================================================================
    Maximize over the dockspace

    One leaf of a viewport's tree may be pinned over the WHOLE dock area (the docked twin of the
    floater maximize): its rect eases between the tree slot and the full dockspace through the
    same timed tween the floater states use, and while the cover is settled every other tree
    node's windows suppress -- they are fully obscured, so emitting them would be pure waste
    (window_route_resolve applies the inactive-tab semantics; the state lives on the viewport,
    see dock_max_* in core/gui_ctx.h).  The per-frame tween step runs in dockspace_over_viewport
    (gui_dock.c), after the tree layout resolves the restore target.
==============================================================================================*/

/* The viewport's maximized leaf, validated -- or NULL, clearing any stale state (the node was
   emptied and collapsed, the tree was cleared / reloaded from a blob, or every window of the leaf
   stopped emitting -- a settled cover that draws nothing would otherwise suppress the whole
   dockspace).  Ids are the stable handle (never reused within a session), so a freed slot can
   never false-match.  A node that outlives its maximize (split, hidden) drops the raised cover z
   here, so it re-tiles as a plain node when it next lays out. */
static gui_dock_node_t*
dock_max_node( gui_vp_t vp )
{
    gui_viewport_t* v = &s_vp_pool[ vp ];
    if ( v->dock_max_id == 0 )
        return NULL;

    gui_dock_node_t* n = dock_node_find( v->dock_max_id );
    if ( !n || n->split != GUI_DOCK_SPLIT_NONE || n->floating || n->viewport != vp
         || n->hidden )
    {
        if ( n && !n->floating )
            n->z = 0;   /* back to the tile floor -- the settle path only resets a LIVE maximize */
        v->dock_max_id      = 0;
        v->dock_max_on      = false;
        v->dock_max_settled = false;
        return NULL;
    }
    return n;
}

/* Toggle leaf n's maximize-over-dockspace.  The single choke point the strip button, the strip
   double-click, and the programmatic verb (gui_dock_window_maximize) all route through -- like
   the floater's window_maximize_set.  Captures the current rect as the tween's FROM and arms the
   shared window-anim clock (zero duration when animation is off, so the next step snaps);
   entering also raises the node's z so the cover paints over the sibling tiles while it grows
   (and over the free floaters while fullscreen -- occlusion follows from z, floater-style).
   The tween itself runs in dockspace_over_viewport; leaving eases back toward the tree rect the
   layout re-resolves every frame, and the settle drops z back to the tile floor. */
static void
dock_max_set( gui_dock_node_t* n, bool on )
{
    gui_viewport_t* v = &s_vp_pool[ n->viewport ];

    if ( on )
    {
        /* Take over from any other maxed node: it snaps back to a plain tile, so the cover z it
           held must drop with it (the settle path only ever resets the CURRENT dock_max_id). */
        if ( v->dock_max_id != 0 && v->dock_max_id != n->id )
        {
            gui_dock_node_t* prev = dock_node_find( v->dock_max_id );
            if ( prev )
                prev->z = 0;
        }
        v->dock_max_id = n->id;
        n->z           = surface_z_raise( n->z );
    }
    v->dock_max_on      = on;
    v->dock_max_settled = false;
    v->dock_max_from    = n->rect;
    gui_anim_start( id_combine( n->id, DOCK_MAX_SALT ), s_win_anim ? FEAT_ANIM_SECS : 0.0f );
    redraw_request();   /* takes effect next frame; force one more build */
}

/*==============================================================================================
    Leaf tab edits + tree collapse
==============================================================================================*/

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
    if ( n->active_tab > idx )
        n->active_tab--;   /* slots below shifted down -- keep the SAME window selected */
    else if ( n->active_tab >= n->tab_count )
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
        s_vp_pool[ parent->viewport ].dock_root = dock_ref( sib );
    else
        gp->child[ gp->child[ 0 ] == dock_ref( parent ) ? 0 : 1 ] = dock_ref( sib );

    dock_node_free( leaf );
    dock_node_free( parent );
}

/* Remove window `wid`'s tab from `n` (if present) and collapse `n` should that empty it -- the
   shared undock step every removal path (gui_dock_window's re-dock, gui_dock_undock,
   dock_undock_by_id) performs identically before doing anything path-specific of its own.

   A FLOATING group (gui_dock_float.c) has no tree to collapse; instead it dissolves once a single
   tab remains -- a group of one is just a window, so the survivor inherits the group's frame
   (rect and z) and returns to free-floating; the node is freed either way. */
static void
dock_node_remove_window( gui_dock_node_t* n, gui_id_t wid )
{
    for ( u32 i = 0; i < n->tab_count; ++i )
        if ( n->tabs[ i ] == wid ) { dock_leaf_remove_tab( n, i ); break; }

    if ( n->floating )
    {
        if ( n->tab_count <= 1 )
        {
            if ( n->tab_count == 1 )
            {
                gui_window_t* w = window_find( n->tabs[ 0 ] );
                if ( w )
                {
                    w->viewport = n->viewport;
                    w->x = n->rect.x;  w->y = n->rect.y;
                    w->w = n->rect.w;  w->h = n->rect.h;
                    w->z = n->z;   /* keep the group's place in the stacking order */
                }
            }
            dock_node_free( n );
        }
        return;
    }

    if ( n->tab_count == 0 )
        dock_collapse( n );
}

/* Tab window `wid` (display `name`) into leaf `n`: pull it out of any node it was already in,
   append it, and make it the active tab.  The shared leaf edit behind both docking entry points
   -- gui_dock_window (title-string path, gui_dock.c) and the floating-group id path
   (gui_dock_float.c: group create, drop service, gui_window_tab).  No-op if `n` is full or the
   window already tabs here. */
static void
dock_leaf_tab_add( gui_dock_node_t* n, gui_id_t wid, const char* name )
{
    if ( n->tab_count >= GUI_DOCK_TABS_MAX )
        return;
    gui_dock_node_t* prev = dock_find_window_node( wid );
    if ( prev == n )
        return;   /* already here */
    if ( prev )
        dock_node_remove_window( prev, wid );

    u32 idx = n->tab_count++;
    n->tabs[ idx ] = wid;
    u32 vis = label_vis_len( name );
    if ( vis >= GUI_DOCK_NAME_CAP ) vis = GUI_DOCK_NAME_CAP - 1;
    memcpy( n->names[ idx ], name, vis );
    n->names[ idx ][ vis ] = '\0';
    n->active_tab = idx;
}

/*==============================================================================================
    Layout -- assign every node a rect, top-down from the surface area.

    A leaf reserves a WIN_TITLE_H tab strip off its top; the remainder is `content` (where the active
    window's body draws).  A split divides its rect by `ratio` (child[0]'s fraction of the axis),
    reserving the DOCK_SPLITTER gutter between the two children.  Resolved fresh every frame so an OS
    resize or a splitter drag re-tiles immediately.
==============================================================================================*/

/* Carve a leaf's tab strip off the top of its rect: content = rect minus a WIN_TITLE_H band,
   clamped so a sliver-thin node keeps a sane (possibly zero-height) body.  The one place the
   strip/body division is defined -- every path that moves a leaf rect outside the tree layout
   (maximize tween, floating-group drag/resize) re-carves through here. */
static void
dock_leaf_carve_content( gui_dock_node_t* n )
{
    f32 th = WIN_TITLE_H;
    if ( th > n->rect.h ) th = n->rect.h;
    n->content = ( gui_rect_t ){ n->rect.x, n->rect.y + th, n->rect.w, n->rect.h - th };
}

static void
dock_node_layout( gui_dock_node_t* n, gui_rect_t r )
{
    if ( !n )   /* defensive only -- load heals missing split children (dock_parse_node), and the
                   runtime edits never produce a one-armed split */
        return;
    n->rect = r;

    if ( n->split == GUI_DOCK_SPLIT_NONE )
    {
        dock_leaf_carve_content( n );
        return;
    }

    /* One child hidden (all its windows stopped emitting): the visible sibling absorbs the whole
       rect, no gutter; the hidden child lays out as a zero-extent slice pinned at its own edge so
       its descendants keep sane rects.  The ratio is NOT touched -- it encodes the pane's revival
       extent.  Both-hidden falls through to the normal split: the parent already collapsed this
       whole subtree (or the entire tree is hidden and lays out inert behind nothing drawn). */
    {
        gui_dock_node_t* c0 = dock_at( n->child[ 0 ] );
        gui_dock_node_t* c1 = dock_at( n->child[ 1 ] );
        bool h0 = c0 && c0->hidden;
        bool h1 = c1 && c1->hidden;
        if ( h0 != h1 )
        {
            bool x = ( n->split == GUI_DOCK_SPLIT_X );
            gui_rect_t e0 = x ? ( gui_rect_t ){ r.x,       r.y,       0.0f, r.h  }
                              : ( gui_rect_t ){ r.x,       r.y,       r.w,  0.0f };
            gui_rect_t e1 = x ? ( gui_rect_t ){ r.x + r.w, r.y,       0.0f, r.h  }
                              : ( gui_rect_t ){ r.x,       r.y + r.h, r.w,  0.0f };
            dock_node_layout( c0, h0 ? e0 : r );
            dock_node_layout( c1, h1 ? e1 : r );
            return;
        }
    }

    f32 thick = DOCK_SPLITTER;
    if ( n->split == GUI_DOCK_SPLIT_X )
    {
        f32 avail = r.w - thick; if ( avail < 0.0f ) avail = 0.0f;
        f32 w0    = floorf( avail * n->ratio );
        dock_node_layout( dock_at( n->child[ 0 ] ), ( gui_rect_t ){ r.x,             r.y, w0,               r.h } );
        dock_node_layout( dock_at( n->child[ 1 ] ), ( gui_rect_t ){ r.x + w0 + thick, r.y, r.w - w0 - thick, r.h } );
    }
    else /* GUI_DOCK_SPLIT_Y */
    {
        f32 avail = r.h - thick; if ( avail < 0.0f ) avail = 0.0f;
        f32 h0    = floorf( avail * n->ratio );
        dock_node_layout( dock_at( n->child[ 0 ] ), ( gui_rect_t ){ r.x, r.y,             r.w, h0               } );
        dock_node_layout( dock_at( n->child[ 1 ] ), ( gui_rect_t ){ r.x, r.y + h0 + thick, r.w, r.h - h0 - thick } );
    }
}

/*==============================================================================================
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
==============================================================================================*/

/* Node extent in pixels along a split axis (this frame's resolved rect). */
static f32
dock_axis_ext( const gui_dock_node_t* n, u8 axis )
{
    return ( axis == GUI_DOCK_SPLIT_X ) ? n->rect.w : n->rect.h;
}

/* How many pixels subtree n can give up along `axis` before a pane on its `side` near chain hits
   DOCK_MIN_PANE -- the drag clamp, so a deep pane never collapses to nothing. */
static f32
dock_shrink_capacity( gui_dock_node_t* n, u8 axis, u32 side )
{
    if ( !n ) return 0.0f;
    if ( n->hidden ) return 1.0e9f;   /* collapsed to zero extent -- poses no MIN_PANE constraint */
    if ( n->split == GUI_DOCK_SPLIT_NONE )
    {
        f32 c = dock_axis_ext( n, axis ) - DOCK_MIN_PANE;
        return c > 0.0f ? c : 0.0f;
    }
    if ( n->split == axis )   /* only the near child shrinks; the far one keeps its pixels */
    {
        gui_dock_node_t* c = dock_at( n->child[ side ] );
        if ( c && c->hidden )   /* collapsed near child: the chain continues in the visible one */
            c = dock_at( n->child[ side ^ 1u ] );
        return dock_shrink_capacity( c, axis, side );
    }
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
    if ( !n || n->split == GUI_DOCK_SPLIT_NONE || delta == 0.0f || n->hidden )
        return;   /* a hidden subtree's ratios are frozen -- they encode its revival extents */
    if ( n->split != axis )
    {
        dock_absorb_delta( dock_at( n->child[ 0 ] ), axis, side, delta );
        dock_absorb_delta( dock_at( n->child[ 1 ] ), axis, side, delta );
        return;
    }

    /* Same-axis split with a collapsed child: this node's ratio also stays frozen (it is the
       hidden pane's revival share); the visible child spans the whole extent and absorbs alone. */
    {
        gui_dock_node_t* c0 = dock_at( n->child[ 0 ] );
        gui_dock_node_t* c1 = dock_at( n->child[ 1 ] );
        if ( ( c0 && c0->hidden ) || ( c1 && c1->hidden ) )
        {
            dock_absorb_delta( ( c0 && c0->hidden ) ? c1 : c0, axis, side, delta );
            return;
        }
    }

    f32 avail_new = dock_axis_ext( n, axis ) - DOCK_SPLITTER + delta;
    if ( avail_new < 1.0f ) avail_new = 1.0f;

    /* The far child keeps its pixels; child[0]'s new extent follows from which side is far. */
    f32 far_px = dock_axis_ext( dock_at( n->child[ side ^ 1u ] ), axis );
    f32 c0_new = ( side == 1u ) ? far_px : ( avail_new - far_px );
    n->ratio   = clampf( c0_new / avail_new, 0.02f, 0.98f );

    dock_absorb_delta( dock_at( n->child[ side ] ), axis, side, delta );
}

/*==============================================================================================
    Splitter interaction + draw (one internal node)

    The gutter sits between the children and over no window, so hover_win is NONE there unless a
    floating window covers it -- gating the grab on hover_win == NONE thus naturally yields to a
    floater drawn on top.  Grab sets active_id (released globally when the button lifts, like a window
    drag); while held, the gutter tracks the cursor as a pixel DELTA absorbed by the adjacent panes
    (see dock_absorb_delta above), clamped so no pane on either near chain shrinks below DOCK_MIN_PANE.
==============================================================================================*/

static void
dock_splitter( gui_dock_node_t* n, u32 vp )
{
    gui_rect_t r     = n->rect;
    f32          thick = DOCK_SPLITTER;
    gui_rect_t sr;
    if ( n->split == GUI_DOCK_SPLIT_X )
        sr = ( gui_rect_t ){ dock_at( n->child[ 1 ] )->rect.x - thick, r.y, thick, r.h };
    else
        sr = ( gui_rect_t ){ r.x, dock_at( n->child[ 1 ] )->rect.y - thick, r.w, thick };

    /* Bare grab through item_grab (core/gui_item.c); the hover-domain gate is this
       splitter's own: the gutter sits over no window on its viewport (see the section note). */
    gui_id_t sid    = id_combine( n->id, DOCK_SPLIT_SALT );
    bool       active = false;
    bool       gate   = ( s_io.mouse_viewport == vp && s_interaction.hover_win == GUI_ID_NONE );
    bool       hot    = item_grab( sid, sr, gate, &active );

    if ( active )
    {
        hot = true;
        u8  axis = n->split;
        gui_dock_node_t* c0 = dock_at( n->child[ 0 ] );
        gui_dock_node_t* c1 = dock_at( n->child[ 1 ] );

        f32 avail = ( ( axis == GUI_DOCK_SPLIT_X ) ? r.w : r.h ) - thick;
        if ( avail < 1.0f ) avail = 1.0f;

        /* Cursor-desired child[0] extent -> a pixel delta against this frame's resolved rect,
           clamped so no pane on either side's near chain shrinks below DOCK_MIN_PANE. */
        f32 old0  = dock_axis_ext( c0, axis );
        f32 want0 = ( axis == GUI_DOCK_SPLIT_X ) ? ( s_io.mouse_x - r.x ) : ( s_io.mouse_y - r.y );
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
        cursor_set( ( n->split == GUI_DOCK_SPLIT_X ) ? APP_CURSOR_RESIZE_EW : APP_CURSOR_RESIZE_NS );

    draw_push_rect_filled( sr.x, sr.y, sr.w, sr.h, 0, 0, 1, 1, 0, ( hot || active ) ? COL_BORDER_HOT : COL_BORDER_IDLE );
}

/* Post-order walk: lay splitters of the children before this node's own, so a parent gutter paints
   over the child borders it abuts. */
static void
dock_tree_splitters( gui_dock_node_t* n, u32 vp )
{
    if ( !n || n->split == GUI_DOCK_SPLIT_NONE || n->hidden )
        return;
    dock_tree_splitters( dock_at( n->child[ 0 ] ), vp );
    dock_tree_splitters( dock_at( n->child[ 1 ] ), vp );

    /* A gutter beside a collapsed (hidden) child does not exist this frame: the visible sibling
       spans the whole rect and there is nothing to drag between. */
    gui_dock_node_t* c0 = dock_at( n->child[ 0 ] );
    gui_dock_node_t* c1 = dock_at( n->child[ 1 ] );
    if ( ( c0 && c0->hidden ) || ( c1 && c1->hidden ) )
        return;
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
    if ( n->split == GUI_DOCK_SPLIT_NONE )
    {
        if ( n->tab_count == 0 )
        {
            draw_set_rounding( 0.0f );   /* empty node tiles flush in the dock grid -- keep it square */
            draw_push_rect_filled ( n->rect.x, n->rect.y, n->rect.w, n->rect.h, 0, 0, 1, 1, 0, COL_PANEL_DIM );
            draw_push_rect_outline( n->rect.x, n->rect.y, n->rect.w, n->rect.h, WIN_BORDER, COL_BORDER_IDLE );
        }
        return;
    }
    dock_tree_placeholders( dock_at( n->child[ 0 ] ) );
    dock_tree_placeholders( dock_at( n->child[ 1 ] ) );
}

// clang-format on
/*============================================================================================*/
