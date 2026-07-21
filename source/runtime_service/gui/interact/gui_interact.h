#ifndef GUI_INTERACT_H
#define GUI_INTERACT_H
/*==============================================================================================

    runtime_service/gui/interact/gui_interact.h -- gesture mechanisms (the interact unit).

    The record-agnostic interaction elements the chrome recipes ride: edge resize, move-drag
    with deferred press, the chrome drag source, window text selection, and the feat_* window
    feature kit.  Mechanisms decide; they never paint (gesture feedback paint lives up in
    element / chrome).  Included by gui_internal.h after core/gui_core.h.

    (The item protocol and the anim utilities are the interact SERVER's -- core/gui_core.h.)

==============================================================================================*/

// clang-format off

/* edge-resize service (interact/gui_resize.c) -- the record-agnostic mechanism the resizeable
   child (flow) and the window / dock chrome ride (chrome interrogates the same gesture id and
   outer band). */
#define GUI_RESIZE_SALT    0x5152E001u
#define RESIZE_BAND_INNER  ( 4.0f )                  /* reach inside the border  */
#define RESIZE_BAND_OUTER  ( WIN_BORDER + 6.0f )     /* and just outside it      */
u8   resize_item( gui_id_t id, gui_id_t owner_win, gui_rect_t box, u8 allow, bool pin_v,
                  bool* dragging );
void resize_apply_edges( gui_rect_t* r, u8 edges );
void resize_grab( gui_id_t id, gui_rect_t box, u8 edges );
extern u8  s_resize_edges;                 /* in-flight edges (GUI_RESIZE_* bits)              */
extern f32 s_resize_off_x, s_resize_off_y; /* grab offsets keeping the edge under the cursor   */
extern f32 s_resize_fix_x, s_resize_fix_y; /* pinned far edges for a left / top drag           */

/* The GUI_RESIZE_L/R/T/B edge bits moved to gui.h (public: feat_resize's edge mask).  GRIP
   stays internal: the CAN_AUTOSIZE corner triangle -- a resize affordance like the edges,
   carried in the same s_scope.resize_hot mask (the highlight painter ignores it; the R|B
   edge bits are promoted alongside it so the corner still bolds). */
#define GUI_RESIZE_GRIP  ( 1u << 4 )

/* move-drag + deferred-press service (interact/gui_move.c). */
void move_grab( gui_id_t id, u8 button, f32 org_x, f32 org_y );
bool move_track( gui_id_t id, f32 cur_x, f32 cur_y, f32* out_x, f32* out_y );
void press_defer_arm( gui_id_t id );
void press_defer_cancel( void );
bool press_defer_crossed( gui_id_t id );

/* the chrome drag source (interact/gui_drag.c). */
bool drag_from_chrome( gui_id_t id, f32 press_x, f32 press_y, const char* type,
                       const void* data, u32 size );

/* window text selection (interact/gui_select.c) -- painted under the body, resolved at end. */
void select_paint_under( void );
void select_window_end( void );

/* the feat_* kit's internals the stock recipe rides (interact/gui_feature.c): the 3-state pin
   core, the collapse liveness peek, and the shared window-feel constants. */
#define FEAT_ANIM_SECS  0.2f
f32  feat_ease( f32 t );
bool feat_pin( gui_id_t id, u32 state, gui_rect_t* r, gui_rect_t* restore, gui_rect_t target );
bool feat_collapse_live( gui_id_t id );

// clang-format on
/*============================================================================================*/
#endif    // GUI_INTERACT_H
