/*==============================================================================================

    runtime_service/gui/interact/gui_move.c -- Move-drag and deferred-press protocols.

    Two small behavior services shared by everything that drags a rect around by a handle
    (window title bars, floating dock-group strips, an undocked tab continuing as a window):

    Move drag: move_grab claims the interaction (active_id + button) and records the cursor's
    offset against the moving rect's origin; move_track maps a cursor back through that offset
    while the drag lives, so the grabbed point stays pinned under the cursor.  The cursor is a
    PARAMETER: a panel tracks the client cursor (s_io.mouse_x/y), a floater the screen cursor --
    the offset is origin-relative either way.  One drag at a time, so one offset record; which
    id owns it is exactly active_id, the same arbitration every widget uses.

    Deferred press: click-vs-drag disambiguation for chrome that also owns a double-click (a
    native title bar, the autosize grip).  Claiming the drag on press-1 would set active_id and
    swallow press-2 before mouse_double can be tested, so the press is only armed here, and
    press_defer_crossed reports true exactly once -- when the cursor crosses the drag threshold
    with the button still down.  A release before that silently cancels: it was a click, and
    the hit rect stayed put for press-2.

    Included by gui_interact.c (the interact unit); consumers are the chrome hosts
    (gui_window_free.c, gui_dock_drag.c, gui_dock_float.c) and the frame conductor's
    tear-off placement (frame/gui_viewport.c, via move_grab_offset).

==============================================================================================*/
// clang-format off

/* In-flight move-drag grab offset (cursor - origin at grab time).  Owner is active_id. */
static f32  s_move_off_x, s_move_off_y;

/* Claim the interaction for a move drag of the rect at (org_x, org_y): active_id = id, held by
   `button` (0 left, 2 middle -- released globally when that button lifts), offset recorded so
   move_track keeps the grabbed point under the cursor. */
void
move_grab( gui_id_t id, u8 button, f32 org_x, f32 org_y )
{
    s_interaction.active_id     = id;
    s_interaction.active_button = button;
    s_move_off_x = s_io.mouse_x - org_x;
    s_move_off_y = s_io.mouse_y - org_y;
}

/* Map a cursor through the recorded grab offset while `id` holds the drag: true with the new
   origin in *out while dragging, false (outputs untouched) otherwise.  Pass s_io.mouse_x/y to
   track in client space, the screen cursor to move an OS window. */
bool
move_track( gui_id_t id, f32 cur_x, f32 cur_y, f32* out_x, f32* out_y )
{
    if ( s_interaction.active_id != id )
        return false;
    *out_x = cur_x - s_move_off_x;
    *out_y = cur_y - s_move_off_y;
    return true;
}

/* The raw grab offset of the in-flight move drag.  For the one consumer that must re-derive a
   position in another coordinate space mid-gesture: the tear-off / merge-back placement in
   frame/gui_viewport.c (cross-unit) lands the surface at (cursor - offset) using the SCREEN
   or the new host's cursor. */
void
move_grab_offset( f32* off_x, f32* off_y )
{
    *off_x = s_move_off_x;
    *off_y = s_move_off_y;
}

/*==============================================================================================
    Deferred press -- click vs drag threshold latch.
==============================================================================================*/

/* How far the cursor must travel with the button down before an armed press commits to a drag. */
#define PRESS_DEFER_THRESH  4.0f

static bool     s_press_defer_pending;
static gui_id_t s_press_defer_id;
static f32      s_press_defer_px, s_press_defer_py;

/* Arm a pending press for `id` at the current cursor.  One latch: only one press exists per
   frame, so arming replaces any stale pending gesture. */
void
press_defer_arm( gui_id_t id )
{
    s_press_defer_pending = true;
    s_press_defer_id      = id;
    s_press_defer_px      = s_io.mouse_x;
    s_press_defer_py      = s_io.mouse_y;
}

/* Drop the pending press without committing (e.g. the second click arrived and the gesture
   resolved as a double-click instead). */
void
press_defer_cancel( void )
{
    s_press_defer_pending = false;
}

/* Poll the pending press for `id` -- call every frame from the arming site, outside its hover
   gate, so sliding off the small hit rect mid-press cannot strand the gesture.  True exactly
   once, when the cursor crosses the threshold with the button still down (commit the drag);
   a release before that clears the latch silently (it was a click). */
bool
press_defer_crossed( gui_id_t id )
{
    if ( !s_press_defer_pending || s_press_defer_id != id )
        return false;
    if ( !s_io.mouse_down[ 0 ] )
    {
        s_press_defer_pending = false;   /* released without dragging -- was a click */
        return false;
    }
    f32 dx = s_io.mouse_x - s_press_defer_px;
    f32 dy = s_io.mouse_y - s_press_defer_py;
    if ( ( dx * dx + dy * dy ) < PRESS_DEFER_THRESH * PRESS_DEFER_THRESH )
        return false;
    s_press_defer_pending = false;
    return true;
}

// clang-format on
/*============================================================================================*/
