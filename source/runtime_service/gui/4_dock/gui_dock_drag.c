/*==============================================================================================

    runtime_service/gui/4_dock/gui_dock_drag.c -- Mouse docking gestures: drag-to-dock + tab strip.

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
    Drag-to-dock + undock-by-tab-drag (the mouse gestures)

    Drag-to-dock: while a FREE window is title-dragged over a dockspace on the same surface,
    dock_drag_detect (called from window_begin_ex) finds the leaf under the cursor, draws a per-node
    5-way chip overlay + a translucent preview of the region the window would take, and records the
    chip the cursor is over.  Hovering the leaf's TAB BAND is a tab drop by itself (the same "merge
    into these tabs" gesture a floating group's strip offers), no chip aim needed.  On the release
    edge, dock_drag_commit (from window_end) tabs the window
    into the leaf (center or strip) or splits the leaf and docks it on a side -- reusing the tree edits in
    gui_dock_core.c (via the public verbs in gui_dock.c: gui_dock_split / gui_dock_window).
    The overlay paints on a reserved z-band above everything (popups sit at 0x80000000).

    Tab drag: a press on a tab rides the generic drag machine (drag_from_chrome, 2_interact/gui_drag.c),
    publishing a "gui.dock_tab" payload.  While the cursor stays inside the strip band the drag
    REORDERS the tabs live (dock_strip_reorder); leaving the band vertically UNDOCKS -- pops that
    window out of its node into a free window that follows the cursor -- handled inside
    dock_window_chrome below, grabbed through move_grab (2_interact/gui_move.c) so the free
    window-drag apply carries it from next frame.
----------------------------------------------------------------------------------------------*/

/* Tab salt: each tab gets a stable per-node widget id distinct from the windows + splitter (see
   DOCK_SPLIT_SALT in gui_dock_core.c). */
#define DOCK_TAB_SALT   0xD0C7AB00u

#define DOCK_TAB_DRAG_THRESH    12.0f                               /* px a tab must move before it undocks     */
#define DOCK_OVERLAY_Z          0xF0000000u                         /* above the popup z-band (0x80000000)     */
#define DOCK_OVERLAY_FILL       GUI_COLOR( 90, 160, 245,  64 )      /* translucent drop-region preview   */
#define DOCK_OVERLAY_LINE       GUI_COLOR( 90, 160, 245, 200 )      /* its outline                       */

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

    /* Tab-onto-window drop (gui_dock_float.c): the cursor is over another free window's title
       bar (float_new -- release enqueues a group creation around float_win) or an existing
       floating group's strip (float_join -- target holds the group node, release tabs straight
       in).  Mutually exclusive with the dockspace chips above; zone is CENTER for both. */
    bool            float_join;
    bool            float_new;
    gui_id_t      float_win;

} s_dock_drag;

/* Undock-by-tab-drag: a tab press pending the move threshold (same click-vs-drag idea as the
   press_defer latch, but carrying its own payload and threshold).
   node_id pins the gesture to the strip it started on: every docked node's chrome runs the
   pending block, and the reorder-vs-undock decision reads THAT chrome's strip band -- another
   node's chrome (whichever emits first) would test the cursor against the wrong strip and
   tear the tab out instantly. */
static struct
{
    bool            pending;
    gui_id_t      win_id;     /* window whose tab is held        */
    gui_dock_id_t node_id;    /* node whose strip owns the press */
    f32             px, py;     /* press position                  */

} s_dock_tab_drag;

/* The leaf whose rect contains (mx,my), or NULL.  Descends the tree following the point. */
static gui_dock_node_t*
dock_leaf_at( gui_dock_node_t* n, f32 mx, f32 my )
{
    if ( !n || !gui_rect_contains( n->rect, mx, my ) )
        return NULL;
    if ( n->split == DOCK_SPLIT_NONE )
        return n;
    gui_dock_node_t* c = dock_leaf_at( dock_at( n->child[ 0 ] ), mx, my );
    return c ? c : dock_leaf_at( dock_at( n->child[ 1 ] ), mx, my );
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
    s_dock_drag.active     = false;
    s_dock_drag.outer      = false;
    s_dock_drag.win_id     = win_id;
    s_dock_drag.zone       = DOCK_ZONE_NONE;
    s_dock_drag.float_join = false;
    s_dock_drag.float_new  = false;
    s_dock_drag.float_win  = GUI_ID_NONE;

    u32 vp = win->viewport;
    if ( vp != s_io.mouse_viewport || vp >= g_ctx->max_viewports )
        return;

    /* Chip layout: a title-bar-sized square per zone, centered in the leaf. */
    f32 s = WIN_TITLE_H * 1.4f;
    f32 g = 6.0f;

    /* Tab-onto-window (gui_dock_float.c): another free window's title bar -- or an existing
       floating group's strip -- under the cursor pre-empts the dockspace chips below (the strip
       is visually above them).  One center chip on the band, previewing the whole target frame:
       the drop tabs the dragged window onto it. */
    {
        gui_dock_node_t* fnode = NULL;
        gui_id_t       fwin  = GUI_ID_NONE;
        if ( dock_float_hit( win_id, vp, &fnode, &fwin ) )
        {
            gui_window_t* tw   = window_find( fwin );
            gui_rect_t    base = fnode ? fnode->rect
                                 : tw    ? ( gui_rect_t ){ tw->x, tw->y, tw->w, tw->h }
                                         : ( gui_rect_t ){ 0, 0, 0, 0 };

            s_dock_drag.viewport   = vp;
            s_dock_drag.zone       = DOCK_ZONE_CENTER;
            s_dock_drag.active     = true;
            s_dock_drag.float_join = ( fnode != NULL );
            s_dock_drag.float_new  = ( fnode == NULL );
            s_dock_drag.target     = fnode ? fnode->id : GUI_DOCK_NONE;
            s_dock_drag.float_win  = fwin;

            const gui_viewport_t* v = &g_ctx->viewports[ vp ];
            draw_set_viewport ( vp );
            draw_set_sort_key ( DOCK_OVERLAY_Z );
            draw_set_root_clip( vp_w( v ), vp_h( v ) );
            draw_set_rounding ( ROUND_WIDGET );

            draw_push_rect_filled ( base.x, base.y, base.w, base.h, 0, 0, 1, 1, 0, DOCK_OVERLAY_FILL );
            draw_push_rect_outline( base.x, base.y, base.w, base.h, WIN_BORDER, 0, DOCK_OVERLAY_LINE );

            /* The hot center chip with its "tab here" glyph, over the title band. */
            gui_rect_t cr = { base.x + base.w * 0.5f - s * 0.5f, base.y + ( WIN_TITLE_H - s ) * 0.5f, s, s };
            draw_push_rect_filled ( cr.x, cr.y, cr.w, cr.h, 0, 0, 1, 1, 0, COL_WIDGET_HOT );
            draw_push_rect_outline( cr.x, cr.y, cr.w, cr.h, WIN_BORDER, 0, COL_BORDER );
            f32 ins = cr.w * 0.28f;
            draw_set_rounding( 0.0f );
            draw_push_rect_outline( cr.x + ins, cr.y + ins, cr.w - 2.0f * ins, cr.h - 2.0f * ins,
                                    WIN_BORDER, 0, COL_TEXT );

            draw_set_sort_key ( 0 );
            draw_set_viewport ( 0 );
            draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );
            return;
        }
    }

    gui_dock_node_t* root = dock_at( g_ctx->viewports[ vp ].dock_root );
    if ( !root )
        return;
    gui_dock_node_t* leaf = dock_leaf_at( root, s_io.mouse_x, s_io.mouse_y );
    if ( !leaf )
        return;

    /* NO_SPLIT dockspace: tab docking only -- the center chip stands alone, no side or edge chips. */
    bool no_split = ( g_ctx->viewports[ vp ].dock_flags & GUI_DOCKSPACE_NO_SPLIT ) != 0;

    /* Outer (edge) chips appear only once the root is split: when the tree is a single leaf the inner
       5-way already spans the whole surface, so an edge chip would be a redundant duplicate.  Once
       there's a split, edge chips are the ONLY way to carve a pane across it (a full-height left column
       beside a top/bottom stack, etc.) -- they target the root, not the leaf under the cursor. */
    bool        has_outer  = !no_split && ( root->split != DOCK_SPLIT_NONE );
    f32         margin     = s * 0.5f + 10.0f;
    dock_zone_t outer_zone = DOCK_ZONE_NONE;
    if ( has_outer )
        for ( i32 z = DOCK_ZONE_LEFT; z <= DOCK_ZONE_BOTTOM; ++z )
            if ( rect_hit( dock_outer_chip_rect( root->rect, (dock_zone_t)z, s, margin ) ) )
                { outer_zone = (dock_zone_t)z; break; }

    /* Tab-strip drop: the cursor over the leaf's TAB BAND is the "merge into these tabs" gesture
       -- the same drop a floating group's strip offers -- so it tabs in directly, without having
       to reach the center chip.  Only offered where a strip is visible (the leaf has tabs). */
    bool strip_drop = ( outer_zone == DOCK_ZONE_NONE ) && ( leaf->tab_count > 0 )
                   && ( s_io.mouse_y < leaf->content.y );

    /* Edge chips win over the inner 5-way: when the cursor is on an edge chip, that is the intent.
       A NO_SPLIT dockspace offers only the center (tab) chip. */
    i32         zone_last = no_split ? (i32)DOCK_ZONE_CENTER : (i32)DOCK_ZONE_BOTTOM;
    dock_zone_t zone      = DOCK_ZONE_NONE;
    if ( strip_drop )
        zone = DOCK_ZONE_CENTER;
    else if ( outer_zone == DOCK_ZONE_NONE )
        for ( i32 z = DOCK_ZONE_CENTER; z <= zone_last; ++z )
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

    /* Strip drop: the hot "tab here" chip sits on the tab band itself (the float-strip visual),
       so the gesture reads as "join these tabs" rather than pointing at the center chip. */
    if ( strip_drop )
    {
        f32          sh = leaf->content.y - leaf->rect.y;
        gui_rect_t cr = { leaf->rect.x + leaf->rect.w * 0.5f - s * 0.5f,
                            leaf->rect.y + ( sh - s ) * 0.5f, s, s };
        draw_push_rect_filled ( cr.x, cr.y, cr.w, cr.h, 0, 0, 1, 1, 0, COL_WIDGET_HOT );
        draw_push_rect_outline( cr.x, cr.y, cr.w, cr.h, WIN_BORDER, 0, COL_BORDER );
        f32 ins = cr.w * 0.28f;
        draw_set_rounding( 0.0f );   /* small glyph box stays square */
        draw_push_rect_outline( cr.x + ins, cr.y + ins, cr.w - 2.0f * ins, cr.h - 2.0f * ins,
                                WIN_BORDER, 0, COL_TEXT );
        draw_set_rounding( ROUND_WIDGET );
    }

    for ( i32 z = DOCK_ZONE_CENTER; z <= zone_last; ++z )
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
        if ( s_dock_drag.float_join )
        {
            /* Drop on an existing floating group's strip: tab straight in (title in hand). */
            gui_dock_window( title, s_dock_drag.target );
        }
        else if ( s_dock_drag.float_new )
        {
            /* Drop on a free window's title bar: the target's name is not in hand here, so the
               group forms when the target next begins (dock_float_service_request). */
            dock_float_request( s_dock_drag.float_win, win_id, title );
        }
        else if ( s_dock_drag.outer )
        {
            /* Edge drop: split the whole tree so the new pane spans a full viewport edge. */
            gui_dock_id_t side = gui_dock_split_root( s_dock_drag.viewport,
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

/* Remove a window from its node by id -- the undock-by-drag path's form of dock_node_remove_window
   (gui_dock_core.c), which gui_dock_undock also uses from the title-string API. */
static void
dock_undock_by_id( gui_id_t win )
{
    gui_dock_node_t* n = dock_find_window_node( win );
    if ( n )
        dock_node_remove_window( n, win );
}

/*----------------------------------------------------------------------------------------------
    Tab reorder -- a tab drag that stays inside the strip band slides the tab through the strip.

    Live reorder: while the drag is in flight the dragged tab follows the cursor by crossing into
    its neighbours (edge-triggered, flicker-guarded for unequal widths -- see dock_strip_reorder),
    immediately (no drop step), and stays the active (visible) tab as it moves.  Leaving the strip band vertically is what undocks instead
    (dock_window_chrome below decides which of the two the cursor position means each frame).
----------------------------------------------------------------------------------------------*/

static void
dock_strip_reorder( gui_dock_node_t* node, gui_id_t wid, f32 strip_x )
{
    /* Current index of the dragged window's tab. */
    u32 from = node->tab_count;
    for ( u32 i = 0; i < node->tab_count; ++i )
        if ( node->tabs[ i ] == wid ) { from = i; break; }
    if ( from >= node->tab_count )
        return;

    node->active_tab = from;   /* the dragged tab is the one being looked at */

    /* Destination slot: EDGE-triggered -- the tab moves the moment the cursor crosses into the
       neighbouring tab, not at its midpoint (the standard tab-bar feel).  With unequal widths a
       bare edge trigger oscillates: right after a swap the cursor can land back inside the tab it
       just passed, immediately re-triggering the reverse move.  So each step arms at the FARTHER
       of the two stable points -- the neighbour's near edge (the gesture) and where the dragged
       tab's origin lands after the swap (the anti-flicker floor):

           right: cross  t0 + max( W, Wn )    left: cross  tp + min( W, Wp )

       where t0 is the dragged tab's slot origin, W its width, Wn/Wp the neighbour's, tp = t0 - Wp.
       Equal widths reduce both to exactly the shared edge.  Walked stepwise (widths simulated
       through the swaps) so one long pull can carry the tab across several neighbours per frame. */
    f32 tw[ GUI_DOCK_TABS_MAX ];
    f32 t0 = strip_x;
    for ( u32 i = 0; i < node->tab_count; ++i )
    {
        tw[ i ] = font_text_w( node->names[ i ] ) + 2.0f * WIDGET_PAD;
        if ( i < from )
            t0 += tw[ i ];
    }
    f32 W  = tw[ from ];
    u32 to = from;

    while ( to + 1 < node->tab_count
            && s_io.mouse_x > t0 + ( ( W > tw[ to + 1 ] ) ? W : tw[ to + 1 ] ) )
    {
        t0 += tw[ to + 1 ];                             /* dragged origin after the swap      */
        tw[ to ] = tw[ to + 1 ];  tw[ to + 1 ] = W;     /* neighbour slides into the old slot */
        ++to;
    }
    while ( to > 0
            && s_io.mouse_x < t0 - tw[ to - 1 ] + ( ( W < tw[ to - 1 ] ) ? W : tw[ to - 1 ] ) )
    {
        t0 -= tw[ to - 1 ];
        tw[ to ] = tw[ to - 1 ];  tw[ to - 1 ] = W;
        --to;
    }
    if ( to == from )
        return;

    /* Move from -> to, sliding the span between them one slot toward `from`. */
    gui_id_t twin = node->tabs[ from ];
    char       tname[ GUI_DOCK_NAME_CAP ];
    memcpy( tname, node->names[ from ], GUI_DOCK_NAME_CAP );

    if ( to < from )
        for ( u32 i = from; i > to; --i )
        {
            node->tabs[ i ] = node->tabs[ i - 1 ];
            memcpy( node->names[ i ], node->names[ i - 1 ], GUI_DOCK_NAME_CAP );
        }
    else
        for ( u32 i = from; i < to; ++i )
        {
            node->tabs[ i ] = node->tabs[ i + 1 ];
            memcpy( node->names[ i ], node->names[ i + 1 ], GUI_DOCK_NAME_CAP );
        }

    node->tabs[ to ] = twin;
    memcpy( node->names[ to ], tname, GUI_DOCK_NAME_CAP );
    node->active_tab = to;
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
        {
            node->active_tab = i;
            /* The tab switch also moves keyboard focus: click-to-focus (gui_nav.c) latched the OLD
               active window (it owned hover), which stops emitting the moment this tab takes over --
               retarget nav at the window actually coming to the front. */
            s_nav.explicit_win = node->tabs[ i ];
        }

        /* Press arms an undock-by-drag: the move threshold below decides click (select) vs drag-out. */
        if ( st.pressed )
        {
            s_dock_tab_drag.pending = true;
            s_dock_tab_drag.win_id  = node->tabs[ i ];
            s_dock_tab_drag.node_id = node->id;
            s_dock_tab_drag.px      = s_io.mouse_x;
            s_dock_tab_drag.py      = s_io.mouse_y;
        }

        tx += tw;
    }

    /* Floating group (gui_dock_float.c): the strip's empty band doubles as the group's title-bar
       drag surface -- a press grabs the move (widget_behavior claims the press; move_grab records
       the offsets that keep the grabbed point pinned under the cursor, applied by
       dock_float_resolve's move_track next frame). */
    if ( node->floating )
    {
        gui_rect_t rem = { tx, y, x + w - tx, th };
        if ( rem.w > 1.0f )
        {
            gui_id_t     gid = id_combine( node->id, DOCK_FLOAT_SALT );
            s_build.nav_skip = true;   /* pure drag surface -- never a keyboard target */
            widget_state_t st  = widget_behavior( gid, rem, WIDGET_KIND_BUTTON );
            if ( st.pressed )
                move_grab( gid, 0, x, y );   /* released globally when the left button lifts */
        }
    }

    /* Border frames the whole node (strip + body), square so adjacent nodes tile flush at right
       angles.  Drawn before the undock handler so it never reads `node` after a drag-out collapses
       an emptied node. */
    draw_set_rounding( 0.0f );
    draw_push_rect_outline( x, y, w, s_build.win_h, WIN_BORDER, 0, COL_BORDER );

    /* Floating group: bold the hot / grabbed resize edges over the thin border, exactly like a
       free window's window_end does (the hot mask was resolved in dock_float_resolve). */
    if ( node->floating )
    {
        u8 hot_edges = ( s_interaction.active_id == id_combine( node->id, GUI_RESIZE_SALT ) )
                     ? s_resize_edges : s_build.win_resize_hot;
        if ( hot_edges )
            draw_resize_highlight( ( gui_rect_t ){ x, y, w, s_build.win_h }, hot_edges );
    }

    /* Tab drag: an armed tab press rides the generic drag machine (drag_from_chrome publishes a
       "gui.dock_tab" payload carrying the window id, so drop targets elsewhere can accept it).
       Once live, the cursor position decides the gesture each frame: inside the strip band it
       REORDERS the tabs (dock_strip_reorder above); leaving the band vertically past the
       threshold UNDOCKS -- the window pops out into a free window that follows the cursor,
       grabbed through move_grab so the free drag-apply in window_begin_ex carries the move
       from next frame.  Released without moving was a tab click (st.clicked already selected it). */
    if ( s_dock_tab_drag.pending )
    {
        if ( !s_io.mouse_down[ 0 ] )
        {
            s_dock_tab_drag.pending = false;
        }
        else if ( s_dock_tab_drag.node_id == node->id )   /* only the pressed strip's chrome decides */
        {
            gui_id_t wid = s_dock_tab_drag.win_id;
            if ( drag_from_chrome( wid, s_dock_tab_drag.px, s_dock_tab_drag.py,
                                   "gui.dock_tab", &wid, sizeof wid ) )
            {
                bool in_strip = ( s_io.mouse_y >= y - DOCK_TAB_DRAG_THRESH )
                             && ( s_io.mouse_y <  y + th + DOCK_TAB_DRAG_THRESH );
                if ( in_strip )
                {
                    dock_strip_reorder( node, wid, x );
                }
                else
                {
                    u32 vp = node->viewport;   /* capture before a collapse may free `node` */
                    s_dock_tab_drag.pending = false;

                    dock_undock_by_id( wid );

                    gui_window_t* win = window_find( wid );
                    if ( win )
                    {
                        win->viewport = vp;              /* float on the surface it was docked on */
                        win->z        = ++s_z_counter;   /* raise above the tiles                 */
                        win->x        = s_io.mouse_x - WIN_TITLE_H;         /* grab near the      */
                        win->y        = s_io.mouse_y - WIN_TITLE_H * 0.5f;  /* title's left edge  */
                        move_grab( wid, 0, win->x, win->y );  /* continue as a free window drag   */
                    }
                }
            }
        }
    }
}

// clang-format on
/*============================================================================================*/
