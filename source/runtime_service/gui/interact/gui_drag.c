/*==============================================================================================

    runtime_service/gui/interact/gui_drag.c -- Drag and drop: typed payload transfer between items.

    The ambient drag machine (the ImGui BeginDragDropSource/Target analogue).  One drag exists at
    a time (one mouse), so the whole feature is a single module-static slot: which item the drag
    started from, the typed payload bytes it carries, and the per-frame source/target bracket
    state.  Tier: ambient singular, like s_interaction -- shared across contexts, reset by
    drag_new_frame from frame_begin (gui_frame.c) alongside interaction_frame_reset.

    Sources arm on an item's press and go live once the cursor moves past GUI_DRAG_THRESH -- the
    same click-vs-drag threshold split the dock tab drag uses.  While live, the source item keeps
    active_id (the button is held on it), which freezes normal widget hover everywhere else --
    so drop targets do NOT read hover_id; drag_target_begin hit-tests the cursor against the
    item's clipped rect in the hover window directly.

    drag_track / drag_from_chrome are the internal seams: drag_track is the raw "press point ->
    live drag" threshold machine, and drag_from_chrome starts a drag from non-item chrome (a dock
    tab, gui_dock_drag.c) with the payload attached in the same call.

    Included by gui.c after gui_paint_core.c (draw_drop_ring, the present-tier accept
    highlight); the preview tooltip reuses gui_tooltip_begin/end (gui_popup.c, later in the TU --
    resolved by the public declarations in gui_host.h).

==============================================================================================*/
#include "runtime_service/gui/gui_internal.h"
// clang-format off

#define GUI_DRAG_THRESH 4.0f    /* px an armed press must move before the drag goes live */

static struct
{
    /* Armed: an item was pressed and may become a drag once the cursor moves past the threshold. */
    gui_id_t arm_id;            // item whose press armed a potential drag; 0 = none
    f32      arm_x, arm_y;      // press position the threshold measures from

    /* Live drag + its payload (copied bytes; one slot -- one mouse, one drag). */
    gui_id_t source_id;         // item the live drag started from; 0 = no drag in flight
    bool     has_payload;       // drag_payload_set stamped a type + bytes
    char     type[ GUI_DRAG_TYPE_CAP ];
    u8       data[ GUI_DRAG_PAYLOAD_CAP ];
    u32      size;

    /* Per-frame bracket state. */
    bool       source_open;     // between a true drag_source_begin and drag_source_end
    bool       source_tooltip;  // that begin opened the preview tooltip (end must close it)
    bool       target_open;     // between a true drag_target_begin and drag_target_end
    gui_rect_t target_rect;     // the open target's item rect (accept highlight)
    bool       delivered;       // payload was delivered this drag -- one drop per release

    gui_drag_payload_t view;    // caller-facing view accept/peek return a pointer to

} s_drag;

/* Frame reset, called from frame_begin next to interaction_frame_reset.  The drag (and its
   payload) survives through the release frame -- accept reads it on mouse_released -- and clears
   on the first fully-idle frame after, mirroring how active_id is released. */
static void
drag_new_frame( void )
{
    if ( !s_io.mouse_down[ 0 ] && !s_io.mouse_released[ 0 ] )
        memset( &s_drag, 0, sizeof s_drag );

    s_drag.source_open = false;   /* per-frame brackets never carry across frames */
    s_drag.target_open = false;
}

/* The raw threshold machine: promote (id, press point) to the live drag once the cursor moves
   far enough with the button still down.  Returns true while `id` owns the live drag. */
static bool
drag_track( gui_id_t id, f32 press_x, f32 press_y )
{
    if ( s_drag.source_id == id )
        return true;
    if ( s_drag.source_id != GUI_ID_NONE )
        return false;                      /* another drag owns the mouse */
    if ( !s_io.mouse_down[ 0 ] )
        return false;

    f32 dx = s_io.mouse_x - press_x;
    f32 dy = s_io.mouse_y - press_y;
    if ( dx * dx + dy * dy < GUI_DRAG_THRESH * GUI_DRAG_THRESH )
        return false;

    s_drag.source_id = id;
    return true;
}

/* Start (or continue) a drag from non-item chrome -- a dock tab, a custom grab handle -- that
   never goes through item_state's last-item latch.  The payload is attached on the frame
   the drag goes live so targets can match it immediately.  Returns true while the drag is live. */
static bool
drag_from_chrome( gui_id_t id, f32 press_x, f32 press_y,
                  const char* type, const void* data, u32 size )
{
    if ( !drag_track( id, press_x, press_y ) )
        return false;
    if ( !s_drag.has_payload )
        gui_drag_payload_set( type, data, size );
    return true;
}

/*----------------------------------------------------------------------------------------------
    Public API
----------------------------------------------------------------------------------------------*/

/* Make the item just emitted a drag source.  Arms on the item's press; once the cursor moves past
   the threshold the drag goes live and this returns true every frame until release.  While true,
   call drag_payload_set and emit preview widgets (they land in a cursor-following tooltip unless
   GUI_DRAG_NO_PREVIEW), then close with drag_source_end. */
bool
gui_drag_source_begin( gui_drag_flags_t flags )
{
    gui_id_t id = s_scope.last_id;
    if ( id == GUI_ID_NONE )
        return false;

    /* Arm on the press; the move threshold decides click vs drag (like the dock tab tear-off). */
    if ( s_scope.last_status.pressed && s_drag.source_id == GUI_ID_NONE )
    {
        s_drag.arm_id = id;
        s_drag.arm_x  = s_io.mouse_x;
        s_drag.arm_y  = s_io.mouse_y;
    }

    bool live = ( s_drag.source_id == id );
    if ( !live && s_drag.arm_id == id && s_interaction.active_id == id )
        live = drag_track( id, s_drag.arm_x, s_drag.arm_y );
    if ( !live )
        return false;

    s_drag.source_open    = true;
    s_drag.source_tooltip = !( flags & GUI_DRAG_NO_PREVIEW );
    if ( s_drag.source_tooltip )
    {
        gui_tooltip_begin();
        gui_stack();          /* preview body lays out like any region: open with a stack */
    }
    return true;
}

/* Close a true drag_source_begin (only call when it returned true). */
void
gui_drag_source_end( void )
{
    if ( !s_drag.source_open )
        return;
    if ( s_drag.source_tooltip )
        gui_tooltip_end();
    s_drag.source_open    = false;
    s_drag.source_tooltip = false;
}

/* Stamp the live drag's typed payload.  Bytes are COPIED (cap GUI_DRAG_PAYLOAD_CAP); call every
   frame the source is live, like ImGui -- the copy is cheap and the last write wins.  Returns
   false when no drag is live, the type is empty, or the data does not fit. */
bool
gui_drag_payload_set( const char* type, const void* data, u32 size )
{
    if ( s_drag.source_id == GUI_ID_NONE || !type || !type[ 0 ] )
        return false;
    if ( size > GUI_DRAG_PAYLOAD_CAP )
        return false;

    snprintf( s_drag.type, sizeof s_drag.type, "%s", type );
    if ( data && size )
        memcpy( s_drag.data, data, size );
    s_drag.size        = size;
    s_drag.has_payload = true;
    return true;
}

/* Make the item just emitted a drop target.  True while a payload-carrying drag hovers it --
   hit-tested directly against the item's clipped rect (normal hover is frozen by the source's
   active_id while a drag is in flight).  Bracket with drag_target_end when true. */
bool
gui_drag_target_begin( void )
{
    if ( s_drag.source_id == GUI_ID_NONE || !s_drag.has_payload )
        return false;

    gui_id_t id = s_scope.last_id;
    if ( id == GUI_ID_NONE || id == s_drag.source_id )
        return false;   /* an item never drops onto itself */

    if ( s_scope.win != s_interaction.hover_win )
        return false;
    if ( !rect_hit( s_scope.clip ) || !rect_hit( s_scope.last_rect ) )
        return false;

    s_drag.target_open = true;
    s_drag.target_rect = s_scope.last_rect;
    return true;
}

/* Inside an open target: match the payload's type tag.  On a match the target is highlighted and
   the payload is returned on the release (drop) frame -- or every hover frame with
   GUI_DRAG_ACCEPT_PEEK.  NULL on a type mismatch or before the drop. */
const gui_drag_payload_t*
gui_drag_payload_accept( const char* type, gui_drag_flags_t flags )
{
    if ( !s_drag.target_open || !type )
        return NULL;
    if ( strncmp( type, s_drag.type, GUI_DRAG_TYPE_CAP ) != 0 )
        return NULL;

    /* Type matches: ring the target so the drop reads as "accepted here" (paint policy lives
       with the skin -- draw_drop_ring, present/gui_paint_core.c). */
    if ( !( flags & GUI_DRAG_NO_PREVIEW ) )
        draw_drop_ring( s_drag.target_rect );

    s_drag.view.type = s_drag.type;
    s_drag.view.data = s_drag.data;
    s_drag.view.size = s_drag.size;

    if ( flags & GUI_DRAG_ACCEPT_PEEK )
        return &s_drag.view;

    if ( s_io.mouse_released[ 0 ] && !s_drag.delivered )
    {
        s_drag.delivered = true;   /* one delivery per drag */
        return &s_drag.view;
    }
    return NULL;
}

/* Close a true drag_target_begin (only call when it returned true). */
void
gui_drag_target_end( void )
{
    s_drag.target_open = false;
}

/* True while a payload-carrying drag is in flight anywhere -- for ambient reactions (dim
   non-target panels, change the cursor) outside any source/target bracket. */
bool
gui_drag_active( void )
{
    return s_drag.source_id != GUI_ID_NONE && s_drag.has_payload;
}

/* Inspect the in-flight payload without being a target (NULL when no drag is live).  Read-only:
   accepting still requires a drag_target_begin / drag_payload_accept bracket. */
const gui_drag_payload_t*
gui_drag_payload_peek( void )
{
    if ( !gui_drag_active() )
        return NULL;
    s_drag.view.type = s_drag.type;
    s_drag.view.data = s_drag.data;
    s_drag.view.size = s_drag.size;
    return &s_drag.view;
}

// clang-format on
/*============================================================================================*/
