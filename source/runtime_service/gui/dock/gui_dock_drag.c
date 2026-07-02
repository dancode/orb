/*==============================================================================================

    runtime_service/gui/dock/gui_dock_drag.c -- Mouse docking gestures: drag-to-dock + tab strip.

    The two mouse gestures layered on top of the programmatic dock tree (gui_dock_core.c /
    gui_dock.c): drag-to-dock (a free window title-dragged over a dockspace previews a per-node
    5-way drop target and commits on release) and undock-by-tab-drag (a press-and-drag on a docked
    tab pops that window back out to free-floating).  Also owns the tab-strip + node-border chrome a
    docked window draws in place of a title bar -- the surface the tab-drag gesture presses on.

    dock_drag_detect / dock_drag_commit are forward-declared in gui_internal.h so window_begin_ex /
    window_end (gui_widget_window.c, included earlier) can call them; dock_window_chrome is
    forward-declared there too, for window_end's docked-chrome path.  Needs gui_dock_core.c's node
    pool + leaf-edit helpers already in scope, so it is included right after that file.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Drag-to-dock + undock-by-tab-drag (Phase 2 mouse gestures)

    Drag-to-dock: while a FREE window is title-dragged over a dockspace on the same surface,
    dock_drag_detect (called from window_begin_ex) finds the leaf under the cursor, draws a per-node
    5-way chip overlay + a translucent preview of the region the window would take, and records the
    chip the cursor is over.  On the release edge, dock_drag_commit (from window_end) tabs the window
    into the leaf (center) or splits the leaf and docks it on a side -- reusing the Phase-1 tree edits.
    The overlay paints on a reserved z-band above everything (popups sit at 0x80000000).

    Undock-by-tab-drag: a press on a tab pending past TITLEBAR_DRAG_THRESH pops that window out of its
    node into a free window that follows the cursor -- handled inside dock_window_chrome below, reusing
    the window-drag statics (s_drag_off_*, s_z_counter) the tear-off threshold already drives.
----------------------------------------------------------------------------------------------*/

/* Tab salt: each tab gets a stable per-node widget id distinct from the windows + splitter (see
   DOCK_SPLIT_SALT in gui_dock_core.c). */
#define DOCK_TAB_SALT   0xD0C7AB00u

#define DOCK_TAB_DRAG_THRESH 12.0f                      /* px a tab must move before it undocks     */
#define DOCK_OVERLAY_Z     0xF0000000u                  /* above the popup z-band (0x80000000)     */
#define DOCK_OVERLAY_FILL  GUI_COLOR( 90, 160, 245,  64 )   /* translucent drop-region preview   */
#define DOCK_OVERLAY_LINE  GUI_COLOR( 90, 160, 245, 200 )   /* its outline                       */

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
    bool            outer;      /* the chip is an edge chip -> split the ROOT   */
    gui_id_t      win_id;     /* the dragged window                          */
    u32             viewport;   /* dockspace surface under the cursor          */
    gui_dock_id_t target;     /* leaf node the cursor is over                */
    i32             zone;       /* dock_zone_t                                 */

} s_dock_drag;

/* Undock-by-tab-drag: a tab press pending the move threshold (mirrors s_titlebar_drag_*). */
static struct
{
    bool       pending;
    gui_id_t win_id;          /* window whose tab is held */
    f32        px, py;          /* press position           */

} s_dock_tab_drag;

/* The leaf whose rect contains (mx,my), or NULL.  Descends the tree following the point. */
static gui_dock_node_t*
dock_leaf_at( gui_dock_node_t* n, f32 mx, f32 my )
{
    if ( !n || !gui_rect_contains( n->rect, mx, my ) )
        return NULL;
    if ( n->split == DOCK_SPLIT_NONE )
        return n;
    gui_dock_node_t* c = dock_leaf_at( n->child[ 0 ], mx, my );
    return c ? c : dock_leaf_at( n->child[ 1 ], mx, my );
}

/* Map a side zone to the split direction the new window docks toward. */
static gui_dir_t
dock_zone_dir( dock_zone_t z )
{
    switch ( z )
    {
        case DOCK_ZONE_LEFT:   return GUI_DIR_LEFT;
        case DOCK_ZONE_RIGHT:  return GUI_DIR_RIGHT;
        case DOCK_ZONE_TOP:    return GUI_DIR_UP;
        case DOCK_ZONE_BOTTOM: return GUI_DIR_DOWN;
        default:               return GUI_DIR_LEFT;
    }
}

/* The chip square for one zone, centered in the leaf: center plus the four arrows at a gap around it. */
static gui_rect_t
dock_chip_rect( gui_rect_t leaf, dock_zone_t z, f32 s, f32 g )
{
    gui_vec2_t c = gui_rect_center( leaf );
    f32 x = c.x - s * 0.5f, y = c.y - s * 0.5f;
    switch ( z )
    {
        case DOCK_ZONE_LEFT:   return ( gui_rect_t ){ x - g - s, y, s, s };
        case DOCK_ZONE_RIGHT:  return ( gui_rect_t ){ x + g + s, y, s, s };
        case DOCK_ZONE_TOP:    return ( gui_rect_t ){ x, y - g - s, s, s };
        case DOCK_ZONE_BOTTOM: return ( gui_rect_t ){ x, y + g + s, s, s };
        case DOCK_ZONE_CENTER:
        default:               return ( gui_rect_t ){ x, y, s, s };
    }
}

/* An outer (edge) chip for one side, hugging the dockspace edge centered along it.  Distinct from the
   per-leaf 5-way: an outer drop splits the whole tree so the new pane spans a FULL viewport edge --
   the only way to carve a pane across an existing split (e.g. a left column beside a top/bottom stack).
   No CENTER outer chip: tabbing into "the whole dockspace" has no single target leaf. */
static gui_rect_t
dock_outer_chip_rect( gui_rect_t area, dock_zone_t z, f32 s, f32 margin )
{
    gui_vec2_t c = gui_rect_center( area );
    switch ( z )
    {
        case DOCK_ZONE_LEFT:   return ( gui_rect_t ){ area.x + margin,                  c.y - s * 0.5f, s, s };
        case DOCK_ZONE_RIGHT:  return ( gui_rect_t ){ area.x + area.w - margin - s,     c.y - s * 0.5f, s, s };
        case DOCK_ZONE_TOP:    return ( gui_rect_t ){ c.x - s * 0.5f, area.y + margin,                  s, s };
        case DOCK_ZONE_BOTTOM: return ( gui_rect_t ){ c.x - s * 0.5f, area.y + area.h - margin - s,     s, s };
        default:               return ( gui_rect_t ){ 0, 0, 0, 0 };
    }
}

/* The half (or whole) of a node rect the window would occupy if dropped on zone -- the preview fill. */
static gui_rect_t
dock_zone_region( gui_rect_t r, dock_zone_t z )
{
    switch ( z )
    {
        case DOCK_ZONE_LEFT:   return ( gui_rect_t ){ r.x,                r.y,                r.w * 0.5f, r.h        };
        case DOCK_ZONE_RIGHT:  return ( gui_rect_t ){ r.x + r.w * 0.5f,   r.y,                r.w * 0.5f, r.h        };
        case DOCK_ZONE_TOP:    return ( gui_rect_t ){ r.x,                r.y,                r.w,        r.h * 0.5f };
        case DOCK_ZONE_BOTTOM: return ( gui_rect_t ){ r.x,                r.y + r.h * 0.5f,   r.w,        r.h * 0.5f };
        case DOCK_ZONE_CENTER:
        default:               return r;
    }
}

/* Compute + preview the drop target for a free window being dragged over a dockspace on its own
   surface.  Sets s_dock_drag (active only when the cursor is over a chip) and draws the overlay on
   the reserved high z-band.  Forward-declared in gui_internal.h for window_begin_ex to call. */
static void
dock_drag_detect( gui_id_t win_id, gui_window_t* win )
{
    s_dock_drag.active = false;
    s_dock_drag.outer  = false;
    s_dock_drag.win_id = win_id;
    s_dock_drag.zone   = DOCK_ZONE_NONE;

    u32 vp = win->viewport;
    if ( vp != s_io.mouse_viewport || vp >= g_ctx->max_viewports )
        return;
    gui_dock_node_t* root = g_ctx->viewports[ vp ].dock_root;
    if ( !root )
        return;
    gui_dock_node_t* leaf = dock_leaf_at( root, s_io.mouse_x, s_io.mouse_y );
    if ( !leaf )
        return;

    /* Chip layout: a title-bar-sized square per zone, centered in the leaf. */
    f32 s = WIN_TITLE_H * 1.4f;
    f32 g = 6.0f;

    /* Outer (edge) chips appear only once the root is split: when the tree is a single leaf the inner
       5-way already spans the whole surface, so an edge chip would be a redundant duplicate.  Once
       there's a split, edge chips are the ONLY way to carve a pane across it (a full-height left column
       beside a top/bottom stack, etc.) -- they target the root, not the leaf under the cursor. */
    bool        has_outer  = ( root->split != DOCK_SPLIT_NONE );
    f32         margin     = s * 0.5f + 10.0f;
    dock_zone_t outer_zone = DOCK_ZONE_NONE;
    if ( has_outer )
        for ( i32 z = DOCK_ZONE_LEFT; z <= DOCK_ZONE_BOTTOM; ++z )
            if ( rect_hit( dock_outer_chip_rect( root->rect, (dock_zone_t)z, s, margin ) ) )
                { outer_zone = (dock_zone_t)z; break; }

    /* Edge chips win over the inner 5-way: when the cursor is on an edge chip, that is the intent. */
    dock_zone_t zone = DOCK_ZONE_NONE;
    if ( outer_zone == DOCK_ZONE_NONE )
        for ( i32 z = DOCK_ZONE_CENTER; z <= DOCK_ZONE_BOTTOM; ++z )
            if ( rect_hit( dock_chip_rect( leaf->rect, (dock_zone_t)z, s, g ) ) ) { zone = (dock_zone_t)z; break; }

    bool outer = ( outer_zone != DOCK_ZONE_NONE );
    s_dock_drag.viewport = vp;
    s_dock_drag.outer    = outer;
    s_dock_drag.target   = outer ? root->id : leaf->id;
    s_dock_drag.zone     = outer ? outer_zone : zone;
    s_dock_drag.active   = ( s_dock_drag.zone != DOCK_ZONE_NONE );

    /* Overlay on the reserved high z-band, clipped to the surface. */
    const gui_viewport_t* v = &g_ctx->viewports[ vp ];
    draw_set_viewport ( vp );
    draw_set_sort_key ( DOCK_OVERLAY_Z );
    draw_set_root_clip( vp_w( v ), vp_h( v ) );

    draw_set_rounding( ROUND_WIDGET );   /* drop preview + chips read as control surfaces */

    /* Preview the band the window would take: a full viewport edge for an outer drop, else the half
       (or whole, for center) of the leaf under the cursor. */
    if ( s_dock_drag.zone != DOCK_ZONE_NONE )
    {
        gui_rect_t base = outer ? root->rect : leaf->rect;
        gui_rect_t hr   = dock_zone_region( base, (dock_zone_t)s_dock_drag.zone );
        draw_push_rect_filled ( hr.x, hr.y, hr.w, hr.h, 0, 0, 1, 1, 0, DOCK_OVERLAY_FILL );
        draw_push_rect_outline( hr.x, hr.y, hr.w, hr.h, WIN_BORDER, 0, DOCK_OVERLAY_LINE );
    }

    for ( i32 z = DOCK_ZONE_CENTER; z <= DOCK_ZONE_BOTTOM; ++z )
    {
        gui_rect_t cr = dock_chip_rect( leaf->rect, (dock_zone_t)z, s, g );
        bool         on = ( !outer && (dock_zone_t)z == zone );
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

    /* Edge chips: drawn against the dockspace edges, each pointing outward to read as "full side". */
    if ( has_outer )
        for ( i32 z = DOCK_ZONE_LEFT; z <= DOCK_ZONE_BOTTOM; ++z )
        {
            gui_rect_t cr = dock_outer_chip_rect( root->rect, (dock_zone_t)z, s, margin );
            bool         on = ( outer && (dock_zone_t)z == outer_zone );
            draw_push_rect_filled ( cr.x, cr.y, cr.w, cr.h, 0, 0, 1, 1, 0, on ? COL_WIDGET_HOT : COL_WIDGET_BG );
            draw_push_rect_outline( cr.x, cr.y, cr.w, cr.h, WIN_BORDER, 0, COL_BORDER );
            draw_arrow( cr, dock_zone_dir( (dock_zone_t)z ), COL_TEXT );
        }

    draw_set_sort_key ( 0 );
    draw_set_viewport ( 0 );
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );
}

/* Execute the drop computed by dock_drag_detect: tab into the target leaf (center) or split it and
   dock on a side.  Called unconditionally from window_end for every free window; no-ops unless this
   is the dragged window on its release edge (the gate is here because s_dock_drag is unit-private).
   Forward-declared in gui_internal.h. */
static void
dock_drag_commit( gui_id_t win_id, const char* title )
{
    if ( !s_dock_drag.active || s_dock_drag.win_id != win_id || !s_io.mouse_released[ 0 ] )
        return;

    if ( s_dock_drag.zone != DOCK_ZONE_NONE && title )
    {
        if ( s_dock_drag.outer )
        {
            /* Edge drop: split the whole tree so the new pane spans a full viewport edge. */
            gui_dock_id_t side = gui_dock_split_root( (gui_vp_t)s_dock_drag.viewport,
                                                          dock_zone_dir( (dock_zone_t)s_dock_drag.zone ),
                                                          0.5f );
            if ( side != GUI_DOCK_NONE )
                gui_dock_window( title, side );
        }
        else
        {
            gui_dock_node_t* leaf = dock_node_find( s_dock_drag.target );
            if ( leaf && leaf->split == DOCK_SPLIT_NONE )
            {
                if ( s_dock_drag.zone == DOCK_ZONE_CENTER )
                {
                    gui_dock_window( title, leaf->id );
                }
                else
                {
                    gui_dock_id_t side = gui_dock_split( leaf->id,
                                                             dock_zone_dir( (dock_zone_t)s_dock_drag.zone ),
                                                             0.5f, NULL );
                    if ( side != GUI_DOCK_NONE )
                        gui_dock_window( title, side );
                }
            }
        }
    }
    s_dock_drag.active = false;
}

/* Remove a window from its node by id (the undock-by-drag path); collapse a node it empties. */
static void
dock_undock_by_id( gui_id_t win )
{
    gui_dock_node_t* n = dock_find_window_node( win );
    if ( !n )
        return;
    for ( u32 i = 0; i < n->tab_count; ++i )
        if ( n->tabs[ i ] == win ) { dock_leaf_remove_tab( n, i ); break; }
    if ( n->tab_count == 0 )
        dock_collapse( n );
}

/*----------------------------------------------------------------------------------------------
    Tab-strip chrome -- drawn by the active window's window_end in place of a title bar.

    Runs while s_build holds the active docked window (its id is hover_win when the cursor is over the
    node, and its clip is the node rect), so widget_behavior hit-tests the tabs correctly.  Tabs march
    left-to-right at natural width; clicking one selects it (takes effect next frame, when that window
    becomes the active tab).  A press dragged past the threshold pops the window out (see below).
    Forward-declared in gui_internal.h for window_end to call.
----------------------------------------------------------------------------------------------*/

static void
dock_window_chrome( gui_dock_node_t* node )
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
        gui_rect_t tr = { tx, y, tw, th };
        bool         is_active = ( i == node->active_tab );

        gui_id_t     tid = id_combine( node->id, DOCK_TAB_SALT + i );
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
                gui_id_t wid = s_dock_tab_drag.win_id;
                u32        vp  = node->viewport;   /* capture before a collapse may free `node` */
                s_dock_tab_drag.pending = false;

                dock_undock_by_id( wid );

                gui_window_t* win = window_find( wid );
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

// clang-format on
/*============================================================================================*/
