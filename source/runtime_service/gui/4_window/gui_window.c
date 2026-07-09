/*==============================================================================================

    runtime_service/gui/4_window/gui_window.c -- Window gesture policy state.

    The window RECORD and its services -- the pool (window_get / window_find), the next-window
    placement channel, the z dispenser, the hover contest, the surface reassignment slot, and
    the open/closed state -- live in 1_surface/gui_surface.c: a window is a placed, stacked,
    occluding rectangle before it is anything else.  This file keeps the gesture POLICY the
    chrome layers over those services: the global drag mode, the tear-off merge-back edge
    latch, and raise-to-front on press (policy because the dock exception -- tiles never
    raise, floating groups raise as a node -- needs 4_dock/ knowledge the surface tier must
    not have).

    Drag / resize / scrollbar interaction lives in gui_widget_window.c alongside
    window_begin / window_end, where the layout dimensions (title-bar height, padding) are in
    scope.

    Included by gui.c after gui_ctx.c so s_interaction (hover_win) and s_io are in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* Drag configuration.  The window currently being dragged is tracked via active_id == window id;
   the in-flight grab offset lives with the move-drag service (2_interact/gui_move.c) -- windows
   grab through move_grab and follow the cursor through move_track. */

static gui_win_drag_t     s_win_drag_mode = GUI_WIN_DRAG_TITLEBAR;

/* Merge-back edge latch.  A floater merges back into the main surface only on a genuine ENTER --
   the cursor crossing from outside the main window into it.  Without this, a floater spawned over
   its parent (cursor already inside the main rect) would merge back the instant it is grabbed,
   making it impossible to drag away.  s_vp_drag_id marks which window the latch belongs to so a
   fresh drag re-arms it; s_vp_merge_armed turns true once the cursor has been clear of the main
   window during this drag, gating the merge until then. */
static gui_id_t           s_vp_drag_id = GUI_ID_NONE;
static bool                 s_vp_merge_armed;

/* Click-vs-drag disambiguation for the native title bar and the CAN_AUTOSIZE corner grip --
   both own a double-click, so committing on press-1 would swallow press-2 before mouse_double
   can be tested.  The threshold latch is the deferred-press service (2_interact/gui_move.c):
   the press site arms press_defer_arm and its window_end polls press_defer_crossed each frame. */

/* In-flight edge resize.  The window being resized holds active_id == (id ^ RESIZE_SALT).
   The s_resize_* in-flight state (edges, offsets, far-edge pins) is owned by the shared
   resize mechanism in 2_interact/gui_resize.c -- a resizeable child_begin (gui_layout_child.c)
   reads it too, so it cannot live here; only the window-record apply / grab / fit stay in
   gui_widget_window.c. */

/*----------------------------------------------------------------------------------------------
    window_raise_on_press -- a press brings the window under the cursor to the front.

    hover_win (the window the cursor is over) was resolved last frame, so this runs at the
    top of the frame -- before any window_begin stamps its z -- and the raise therefore
    takes effect this same frame: clicking a window's exposed area brings it up at once.
    Called from gui_ctx_begin() right after ctx_new_frame() promotes hover_win.
----------------------------------------------------------------------------------------------*/

static void
window_raise_on_press( void )
{
    /* Either button raises: left for the normal click/drag, middle for the convenience move
       grab (gui_widget.c window_end), so a middle-grabbed window also comes to the front. */
    if ( ( !s_io.mouse_pressed[ 0 ] && !s_io.mouse_pressed[ 2 ] )
         || s_interaction.hover_win == GUI_ID_NONE )
        return;

    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == s_interaction.hover_win )
        {
            /* A frame-only native shell (GUI_WIN_NATIVE) is the borderless viewport's backdrop
               frame: it must stay behind the windows living inside it, so it never raises. */
            if ( s_windows[ i ].flags & GUI_WIN_NATIVE )
                break;

            /* A docked window is tiled by its node, not stacked: it draws in a low z band behind the
               free-floating windows and a click must never reorder the tile.  Leave its z untouched.
               A FLOATING tab group stacks like a free window, though -- raise the whole group (its
               node carries the z all its tabs draw at). */
            {
                gui_dock_node_t* dn = dock_find_window_node( s_windows[ i ].id );
                if ( dn )
                {
                    if ( dn->floating )
                        dn->z = surface_z_raise( dn->z );
                    break;
                }
            }

            s_windows[ i ].z = surface_z_raise( s_windows[ i ].z );
            break;
        }
}

/*----------------------------------------------------------------------------------------------
    window_set_drag -- select the global drag mode; call between frames.
----------------------------------------------------------------------------------------------*/

void
gui_window_set_drag( gui_win_drag_t mode )
{
    s_win_drag_mode = mode;
}

// clang-format on
/*============================================================================================*/
