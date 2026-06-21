/*==============================================================================================

    runtime_service/imgui/imgui_dock.c -- Docking: the dock-node tree behind viewport.dock_root.

    Makes the dock_root seam (imgui_internal.h / imgui_render.c) live.  A viewport's dock_root is a
    tree of imgui_dock_node_t: LEAF nodes tab one or more windows into a region, INTERNAL nodes split
    their rect between two children at a draggable ratio.  This file owns the node pool, the per-frame
    layout (rect assignment from the surface extent down), the splitter interaction, the tab-strip
    chrome a docked window draws in place of a title bar, and the programmatic build API
    (dockspace_over_viewport / dock_split / dock_window / dock_undock / is_window_docked).

    Phase 1 is programmatic only: a host builds a layout in code and the windows tile + tab into it.
    Mouse drag-to-dock (the 5-way preview overlay) and layout persistence are later phases.

    How a docked window renders: window_begin_ex (imgui_widget_window.c) detects a docked window via
    dock_find_window_node and routes it through window_begin_docked -- the window's geometry is pinned
    to its node, its title bar suppressed, and only the node's ACTIVE tab opens a body.  end_window
    then calls dock_window_chrome here to draw the shared tab strip + node border.  So this file owns
    dock placement + chrome while the window body machinery (regions, scroll, widgets) is reused whole.

    Included by imgui.c AFTER imgui_widget_window.c (so window_begin_docked / end_window can forward to
    dock_find_window_node + dock_window_chrome, declared in imgui_internal.h) and before imgui_popup.c.
    Reaches the node pool through the g_ctx aliases (s_dock_nodes / s_dock_node_count / s_dock_id_seq).

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

/* Salts: a splitter and each tab get a stable per-node widget id distinct from the windows + chrome. */
#define DOCK_SPLIT_SALT 0xD0C5B17u
#define DOCK_TAB_SALT   0xD0C7AB00u

/*----------------------------------------------------------------------------------------------
    Node pool

    Fixed per-context array so child / parent pointers stay valid across frames (no compaction).  A
    free slot has id == 0; alloc reuses the first freed hole or appends, and never returns id 0.
----------------------------------------------------------------------------------------------*/

static imgui_dock_node_t*
dock_node_alloc( u32 viewport )
{
    if ( !s_dock_nodes ) return NULL;   /* docking disabled for this context */
    imgui_dock_node_t* n = NULL;
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
dock_node_free( imgui_dock_node_t* n )
{
    if ( n ) memset( n, 0, sizeof *n );
}

static imgui_dock_node_t*
dock_node_find( imgui_dock_id_t id )
{
    if ( !id ) return NULL;
    for ( u32 i = 0; i < s_dock_node_count; ++i )
        if ( s_dock_nodes[ i ].id == id )
            return &s_dock_nodes[ i ];
    return NULL;
}

/* The lookup begin_window routes through: which LEAF tabs this window, or NULL.  Forward-declared in
   imgui_internal.h so imgui_widget_window.c (included earlier) can call it. */
static imgui_dock_node_t*
dock_find_window_node( imgui_id_t win )
{
    if ( !win ) return NULL;
    for ( u32 i = 0; i < s_dock_node_count; ++i )
    {
        imgui_dock_node_t* n = &s_dock_nodes[ i ];
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
dock_leaf_remove_tab( imgui_dock_node_t* n, u32 idx )
{
    for ( u32 i = idx; i + 1 < n->tab_count; ++i )
    {
        n->tabs[ i ] = n->tabs[ i + 1 ];
        memcpy( n->names[ i ], n->names[ i + 1 ], IMGUI_DOCK_NAME_CAP );
    }
    n->tab_count--;
    if ( n->active_tab >= n->tab_count )
        n->active_tab = n->tab_count ? n->tab_count - 1 : 0;
}

/* A leaf that lost its last tab is removed and its parent split replaced by the surviving sibling --
   the sibling takes the parent's slot in the grandparent (or becomes the new dock_root).  A root leaf
   that empties is left in place: a bare dockspace is valid (nothing tiled yet). */
static void
dock_collapse( imgui_dock_node_t* leaf )
{
    imgui_dock_node_t* parent = leaf->parent;
    if ( !parent )
        return;   /* root leaf emptied -- keep the bare dockspace */

    imgui_dock_node_t* sib = ( parent->child[ 0 ] == leaf ) ? parent->child[ 1 ] : parent->child[ 0 ];
    imgui_dock_node_t* gp  = parent->parent;

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
dock_node_layout( imgui_dock_node_t* n, imgui_rect_t r )
{
    if ( !n )   /* defensive: a corrupt/truncated loaded blob could leave a split child NULL */
        return;
    n->rect = r;

    if ( n->split == DOCK_SPLIT_NONE )
    {
        f32 th = WIN_TITLE_H;
        if ( th > r.h ) th = r.h;
        n->content = ( imgui_rect_t ){ r.x, r.y + th, r.w, r.h - th };
        return;
    }

    f32 thick = DOCK_SPLITTER;
    if ( n->split == DOCK_SPLIT_X )
    {
        f32 avail = r.w - thick; if ( avail < 0.0f ) avail = 0.0f;
        f32 w0    = floorf( avail * n->ratio );
        dock_node_layout( n->child[ 0 ], ( imgui_rect_t ){ r.x,             r.y, w0,               r.h } );
        dock_node_layout( n->child[ 1 ], ( imgui_rect_t ){ r.x + w0 + thick, r.y, r.w - w0 - thick, r.h } );
    }
    else /* DOCK_SPLIT_Y */
    {
        f32 avail = r.h - thick; if ( avail < 0.0f ) avail = 0.0f;
        f32 h0    = floorf( avail * n->ratio );
        dock_node_layout( n->child[ 0 ], ( imgui_rect_t ){ r.x, r.y,             r.w, h0               } );
        dock_node_layout( n->child[ 1 ], ( imgui_rect_t ){ r.x, r.y + h0 + thick, r.w, r.h - h0 - thick } );
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
dock_splitter( imgui_dock_node_t* n, u32 vp )
{
    imgui_rect_t r     = n->rect;
    f32          thick = DOCK_SPLITTER;
    imgui_rect_t sr;
    if ( n->split == DOCK_SPLIT_X )
        sr = ( imgui_rect_t ){ n->child[ 1 ]->rect.x - thick, r.y, thick, r.h };
    else
        sr = ( imgui_rect_t ){ r.x, n->child[ 1 ]->rect.y - thick, r.w, thick };

    imgui_id_t sid    = id_combine( n->id, DOCK_SPLIT_SALT );
    bool       active = ( s_interaction.active_id == sid );
    bool       hot    = false;

    if ( s_io.mouse_viewport == vp && s_interaction.active_id == IMGUI_ID_NONE
         && s_interaction.hover_win == IMGUI_ID_NONE && rect_hit( sr ) )
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

    draw_push_rect_filled( sr.x, sr.y, sr.w, sr.h, 0, 0, 1, 1, 0, ( hot || active ) ? COL_RESIZE_HOT : COL_BORDER );
}

/* Post-order walk: lay splitters of the children before this node's own, so a parent gutter paints
   over the child borders it abuts. */
static void
dock_tree_splitters( imgui_dock_node_t* n, u32 vp )
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
dock_tree_placeholders( imgui_dock_node_t* n )
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

/*----------------------------------------------------------------------------------------------
    Drag-to-dock + undock-by-tab-drag (Phase 2 mouse gestures)

    Drag-to-dock: while a FREE window is title-dragged over a dockspace on the same surface,
    dock_drag_detect (called from window_begin_ex) finds the leaf under the cursor, draws a per-node
    5-way chip overlay + a translucent preview of the region the window would take, and records the
    chip the cursor is over.  On the release edge, dock_drag_commit (from end_window) tabs the window
    into the leaf (center) or splits the leaf and docks it on a side -- reusing the Phase-1 tree edits.
    The overlay paints on a reserved z-band above everything (popups sit at 0x80000000).

    Undock-by-tab-drag: a press on a tab pending past TITLEBAR_DRAG_THRESH pops that window out of its
    node into a free window that follows the cursor -- handled inside dock_window_chrome below, reusing
    the window-drag statics (s_drag_off_*, s_z_counter) the tear-off threshold already drives.
----------------------------------------------------------------------------------------------*/

#define DOCK_TAB_DRAG_THRESH 12.0f                      /* px a tab must move before it undocks     */
#define DOCK_OVERLAY_Z     0xF0000000u                  /* above the popup z-band (0x80000000)     */
#define DOCK_OVERLAY_FILL  IMGUI_COLOR( 90, 160, 245,  64 )   /* translucent drop-region preview   */
#define DOCK_OVERLAY_LINE  IMGUI_COLOR( 90, 160, 245, 200 )   /* its outline                       */

/* Drop zones of the per-node 5-way: the chip the cursor is over (NONE = over the node but no chip). */
typedef enum
{
    DOCK_ZONE_NONE = -1,
    DOCK_ZONE_CENTER = 0,   /* tab into the leaf            */
    DOCK_ZONE_LEFT,         /* split the leaf, new on left  */
    DOCK_ZONE_RIGHT,
    DOCK_ZONE_TOP,
    DOCK_ZONE_BOTTOM,

} dock_zone_t;

/* Drag-to-dock: computed each frame the dragged window is over a dockspace, consumed on release.
   One slot -- a single window owns active_id. */
static struct
{
    bool            active;     /* a chip is hovered this frame (a valid drop)  */
    imgui_id_t      win_id;     /* the dragged window                          */
    u32             viewport;   /* dockspace surface under the cursor          */
    imgui_dock_id_t target;     /* leaf node the cursor is over                */
    i32             zone;       /* dock_zone_t                                 */

} s_dock_drag;

/* Undock-by-tab-drag: a tab press pending the move threshold (mirrors s_titlebar_drag_*). */
static struct
{
    bool       pending;
    imgui_id_t win_id;          /* window whose tab is held */
    f32        px, py;          /* press position           */

} s_dock_tab_drag;

/* The leaf whose rect contains (mx,my), or NULL.  Descends the tree following the point. */
static imgui_dock_node_t*
dock_leaf_at( imgui_dock_node_t* n, f32 mx, f32 my )
{
    if ( !n || !imgui_rect_contains( n->rect, mx, my ) )
        return NULL;
    if ( n->split == DOCK_SPLIT_NONE )
        return n;
    imgui_dock_node_t* c = dock_leaf_at( n->child[ 0 ], mx, my );
    return c ? c : dock_leaf_at( n->child[ 1 ], mx, my );
}

/* Map a side zone to the split direction the new window docks toward. */
static imgui_dir_t
dock_zone_dir( dock_zone_t z )
{
    switch ( z )
    {
        case DOCK_ZONE_LEFT:   return IMGUI_DIR_LEFT;
        case DOCK_ZONE_RIGHT:  return IMGUI_DIR_RIGHT;
        case DOCK_ZONE_TOP:    return IMGUI_DIR_UP;
        case DOCK_ZONE_BOTTOM: return IMGUI_DIR_DOWN;
        default:               return IMGUI_DIR_LEFT;
    }
}

/* The chip square for one zone, centered in the leaf: center plus the four arrows at a gap around it. */
static imgui_rect_t
dock_chip_rect( imgui_rect_t leaf, dock_zone_t z, f32 s, f32 g )
{
    imgui_vec2_t c = imgui_rect_center( leaf );
    f32 x = c.x - s * 0.5f, y = c.y - s * 0.5f;
    switch ( z )
    {
        case DOCK_ZONE_LEFT:   return ( imgui_rect_t ){ x - g - s, y, s, s };
        case DOCK_ZONE_RIGHT:  return ( imgui_rect_t ){ x + g + s, y, s, s };
        case DOCK_ZONE_TOP:    return ( imgui_rect_t ){ x, y - g - s, s, s };
        case DOCK_ZONE_BOTTOM: return ( imgui_rect_t ){ x, y + g + s, s, s };
        case DOCK_ZONE_CENTER:
        default:               return ( imgui_rect_t ){ x, y, s, s };
    }
}

/* The half (or whole) of a node rect the window would occupy if dropped on zone -- the preview fill. */
static imgui_rect_t
dock_zone_region( imgui_rect_t r, dock_zone_t z )
{
    switch ( z )
    {
        case DOCK_ZONE_LEFT:   return ( imgui_rect_t ){ r.x,                r.y,                r.w * 0.5f, r.h        };
        case DOCK_ZONE_RIGHT:  return ( imgui_rect_t ){ r.x + r.w * 0.5f,   r.y,                r.w * 0.5f, r.h        };
        case DOCK_ZONE_TOP:    return ( imgui_rect_t ){ r.x,                r.y,                r.w,        r.h * 0.5f };
        case DOCK_ZONE_BOTTOM: return ( imgui_rect_t ){ r.x,                r.y + r.h * 0.5f,   r.w,        r.h * 0.5f };
        case DOCK_ZONE_CENTER:
        default:               return r;
    }
}

/* Compute + preview the drop target for a free window being dragged over a dockspace on its own
   surface.  Sets s_dock_drag (active only when the cursor is over a chip) and draws the overlay on
   the reserved high z-band.  Forward-declared in imgui_internal.h for window_begin_ex to call. */
static void
dock_drag_detect( imgui_id_t win_id, imgui_window_t* win )
{
    s_dock_drag.active = false;
    s_dock_drag.win_id = win_id;
    s_dock_drag.zone   = DOCK_ZONE_NONE;

    u32 vp = win->viewport;
    if ( vp != s_io.mouse_viewport || vp >= g_ctx->max_viewports )
        return;
    imgui_dock_node_t* root = g_ctx->viewports[ vp ].dock_root;
    if ( !root )
        return;
    imgui_dock_node_t* leaf = dock_leaf_at( root, s_io.mouse_x, s_io.mouse_y );
    if ( !leaf )
        return;

    /* Chip layout: a title-bar-sized square per zone, centered in the leaf. */
    f32 s = WIN_TITLE_H * 1.4f;
    f32 g = 6.0f;

    dock_zone_t zone = DOCK_ZONE_NONE;
    for ( i32 z = DOCK_ZONE_CENTER; z <= DOCK_ZONE_BOTTOM; ++z )
        if ( rect_hit( dock_chip_rect( leaf->rect, (dock_zone_t)z, s, g ) ) ) { zone = (dock_zone_t)z; break; }

    s_dock_drag.viewport = vp;
    s_dock_drag.target   = leaf->id;
    s_dock_drag.zone     = zone;
    s_dock_drag.active   = ( zone != DOCK_ZONE_NONE );

    /* Overlay on the reserved high z-band, clipped to the surface. */
    const imgui_viewport_t* v = &g_ctx->viewports[ vp ];
    draw_set_viewport ( vp );
    draw_set_sort_key ( DOCK_OVERLAY_Z );
    draw_set_root_clip( vp_w( v ), vp_h( v ) );

    draw_set_rounding( ROUND_WIDGET );   /* drop preview + chips read as control surfaces */

    if ( zone != DOCK_ZONE_NONE )
    {
        imgui_rect_t hr = dock_zone_region( leaf->rect, zone );
        draw_push_rect_filled ( hr.x, hr.y, hr.w, hr.h, 0, 0, 1, 1, 0, DOCK_OVERLAY_FILL );
        draw_push_rect_outline( hr.x, hr.y, hr.w, hr.h, WIN_BORDER, 0, DOCK_OVERLAY_LINE );
    }

    for ( i32 z = DOCK_ZONE_CENTER; z <= DOCK_ZONE_BOTTOM; ++z )
    {
        imgui_rect_t cr = dock_chip_rect( leaf->rect, (dock_zone_t)z, s, g );
        bool         on = ( (dock_zone_t)z == zone );
        draw_push_rect_filled ( cr.x, cr.y, cr.w, cr.h, 0, 0, 1, 1, 0, on ? COL_WIDGET_HOT : COL_WIDGET_BG );
        draw_push_rect_outline( cr.x, cr.y, cr.w, cr.h, WIN_BORDER, 0, COL_BORDER );
        if ( z == DOCK_ZONE_CENTER )
        {
            f32 ins = cr.w * 0.28f;   /* inner square = the "tab here" glyph */
            draw_set_rounding( 0.0f );   /* small glyph box stays square */
            draw_push_rect_outline( cr.x + ins, cr.y + ins, cr.w - 2.0f * ins, cr.h - 2.0f * ins,
                                    WIN_BORDER, 0, COL_TEXT );
            draw_set_rounding( ROUND_WIDGET );   /* restore for the remaining chips */
        }
        else
        {
            draw_arrow( cr, dock_zone_dir( (dock_zone_t)z ), COL_TEXT );
        }
    }

    draw_set_sort_key ( 0 );
    draw_set_viewport ( 0 );
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );
}

/* Execute the drop computed by dock_drag_detect: tab into the target leaf (center) or split it and
   dock on a side.  Called unconditionally from end_window for every free window; no-ops unless this
   is the dragged window on its release edge (the gate is here because s_dock_drag is unit-private).
   Forward-declared in imgui_internal.h. */
static void
dock_drag_commit( imgui_id_t win_id, const char* title )
{
    if ( !s_dock_drag.active || s_dock_drag.win_id != win_id || !s_io.mouse_released[ 0 ] )
        return;

    if ( s_dock_drag.zone != DOCK_ZONE_NONE && title )
    {
        imgui_dock_node_t* leaf = dock_node_find( s_dock_drag.target );
        if ( leaf && leaf->split == DOCK_SPLIT_NONE )
        {
            if ( s_dock_drag.zone == DOCK_ZONE_CENTER )
            {
                imgui_dock_window( title, leaf->id );
            }
            else
            {
                imgui_dock_id_t side = imgui_dock_split( leaf->id,
                                                         dock_zone_dir( (dock_zone_t)s_dock_drag.zone ),
                                                         0.5f, NULL );
                if ( side != IMGUI_DOCK_NONE )
                    imgui_dock_window( title, side );
            }
        }
    }
    s_dock_drag.active = false;
}

/* Remove a window from its node by id (the undock-by-drag path); collapse a node it empties. */
static void
dock_undock_by_id( imgui_id_t win )
{
    imgui_dock_node_t* n = dock_find_window_node( win );
    if ( !n )
        return;
    for ( u32 i = 0; i < n->tab_count; ++i )
        if ( n->tabs[ i ] == win ) { dock_leaf_remove_tab( n, i ); break; }
    if ( n->tab_count == 0 )
        dock_collapse( n );
}

/*----------------------------------------------------------------------------------------------
    Tab-strip chrome -- drawn by the active window's end_window in place of a title bar.

    Runs while s_build holds the active docked window (its id is hover_win when the cursor is over the
    node, and its clip is the node rect), so widget_behavior hit-tests the tabs correctly.  Tabs march
    left-to-right at natural width; clicking one selects it (takes effect next frame, when that window
    becomes the active tab).  A press dragged past the threshold pops the window out (see below).
    Forward-declared in imgui_internal.h for end_window to call.
----------------------------------------------------------------------------------------------*/

static void
dock_window_chrome( imgui_dock_node_t* node )
{
    f32 x  = s_build.win_x;
    f32 y  = s_build.win_y;
    f32 w  = s_build.win_w;
    f32 th = s_build.win_title_h;   /* tab-strip height (= WIN_TITLE_H, clamped for a tiny node) */

    draw_set_rounding( 0.0f );   /* the strip is a flat band behind the tabs */
    draw_push_rect_filled( x, y, w, th, 0, 0, 1, 1, 0, COL_TITLE_BG );

    f32 tx = x;
    for ( u32 i = 0; i < node->tab_count; ++i )
    {
        const char*  nm = node->names[ i ];
        f32          tw = font_text_w( nm ) + 2.0f * WIDGET_PAD;
        imgui_rect_t tr = { tx, y, tw, th };
        bool         is_active = ( i == node->active_tab );

        imgui_id_t     tid = id_combine( node->id, DOCK_TAB_SALT + i );
        widget_state_t st  = widget_behavior( tid, tr, WIDGET_KIND_BUTTON );

        /* Active tab takes the body colour so it reads as joined to the content below; the rest stay
           on the title band, lifting to the hover colour under the cursor. */
        u32 bg   = is_active ? COL_WIN_BG : ( st.hover ? COL_WIDGET_HOT : COL_TITLE_BG );
        u32 tcol = ( is_active || st.hover ) ? COL_TEXT : COL_TEXT_DIM;
        /* Tabs in a docked node stay square: the active tab takes the body colour to read as joined
           to the content below, and a rounded corner would break that seam. */
        draw_push_rect_filled( tr.x, tr.y, tr.w, tr.h, 0, 0, 1, 1, 0, bg );
        draw_text_fit_n( tr.x + WIDGET_PAD, text_center_y( y, th ), tcol, nm, (u32)strlen( nm ),
                         tw - 2.0f * WIDGET_PAD );

        if ( st.clicked )
            node->active_tab = i;

        /* Press arms an undock-by-drag: the move threshold below decides click (select) vs drag-out. */
        if ( st.pressed )
        {
            s_dock_tab_drag.pending = true;
            s_dock_tab_drag.win_id  = node->tabs[ i ];
            s_dock_tab_drag.px      = s_io.mouse_x;
            s_dock_tab_drag.py      = s_io.mouse_y;
        }

        tx += tw;
    }

    /* Border frames the whole node (strip + body), square so adjacent nodes tile flush at right
       angles.  Drawn before the undock handler so it never reads `node` after a drag-out collapses
       an emptied node. */
    draw_set_rounding( 0.0f );
    draw_push_rect_outline( x, y, w, s_build.win_h, WIN_BORDER, 0, COL_BORDER );

    /* Undock-by-tab-drag: an armed tab press dragged past the threshold pops its window out of the
       node into a free window that follows the cursor.  Reuses the window-drag statics so the free
       drag-apply in window_begin_ex carries the move from next frame.  Released without moving was a
       tab click (st.clicked already selected it). */
    if ( s_dock_tab_drag.pending )
    {
        if ( !s_io.mouse_down[ 0 ] )
        {
            s_dock_tab_drag.pending = false;
        }
        else
        {
            f32 dx = s_io.mouse_x - s_dock_tab_drag.px;
            f32 dy = s_io.mouse_y - s_dock_tab_drag.py;
            if ( dx * dx + dy * dy >= DOCK_TAB_DRAG_THRESH * DOCK_TAB_DRAG_THRESH )
            {
                imgui_id_t wid = s_dock_tab_drag.win_id;
                u32        vp  = node->viewport;   /* capture before a collapse may free `node` */
                s_dock_tab_drag.pending = false;

                dock_undock_by_id( wid );

                imgui_window_t* win = window_find( wid );
                if ( win )
                {
                    win->viewport = vp;              /* float on the surface it was docked on */
                    win->z        = ++s_z_counter;   /* raise above the tiles                 */
                    s_drag_off_x  = WIN_TITLE_H;      /* grab near the title's left edge       */
                    s_drag_off_y  = WIN_TITLE_H * 0.5f;
                    win->x        = s_io.mouse_x - s_drag_off_x;
                    win->y        = s_io.mouse_y - s_drag_off_y;
                    s_interaction.active_id     = wid;  /* continue as a free window drag      */
                    s_interaction.active_button = 0;
                }
            }
        }
    }
}

/*----------------------------------------------------------------------------------------------
    Public API
----------------------------------------------------------------------------------------------*/

/* Ensure viewport vp hosts a dock tree, lay it out over the surface (below any native caption band),
   draw + interact the splitters and empty-leaf placeholders, and return the tree root's id.  Call
   once per frame at the TOP of the build for each dockspace viewport, before the docked windows'
   begin_window run (they read their node's resolved content rect). */
imgui_dock_id_t
imgui_dockspace_over_viewport( imgui_vp_t vp, imgui_dockspace_flags_t flags )
{
    UNUSED( flags );
    if ( !s_dock_nodes ) return IMGUI_DOCK_NONE;   /* docking disabled */
    if ( vp < 0 || vp >= (imgui_vp_t)g_ctx->max_viewports )
        return IMGUI_DOCK_NONE;

    imgui_viewport_t* v = &g_ctx->viewports[ vp ];
    if ( !v->dock_root )
    {
        imgui_dock_node_t* root = dock_node_alloc( (u32)vp );
        if ( !root )
            return IMGUI_DOCK_NONE;
        v->dock_root = root;
    }

    f32 dw  = vp_w( v );
    f32 dh  = vp_h( v );
    f32 top = v->caption_inset;
    imgui_rect_t area = { 0.0f, top, dw, dh - top };
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
   the axis the NEW side receives.  A no-op (returns IMGUI_DOCK_NONE) if node is not a leaf or the pool
   is full. */
imgui_dock_id_t
imgui_dock_split( imgui_dock_id_t node_id, imgui_dir_t dir, f32 ratio, imgui_dock_id_t* out_remain )
{
    if ( out_remain ) *out_remain = node_id;

    imgui_dock_node_t* n = dock_node_find( node_id );
    if ( !n || n->split != DOCK_SPLIT_NONE )
        return IMGUI_DOCK_NONE;

    imgui_dock_node_t* a = dock_node_alloc( n->viewport );
    imgui_dock_node_t* b = dock_node_alloc( n->viewport );
    if ( !a || !b )
    {
        if ( a ) dock_node_free( a );
        if ( b ) dock_node_free( b );
        return IMGUI_DOCK_NONE;
    }

    bool horizontal = ( dir == IMGUI_DIR_LEFT || dir == IMGUI_DIR_RIGHT );
    bool new_first  = ( dir == IMGUI_DIR_LEFT || dir == IMGUI_DIR_UP );   /* new on child[0] side */
    imgui_dock_node_t* new_node = new_first ? a : b;
    imgui_dock_node_t* remain   = new_first ? b : a;

    /* Move the original node's tabs onto the remaining child. */
    remain->tab_count  = n->tab_count;
    remain->active_tab = n->active_tab;
    for ( u32 i = 0; i < n->tab_count; ++i )
    {
        remain->tabs[ i ] = n->tabs[ i ];
        memcpy( remain->names[ i ], n->names[ i ], IMGUI_DOCK_NAME_CAP );
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

/* Add a window (matched to begin_window by id_hash(title)) as a tab in a LEAF node, removing it from
   any node it was previously docked in.  The display name is the title's visible span (any "##" id
   suffix stripped), copied so the tab bar is self-sufficient.  The newly docked window becomes the
   active tab.  No-op if node is not a leaf or its tab list is full. */
void
imgui_dock_window( const char* title, imgui_dock_id_t node_id )
{
    if ( !title )
        return;
    imgui_dock_node_t* n = dock_node_find( node_id );
    if ( !n || n->split != DOCK_SPLIT_NONE || n->tab_count >= IMGUI_DOCK_TABS_MAX )
        return;

    imgui_id_t wid = id_hash( title );

    imgui_dock_node_t* prev = dock_find_window_node( wid );
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
    if ( vis >= IMGUI_DOCK_NAME_CAP ) vis = IMGUI_DOCK_NAME_CAP - 1;
    memcpy( n->names[ idx ], title, vis );
    n->names[ idx ][ vis ] = '\0';
    n->active_tab = idx;
}

/* Remove a window from its node, returning it to free-floating.  A node emptied by this is collapsed
   (its sibling takes its place).  No-op if the window is not docked. */
void
imgui_dock_undock( const char* title )
{
    if ( !title )
        return;
    imgui_id_t wid = id_hash( title );
    imgui_dock_node_t* n = dock_find_window_node( wid );
    if ( !n )
        return;
    for ( u32 i = 0; i < n->tab_count; ++i )
        if ( n->tabs[ i ] == wid ) { dock_leaf_remove_tab( n, i ); break; }
    if ( n->tab_count == 0 )
        dock_collapse( n );
}

bool
imgui_is_window_docked( const char* title )
{
    return title != NULL && dock_find_window_node( id_hash( title ) ) != NULL;
}

/*----------------------------------------------------------------------------------------------
    Layout persistence (Phase 3)

    Serialize a viewport's dock tree to a small ASCII blob and rebuild it later (across a restart
    when the host writes the blob to disk).  The format is a pre-order line stream -- each node
    self-describes and its children follow, so it parses with a one-pass line cursor and no lookahead:

        ORBDOCK 1                 (header + version)
        S <axis> <ratio>          (internal split: axis 0=X/1=Y, then its two children)
        L <active_tab> <count>     (leaf, followed by `count` tab lines)
        T <id_hex> <name>          (one tab: the window id and its display name)

    The window id is stored explicitly (not re-hashed from the name) so a "Title##key" window -- whose
    stored name is the stripped visible span -- restores to the exact id begin_window produces.  Node
    ids are NOT stored: they are runtime handles, freshly assigned on load.  imgui owns only the blob;
    the host owns the file I/O (read at startup, write on change).
----------------------------------------------------------------------------------------------*/

/* Bounds-tracking text appender: writes while within cap but always counts the bytes a full write
   would need, so imgui_dock_save can report the required size like snprintf. */
typedef struct { char* p; u32 cap; u32 len; } dock_writer_t;

static void
dw_emit( dock_writer_t* w, const char* s, u32 n )
{
    for ( u32 i = 0; i < n; ++i )
    {
        if ( w->len + 1u < w->cap )
            w->p[ w->len ] = s[ i ];
        w->len++;
    }
}

static void
dock_serialize_node( dock_writer_t* w, imgui_dock_node_t* n )
{
    char line[ 64 ];
    if ( !n )
        return;
    if ( n->split == DOCK_SPLIT_NONE )
    {
        int k = snprintf( line, sizeof line, "L %u %u\n", n->active_tab, n->tab_count );
        dw_emit( w, line, (u32)k );
        for ( u32 t = 0; t < n->tab_count; ++t )
        {
            k = snprintf( line, sizeof line, "T %08x ", n->tabs[ t ] );
            dw_emit( w, line, (u32)k );
            dw_emit( w, n->names[ t ], (u32)strlen( n->names[ t ] ) );
            dw_emit( w, "\n", 1 );
        }
    }
    else
    {
        int k = snprintf( line, sizeof line, "S %d %.4f\n",
                          ( n->split == DOCK_SPLIT_Y ) ? 1 : 0, n->ratio );
        dw_emit( w, line, (u32)k );
        dock_serialize_node( w, n->child[ 0 ] );
        dock_serialize_node( w, n->child[ 1 ] );
    }
}

/* Serialize viewport vp's dock tree into buf (NUL-terminated, truncated to bufsz).  Returns the byte
   count a full write needs (excluding the NUL), so a caller can size the buffer like snprintf. */
u32
imgui_dock_save( imgui_vp_t vp, char* buf, u32 bufsz )
{
    dock_writer_t w = { buf, bufsz, 0u };
    if ( vp < 0 || vp >= (imgui_vp_t)g_ctx->max_viewports )
    {
        if ( bufsz ) buf[ 0 ] = '\0';
        return 0u;
    }
    dw_emit( &w, "ORBDOCK 1\n", 10u );
    dock_serialize_node( &w, g_ctx->viewports[ vp ].dock_root );
    if ( bufsz )
        buf[ ( w.len < bufsz ) ? w.len : bufsz - 1u ] = '\0';
    return w.len;
}

/* Free every node belonging to viewport vp and clear its dock_root -- the load path's clean slate. */
static void
dock_free_viewport_tree( u32 vp )
{
    for ( u32 i = 0; i < s_dock_node_count; ++i )
        if ( s_dock_nodes[ i ].id != 0 && s_dock_nodes[ i ].viewport == vp )
            dock_node_free( &s_dock_nodes[ i ] );
    g_ctx->viewports[ vp ].dock_root = NULL;
}

/* Line cursor over the blob: copy the next line (sans newline) into out, advance past it; false at
   end of input. */
typedef struct { const char* p; } dock_reader_t;

static bool
dr_line( dock_reader_t* r, char* out, u32 cap )
{
    if ( !*r->p )
        return false;
    u32 i = 0;
    while ( *r->p && *r->p != '\n' )
    {
        if ( i + 1u < cap ) out[ i++ ] = *r->p;
        r->p++;
    }
    if ( *r->p == '\n' ) r->p++;
    out[ i ] = '\0';
    return true;
}

/* Recursively parse one node (and, for a split, its two children) for viewport vp. */
static imgui_dock_node_t*
dock_parse_node( dock_reader_t* r, u32 vp )
{
    char line[ 128 ];
    if ( !dr_line( r, line, sizeof line ) )
        return NULL;

    if ( line[ 0 ] == 'S' )
    {
        int   axis  = 0;
        float ratio = 0.5f;
        if ( sscanf( line + 1, " %d %f", &axis, &ratio ) != 2 )
            return NULL;
        imgui_dock_node_t* n = dock_node_alloc( vp );
        if ( !n )
            return NULL;
        n->split = axis ? DOCK_SPLIT_Y : DOCK_SPLIT_X;
        n->ratio = ratio;
        n->child[ 0 ] = dock_parse_node( r, vp );
        n->child[ 1 ] = dock_parse_node( r, vp );
        if ( n->child[ 0 ] ) n->child[ 0 ]->parent = n;
        if ( n->child[ 1 ] ) n->child[ 1 ]->parent = n;
        return n;
    }

    if ( line[ 0 ] == 'L' )
    {
        unsigned active = 0, count = 0;
        if ( sscanf( line + 1, " %u %u", &active, &count ) != 2 )
            return NULL;
        imgui_dock_node_t* n = dock_node_alloc( vp );
        if ( !n )
            return NULL;
        n->split = DOCK_SPLIT_NONE;
        if ( count > IMGUI_DOCK_TABS_MAX ) count = IMGUI_DOCK_TABS_MAX;
        for ( u32 t = 0; t < count; ++t )
        {
            if ( !dr_line( r, line, sizeof line ) || line[ 0 ] != 'T' )
                break;
            unsigned id = 0;
            int      adv = 0;
            sscanf( line + 1, " %x%n", &id, &adv );
            const char* name = line + 1 + adv;
            if ( *name == ' ' ) name++;            /* the single space before the name */
            u32 ln = (u32)strlen( name );
            if ( ln >= IMGUI_DOCK_NAME_CAP ) ln = IMGUI_DOCK_NAME_CAP - 1u;
            n->tabs[ n->tab_count ] = (imgui_id_t)id;
            memcpy( n->names[ n->tab_count ], name, ln );
            n->names[ n->tab_count ][ ln ] = '\0';
            n->tab_count++;
        }
        n->active_tab = ( active < n->tab_count ) ? active : 0u;
        return n;
    }

    return NULL;
}

/* Replace viewport vp's dock tree with the one encoded in `text` (from a prior imgui_dock_save).
   Returns true if the header is valid.  CAUTION: this frees + rebuilds the tree, so call it at a SAFE
   point -- between frames, or at the top of the build before any docked window is emitted -- never
   from inside a docked window's body (its node would be freed mid-render). */
bool
imgui_dock_load( imgui_vp_t vp, const char* text )
{
    if ( vp < 0 || vp >= (imgui_vp_t)g_ctx->max_viewports || !text )
        return false;

    dock_reader_t r = { text };
    char header[ 32 ];
    if ( !dr_line( &r, header, sizeof header ) || strncmp( header, "ORBDOCK", 7 ) != 0 )
        return false;

    dock_free_viewport_tree( (u32)vp );
    g_ctx->viewports[ vp ].dock_root = dock_parse_node( &r, (u32)vp );
    return true;
}

// clang-format on
/*============================================================================================*/
