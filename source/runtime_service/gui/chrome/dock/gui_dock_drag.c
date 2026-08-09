/*==============================================================================================

    runtime_service/gui/chrome/dock/gui_dock_drag.c -- Mouse docking gestures: drag-to-dock + tab strip.

    The two mouse gestures layered on top of the programmatic dock tree (gui_dock_core.c /
    gui_dock.c): drag-to-dock (a free window title-dragged over a dockspace previews a per-node
    5-way drop target and commits on release) and undock-by-tab-drag (a press-and-drag on a docked
    tab pops that window back out to free-floating).  Also owns the tab-strip + node-border chrome a
    docked window draws in place of a title bar -- the surface the tab-drag gesture presses on.

    window/ reaches dock_drag_detect / dock_drag_commit / dock_window_chrome only through the
    route seam (chrome/dock/gui_dock_route.c, included after this file); nothing here is visible to
    earlier files directly.  Needs gui_dock_core.c's node pool + leaf-edit helpers already in
    scope, so it is included right after that file.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Drag-to-dock + undock-by-tab-drag (the mouse gestures)

    Drag-to-dock: while a FREE window is title-dragged over a dockspace on the same surface,
    dock_drag_detect (called from window_begin_ex) finds the leaf under the cursor, draws a per-node
    5-way chip overlay + a translucent preview of the region the window would take, and records the
    chip the cursor is over.  Hovering the leaf's TAB BAND is a tab drop by itself (the same "merge
    into these tabs" gesture a floating group's strip offers), no chip aim needed.  On the release
    edge, dock_drag_commit (from window_end) tabs the window
    into the leaf (center or strip) or splits the leaf and docks it on a side -- reusing the tree edits in
    gui_dock_core.c (via the public verbs in gui_dock.c: gui_dock_split / gui_dock_window).
    The overlay paints in its own synthetic draw slot (DOCK_OVERLAY_WIN) on the topmost z band,
    above every popup, foreground region, and splitter (see the band map in gui_surface.c).

    Tab drag: a press on a tab rides the generic drag machine (drag_from_chrome, interact/gui_drag.c),
    publishing a "gui.dock_tab" payload.  While the cursor stays inside the strip band the drag
    REORDERS the tabs live (dock_strip_reorder); leaving the band vertically UNDOCKS -- pops that
    window out of its node into a free window that follows the cursor -- handled inside
    dock_window_chrome below, grabbed through move_grab (interact/gui_move.c) so the free
    window-drag apply carries it from next frame.
==============================================================================================*/

/* Tab salt: each tab gets a stable per-node widget id distinct from the windows + splitter (see
   DOCK_SPLIT_SALT in gui_dock_core.c). */
#define DOCK_TAB_SALT   0xD0C7AB00u

#define DOCK_TAB_DRAG_THRESH    12.0f                               /* px a tab must move before it undocks     */

/* The drop overlay owns its own draw slot at the topmost z band (see the band map in
   gui_surface.c): a synthetic window id keeps its high z from riding on whatever segment happens
   to be open when window_begin runs the gesture (the win-0 background, whose splitters and host
   draws must NOT be hoisted along), and the z sits strictly ABOVE every other band -- popups
   (0x80000000+) and foreground regions (0xF0000000) included -- so the drop graphic can never
   lose a tie to a splitter gutter or any other chrome it overlaps. */
#define DOCK_OVERLAY_WIN        ( (gui_id_t)0xD0C0DA6u )            /* synthetic slot id, draw-side only */
#define DOCK_OVERLAY_Z          0xF8000000u                         /* topmost band -- above GUI_REGION_FG_Z    */
/* The drop preview's colour is the INFO signal, not a literal.  It used to be a hardcoded blue,
   which meant a kit that re-seeded its whole UI gold still got a stock blue drop graphic -- a
   private literal is a colour the theme cannot reach.  Alpha is punched in here rather than
   authored into the seed because these two are the SAME signal at two weights (a wash and its
   outline), which is a draw decision, not a palette one. */
#define DOCK_OVERLAY_FILL       ( ( COL_INFO_IDLE & 0x00FFFFFFu ) |  64u << 24 )   /* drop-region preview */
#define DOCK_OVERLAY_LINE       ( ( COL_INFO_IDLE & 0x00FFFFFFu ) | 200u << 24 )   /* its outline         */

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
    gui_vp_t        viewport;   /* dockspace surface under the cursor          */
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
    if ( n->split == GUI_DOCK_SPLIT_NONE )
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

/* Enter the overlay's draw scope: its own synthetic slot on the reserved topmost z-band, clipped
   to the dockspace surface, chips rounded as control surfaces.  Paired with dock_overlay_end,
   which restores the ambient build state for the windows emitted next. */
static void
dock_overlay_begin( gui_vp_t vp )
{
    draw_set_window   ( DOCK_OVERLAY_WIN );
    draw_set_viewport ( vp );
    draw_set_sort_key ( DOCK_OVERLAY_Z );
    draw_set_root_clip( vp_w( vp ), vp_h( vp ) );
    draw_set_rounding ( ROUND_WIDGET );
}

static void
dock_overlay_end( void )
{
    draw_set_window   ( 0 );
    draw_set_sort_key ( 0 );
    draw_set_viewport ( 0 );
    draw_set_root_clip( (f32)s_io.display_w, (f32)s_io.display_h );
}

/* One drop chip: filled square + border, lit when it is the armed drop. */
static void
dock_chip_draw( gui_rect_t cr, bool on )
{
    draw_push_rect_filled ( cr.x, cr.y, cr.w, cr.h, 0, 0, 1, 1, 0, on ? COL_BG_HOT : COL_BG_IDLE );
    draw_push_rect_outline( cr.x, cr.y, cr.w, cr.h, WIN_BORDER, COL_BORDER_IDLE );
}

/* The "tab here" glyph: a small square inset in a center chip (kept square while the chip itself
   rounds; restores the ambient chip rounding after). */
static void
dock_chip_tab_glyph( gui_rect_t cr )
{
    f32 ins = cr.w * 0.28f;
    draw_set_rounding( 0.0f );
    draw_push_rect_outline( cr.x + ins, cr.y + ins, cr.w - 2.0f * ins, cr.h - 2.0f * ins,
                            WIN_BORDER, COL_TEXT_IDLE );
    draw_set_rounding( ROUND_WIDGET );
}

/* Tab-onto-window pre-empt: another free window's title bar -- or an existing floating group's
   strip -- under the cursor takes priority over the dockspace chips (the strip sits visually above
   them).  When one is hit, arm s_dock_drag for a tab / float-join drop, draw one center chip
   previewing the whole target frame, and return true so the caller skips the dockspace path. */
static bool
dock_drag_float_target( gui_id_t win_id, gui_vp_t vp, f32 s )
{
    gui_dock_node_t* fnode = NULL;
    gui_id_t       fwin  = GUI_ID_NONE;
    if ( !dock_float_hit( win_id, vp, &fnode, &fwin ) )
        return false;

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

    dock_overlay_begin( vp );

    draw_push_rect_filled ( base.x, base.y, base.w, base.h, 0, 0, 1, 1, 0, DOCK_OVERLAY_FILL );
    draw_push_rect_outline( base.x, base.y, base.w, base.h, WIN_BORDER, DOCK_OVERLAY_LINE );

    /* The hot center chip with its "tab here" glyph, over the title band. */
    gui_rect_t cr = { base.x + base.w * 0.5f - s * 0.5f, base.y + ( WIN_TITLE_H - s ) * 0.5f, s, s };
    dock_chip_draw( cr, true );
    dock_chip_tab_glyph( cr );

    dock_overlay_end();
    return true;
}

/* Compute + preview the drop target for a free window being dragged over a dockspace on its own
   surface.  Sets s_dock_drag (active only when the cursor is over a chip) and draws the overlay on
   the reserved high z-band.  Forward-declared in chrome/gui_chrome.h for window_begin_ex to call. */
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

    gui_vp_t vp = win->viewport;
    if ( vp != s_io.mouse_viewport || vp < 0 || vp >= GUI_MAX_VIEWPORTS )
        return;

    /* Chip layout: a title-bar-sized square per zone, centered in the leaf. */
    f32 s = WIN_TITLE_H * 1.4f;
    f32 g = 6.0f;

    /* Tab-onto-window pre-empts the dockspace chips below (see dock_drag_float_target). */
    if ( dock_drag_float_target( win_id, vp, s ) )
        return;

    /* A dormant dockspace (tree retained but not emitted this build) offers no drop chips --
       drops would land in rects that no longer lay out.  See dock_seen_frame, core/gui_ctx.h. */
    gui_dock_node_t* root = dock_at( s_vp_pool[ vp ].dock_root );
    if ( !root || !dock_vp_emitted( vp ) )
        return;

    /* A settled maximized leaf covers the dockspace: the tree's rects are all obscured, so no
       drop chips are offered while fullscreen -- restore first, then dock. */
    if ( s_vp_pool[ vp ].dock_max_settled )
        return;
    /* Over a splitter gutter no leaf contains the cursor.  That must NOT drop the whole overlay:
       the viewport edge chips target the ROOT and stay offered anywhere inside the dockspace, so
       a NULL leaf only suppresses the per-leaf 5-way below.  Fully outside the dockspace area
       there is nothing to offer. */
    gui_dock_node_t* leaf = dock_leaf_at( root, s_io.mouse_x, s_io.mouse_y );
    if ( !leaf && !rect_hit( root->rect ) )
        return;

    /* NO_SPLIT dockspace: tab docking only -- the center chip stands alone, no side or edge chips. */
    bool no_split = ( s_vp_pool[ vp ].dock_flags & GUI_DOCKSPACE_NO_SPLIT ) != 0;

    /* Outer (edge) chips appear only once the root is split: when the tree is a single leaf the inner
       5-way already spans the whole surface, so an edge chip would be a redundant duplicate.  Once
       there's a split, edge chips are the ONLY way to carve a pane across it (a full-height left column
       beside a top/bottom stack, etc.) -- they target the root, not the leaf under the cursor. */
    bool        has_outer  = !no_split && ( root->split != GUI_DOCK_SPLIT_NONE );
    f32         margin     = s * 0.5f + 10.0f;
    dock_zone_t outer_zone = DOCK_ZONE_NONE;
    if ( has_outer )
        for ( i32 z = DOCK_ZONE_LEFT; z <= DOCK_ZONE_BOTTOM; ++z )
            if ( rect_hit( dock_outer_chip_rect( root->rect, (dock_zone_t)z, s, margin ) ) )
                { outer_zone = (dock_zone_t)z; break; }

    /* A tab drop (strip band or center chip) needs a free slot in the leaf's tab list -- a full
       leaf offers neither, so the preview never promises a drop the commit would refuse. */
    bool leaf_can_tab = leaf && ( leaf->tab_count < GUI_DOCK_TABS_MAX );

    /* Tab-strip drop: the cursor over the leaf's TAB BAND is the "merge into these tabs" gesture
       -- the same drop a floating group's strip offers -- so it tabs in directly, without having
       to reach the center chip.  Only offered where a strip is visible (the leaf has tabs). */
    bool strip_drop = ( outer_zone == DOCK_ZONE_NONE ) && leaf_can_tab && ( leaf->tab_count > 0 )
                   && ( s_io.mouse_y < leaf->content.y );

    /* Edge chips win over the inner 5-way: when the cursor is on an edge chip, that is the intent.
       A NO_SPLIT dockspace offers only the center (tab) chip. */
    i32         zone_last = no_split ? (i32)DOCK_ZONE_CENTER : (i32)DOCK_ZONE_BOTTOM;
    dock_zone_t zone      = DOCK_ZONE_NONE;
    if ( strip_drop )
        zone = DOCK_ZONE_CENTER;
    else if ( leaf && outer_zone == DOCK_ZONE_NONE )
        for ( i32 z = DOCK_ZONE_CENTER; z <= zone_last; ++z )
        {
            if ( z == DOCK_ZONE_CENTER && !leaf_can_tab )
                continue;
            if ( rect_hit( dock_chip_rect( leaf->rect, (dock_zone_t)z, s, g ) ) ) { zone = (dock_zone_t)z; break; }
        }

    bool outer = ( outer_zone != DOCK_ZONE_NONE );
    s_dock_drag.viewport = vp;
    s_dock_drag.outer    = outer;
    s_dock_drag.target   = outer ? root->id : leaf ? leaf->id : GUI_DOCK_NONE;
    s_dock_drag.zone     = outer ? outer_zone : zone;
    s_dock_drag.active   = ( s_dock_drag.zone != DOCK_ZONE_NONE );

    /* Overlay in its own slot on the reserved topmost z-band, clipped to the surface. */
    dock_overlay_begin( vp );

    /* Preview the band the window would take: a full viewport edge for an outer drop, else the half
       (or whole, for center) of the leaf under the cursor. */
    if ( s_dock_drag.zone != DOCK_ZONE_NONE )
    {
        gui_rect_t base = outer ? root->rect : leaf->rect;
        gui_rect_t hr   = dock_zone_region( base, (dock_zone_t)s_dock_drag.zone );
        draw_push_rect_filled ( hr.x, hr.y, hr.w, hr.h, 0, 0, 1, 1, 0, DOCK_OVERLAY_FILL );
        draw_push_rect_outline( hr.x, hr.y, hr.w, hr.h, WIN_BORDER, DOCK_OVERLAY_LINE );
    }

    /* Strip drop: the hot "tab here" chip sits on the tab band itself (the float-strip visual),
       so the gesture reads as "join these tabs" rather than pointing at the center chip. */
    if ( strip_drop )
    {
        f32          sh = leaf->content.y - leaf->rect.y;
        gui_rect_t cr = { leaf->rect.x + leaf->rect.w * 0.5f - s * 0.5f,
                            leaf->rect.y + ( sh - s ) * 0.5f, s, s };
        dock_chip_draw( cr, true );
        dock_chip_tab_glyph( cr );
    }

    /* Per-leaf 5-way -- only where a leaf sits under the cursor (over a gutter it simply drops,
       leaving the viewport edge chips below as the offer).  The center chip drops with a full
       tab list (leaf_can_tab). */
    if ( leaf )
        for ( i32 z = DOCK_ZONE_CENTER; z <= zone_last; ++z )
        {
            if ( z == DOCK_ZONE_CENTER && !leaf_can_tab )
                continue;
            gui_rect_t cr = dock_chip_rect( leaf->rect, (dock_zone_t)z, s, g );
            dock_chip_draw( cr, !outer && (dock_zone_t)z == zone );
            if ( z == DOCK_ZONE_CENTER )
                dock_chip_tab_glyph( cr );
            else
                draw_arrow( cr, dock_zone_dir( (dock_zone_t)z ), COL_TEXT_IDLE );
        }

    /* Edge chips: drawn against the dockspace edges, each pointing outward to read as "full side". */
    if ( has_outer )
        for ( i32 z = DOCK_ZONE_LEFT; z <= DOCK_ZONE_BOTTOM; ++z )
        {
            gui_rect_t cr = dock_outer_chip_rect( root->rect, (dock_zone_t)z, s, margin );
            dock_chip_draw( cr, outer && (dock_zone_t)z == outer_zone );
            draw_arrow( cr, dock_zone_dir( (dock_zone_t)z ), COL_TEXT_IDLE );
        }

    dock_overlay_end();
}

/* Execute the drop computed by dock_drag_detect: tab into the target leaf (center) or split it and
   dock on a side.  Called unconditionally from window_end for every free window; no-ops unless this
   is the dragged window on its release edge (the gate is here because s_dock_drag is unit-private).
   Forward-declared in chrome/gui_chrome.h. */
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
            /* Edge drop: split the whole tree so the new pane spans a full viewport edge -- unless
               a hidden pane already sits on that edge, which the drop revives instead of stacking
               a second (invisible-neighbour) split beside it (dock_hidden_reuse, gui_dock_core.c). */
            gui_dir_t        dir   = dock_zone_dir( (dock_zone_t)s_dock_drag.zone );
            gui_dock_node_t* root  = dock_node_find( s_dock_drag.target );
            gui_dock_node_t* reuse = root ? dock_hidden_reuse( root, dir ) : NULL;
            gui_dock_id_t    side  = reuse ? reuse->id
                                           : gui_dock_split_root( s_dock_drag.viewport, dir, 0.5f );
            if ( side != GUI_DOCK_NONE )
                gui_dock_window( title, side );
        }
        else
        {
            gui_dock_node_t* leaf = dock_node_find( s_dock_drag.target );
            if ( leaf && leaf->split == GUI_DOCK_SPLIT_NONE )
            {
                if ( s_dock_drag.zone == DOCK_ZONE_CENTER )
                {
                    gui_dock_window( title, leaf->id );
                }
                else
                {
                    /* Side drop: if the space this would carve is exactly where a hidden pane sits
                       (its windows stopped emitting and the target absorbed its extent), tab into
                       that pane instead of splitting -- the hidden window later revives as a
                       sibling tab there rather than beside a duplicate pane. */
                    gui_dir_t        dir   = dock_zone_dir( (dock_zone_t)s_dock_drag.zone );
                    gui_dock_node_t* reuse = dock_hidden_reuse( leaf, dir );
                    gui_dock_id_t    side  = reuse ? reuse->id
                                                   : gui_dock_split( leaf->id, dir, 0.5f, NULL );
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

/*==============================================================================================
    Tab reorder -- a tab drag that stays inside the strip band slides the tab through the strip.

    Live reorder: while the drag is in flight the dragged tab follows the cursor by crossing into
    its neighbours (edge-triggered, flicker-guarded for unequal widths -- see dock_strip_reorder),
    immediately (no drop step), and stays the active (visible) tab as it moves.  Leaving the strip band vertically is what undocks instead
    (dock_window_chrome below decides which of the two the cursor position means each frame).
==============================================================================================*/

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
        /* Hidden tabs draw no chip (dock_window_chrome) -- width 0 keeps the trigger points
           aligned with the strip as drawn; the dragged tab slides past them freely. */
        tw[ i ] = dock_tab_seen( node->tabs[ i ] )
                    ? font_text_w( node->names[ i ] ) + 2.0f * WIDGET_PAD
                    : 0.0f;
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

/*==============================================================================================
    Tab-strip chrome -- drawn by the active window's window_end in place of a title bar.

    Runs while s_build holds the active docked window (its id is hover_win when the cursor is over the
    node, and its clip is the node rect), so item_state hit-tests the tabs correctly.  Tabs march
    left-to-right at natural width; clicking one selects it (takes effect next frame, when that window
    becomes the active tab).  A press dragged past the threshold pops the window out (see below).
    Forward-declared in chrome/gui_chrome.h for window_end to call.
==============================================================================================*/

static void
dock_window_chrome( gui_dock_node_t* node )
{
    f32 x  = s_build.win.x;
    f32 y  = s_build.win.y;
    f32 w  = s_build.win.w;
    f32 th = s_build.win.title_h;   /* tab-strip height (= WIN_TITLE_H, clamped for a tiny node) */

    draw_set_rounding( 0.0f );   /* the strip is a flat band behind the tabs */
    draw_push_rect_filled( x, y, w, th, 0, 0, 1, 1, 0, COL_TITLE_IDLE );

    f32 tx = x;
    for ( u32 i = 0; i < node->tab_count; ++i )
    {
        /* A hidden tab (its window stopped emitting -- menu-hidden, X-closed) keeps its
           membership but offers no chip: selecting it would front a window that renders
           nothing.  The chip returns the moment the window re-emits (dock_tab_seen). */
        if ( !dock_tab_seen( node->tabs[ i ] ) )
            continue;

        const char*  nm = node->names[ i ];
        f32          tw = font_text_w( nm ) + 2.0f * WIDGET_PAD;
        gui_rect_t tr = { tx, y, tw, th };
        bool         is_active = ( i == node->active_tab );

        gui_id_t     tid = id_combine( node->id, DOCK_TAB_SALT + i );
        gui_item_state_t st  = item_state( tid, tr, ITEM_BUTTON );

        /* Chip face + ink off the shared tab projection: the current chip takes the body colour
           (joined to the content below), a pressed chip previews that join, the rest stay on the
           title band, lifting under the cursor. */
        u32 bg   = col_tab_bg ( st, is_active );
        u32 tcol = col_tab_ink( st, is_active );
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
            g_ctx->nav.focused_win = node->tabs[ i ];
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

    /* Dockspace maximize (GUI_WIN_DOCK_MAXIMIZE, tree nodes only): a maximize / restore button at
       the strip's right edge -- the docked twin of the floater title-bar pair, gated by the ACTIVE
       tab's flag (this chrome runs from its window_end, so s_build.win.flags is that window's).
       Toggling pins the node over the whole dockspace (dock_max_set; the tween + sibling
       suppression run in dockspace_over_viewport / the route seam).  Double-click on the strip's
       empty band toggles too, the floater title-bar convention -- tabs claim their own hover, so
       the bare-hover gate excludes them. */
    if ( !node->floating && ( s_build.win.flags & GUI_WIN_DOCK_MAXIMIZE ) )
    {
        gui_viewport_t* v     = &s_vp_pool[ node->viewport ];
        bool            maxed = ( v->dock_max_id == node->id && v->dock_max_on );

        gui_rect_t       mx_r  = { x + w - th, y, th, th };
        gui_id_t         mx_id = id_combine( node->id, GUI_MAXIMIZE_SALT );
        gui_item_state_t mx_st = item_state( mx_id, mx_r, ITEM_BUTTON );
        if ( mx_st.hover || mx_st.active )
        {
            draw_set_rounding( ROUND_WIDGET );
            draw_push_rect_filled( mx_r.x, mx_r.y, mx_r.w, mx_r.h, 0, 0, 1, 1, 0, mx_st.active ? COL_BG_ACTIVE : COL_BG_HOT );
        }
        native_btn_draw_glyph( NATIVE_BTN_MAXIMIZE, mx_r, maxed, col_btn_glyph( mx_st ) );

        gui_rect_t band = { tx, y, mx_r.x - tx, th };
        bool band_double = band.w > 1.0f && s_io.mouse_double[ 0 ]
                        && interact_hover_bare( s_build.win.id ) && rect_hit( band );
        if ( mx_st.clicked || band_double )
            dock_max_set( node, !maxed );
    }

    /* Floating group (gui_dock_float.c): the strip's empty band doubles as the group's title-bar
       drag surface -- a press grabs the move (item_state claims the press; move_grab records
       the offsets that keep the grabbed point pinned under the cursor, applied by
       dock_float_resolve's move_track next frame). */
    if ( node->floating )
    {
        gui_rect_t rem = { tx, y, x + w - tx, th };
        if ( rem.w > 1.0f )
        {
            gui_id_t     gid = id_combine( node->id, DOCK_FLOAT_SALT );
            s_scope.nav.skip = true;   /* pure drag surface -- never a keyboard target */
            gui_item_state_t st  = item_state( gid, rem, ITEM_BUTTON );
            if ( st.pressed )
                move_grab( gid, 0, x, y );   /* released globally when the left button lifts */
        }
    }

    /* Border frames the whole node (strip + body), square so adjacent nodes tile flush at right
       angles.  Drawn before the undock handler so it never reads `node` after a drag-out collapses
       an emptied node. */
    draw_set_rounding( 0.0f );
    draw_push_rect_outline( x, y, w, s_build.win.h, WIN_BORDER, COL_BORDER_IDLE );

    /* Keyboard-focus marker: the node's active tab is the window being ended here, so overlay the
       focus border when it is the focused window -- the docked twin of window_end's marker. */
    if ( s_build.win.id == g_ctx->nav.focused_win )
        draw_window_focus_border( ( gui_rect_t ){ x, y, w, s_build.win.h } );

    /* Floating group: bold the hot / grabbed resize edges over the thin border, exactly like a
       free window's window_end does (the hot mask was resolved in dock_float_resolve). */
    if ( node->floating )
    {
        u8 hot_edges = ( s_interaction.active_id == id_combine( node->id, GUI_RESIZE_SALT ) )
                     ? s_resize_edges : s_scope.resize_hot;
        if ( hot_edges )
            draw_resize_highlight( ( gui_rect_t ){ x, y, w, s_build.win.h }, hot_edges );
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
                    gui_vp_t vp = node->viewport;   /* capture before a collapse may free `node` */
                    s_dock_tab_drag.pending = false;

                    dock_undock_by_id( wid );

                    gui_window_t* win = window_find( wid );
                    if ( win )
                    {
                        win->viewport = vp;              /* float on the surface it was docked on */
                        win->z        = surface_z_raise( win->z );   /* raise above the tiles      */
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
