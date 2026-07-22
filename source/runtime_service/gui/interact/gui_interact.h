#ifndef GUI_INTERACT_H
#define GUI_INTERACT_H
/*==============================================================================================

    runtime_service/gui/interact/gui_interact.h -- gesture mechanisms (the interact unit).

    The record-agnostic interaction elements the chrome recipes ride: edge resize, move-drag
    with deferred press, drag-and-drop, and the feat_* window feature kit.  Mechanisms decide;
    they never paint (gesture feedback paint lives up in element / chrome).  Its own TU
    (root gui_interact.c).  Stack position: after style (each unit .c lists its sub-stack).

    (The item protocol and the anim utilities are the interact SERVER's -- core/gui_core.h.
    Window text selection is NOT here: it reads the render capture and font metrics, so it is
    chrome -- chrome/window/gui_select.c; it claims gestures through interact_claim.)

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

/* The GUI_RESIZE_L/R/T/B edge bits live in gui.h (public: feat_resize's edge mask).  GRIP
   stays internal: the CAN_AUTOSIZE corner triangle -- a resize affordance like the edges,
   carried in the same s_scope.resize_hot mask (the highlight painter ignores it; the R|B
   edge bits are promoted alongside it so the corner still bolds). */
#define GUI_RESIZE_GRIP  ( 1u << 4 )

/* move-drag + deferred-press service (interact/gui_move.c).  move_grab_offset: the raw grab
   offset, for the one consumer re-deriving a position in another coordinate space mid-gesture
   (the tear-off placement, frame/gui_viewport.c). */
void move_grab( gui_id_t id, u8 button, f32 org_x, f32 org_y );
bool move_track( gui_id_t id, f32 cur_x, f32 cur_y, f32* out_x, f32* out_y );
void move_grab_offset( f32* off_x, f32* off_y );
void press_defer_arm( gui_id_t id );
void press_defer_cancel( void );
bool press_defer_crossed( gui_id_t id );

/* the chrome drag source + the frame-driven lifecycle reset (interact/gui_drag.c).
   drag_new_frame: called from frame_begin beside interaction_frame_reset. */
bool drag_from_chrome( gui_id_t id, f32 press_x, f32 press_y, const char* type,
                       const void* data, u32 size );
void drag_new_frame( void );

/*==============================================================================================
    Upward seams -- the unit's documented exceptions, mirroring core's block (gui_core.h).
    Do not add more.
      - draw_drop_ring (element/gui_adornment.c): the ONE adornment paint
        this unit invokes -- the accept ring must land in the same call that decides the
        accept (gui_drag_payload_accept), exactly like core's draw_nav_ring.
      - gui_tooltip_begin/end + gui_stack (chrome, via gui_host.h): the drag preview body.
      - WIN_BORDER (style vocabulary) in the resize bands above: geometry, not paint.
==============================================================================================*/
void draw_drop_ring( gui_rect_t r );

/* the feat_* kit's internals the stock recipe rides (interact/gui_feature.c): the 3-state pin
   core, the collapse liveness peek, and the shared window-feel constants. */
#define FEAT_ANIM_SECS  0.2f
f32  feat_ease( f32 t );
bool feat_pin( gui_id_t id, u32 state, gui_rect_t* r, gui_rect_t* restore, gui_rect_t target );
bool feat_collapse_live( gui_id_t id );

/*==============================================================================================
    Single-line text edit engine (interact/gui_edit.c) -- keyboard-driven buffer editing, the
    measurement-free core behind input_text.  A gesture mechanism like move / resize: it mutates
    a caller buffer + edit state from io, and paints nothing.  The wrapping widget
    (chrome/widgets/gui_text_edit.c) owns font measurement, the mouse handler, horizontal scroll,
    and rendering; it drives the engine through edit_keys() once per focused frame.  The pure
    byte-offset helpers are shared with the multiline wrapper (gui_text_edit_multi.c).
==============================================================================================*/

/* Persisted per-id edit state.  cursor + anchor describe the selection ([min,max), equal = none).
   blink_t (caret-blink dt accumulator) and scroll_x (horizontal pixel bias) are widget-owned
   presentation fields kept here so the whole field state is one keyed slot.  16 bytes -- fits
   within GUI_STATE_CAP. */

typedef struct
{
    f32  blink_t;          // seconds since last caret-visibility reset (widget-owned)
    u16  cursor;           // byte offset of the caret
    u16  anchor;           // passive end of the selection; cursor == anchor -> none
    u16  dbl_lo, dbl_hi;   // double-clicked word bounds (word-drag mode)
    u16  scroll_x;         // horizontal scroll bias in px (widget-owned)
    u8   word_sel;         // nonzero while in a word-select drag
    u8   _pad;

} gui_edit_state_t;

/* edit_keys result: changed on any buffer modification, enter on Enter submit.  Independent. */

typedef struct { bool changed; bool enter; } input_field_result_t;

/* The engine entry: run one focused frame's keyboard editing (hook, cursor-end, undo, selection
   publish, all key commands) over a caller buffer + edit state.  Sets *blink_reset on activity. */

input_field_result_t edit_keys( gui_id_t id, char* buf, u32 bufsz, gui_edit_state_t* es,
                                bool* blink_reset );

/* Pure byte-offset helpers, shared with the single-line + multiline widget wrappers. */

int  char_class    ( u8 c );                                                /* 0 ws / 1 word / 2 sym */
void word_bounds   ( const char* buf, u32 len, u32 off, u32* lo, u32* hi ); /* word run around off   */
u32  word_click_off( const char* buf, u32 len, u32 off );                   /* right-edge click fixup*/
u32  edit_strlen   ( const char* buf, u32 bufsz );                          /* capacity-bounded len  */
void edit_sel      ( const gui_edit_state_t* es, u32* lo, u32* hi, bool* has ); /* selection range */

/* Decentralized memory accounting -- this unit's fixed statics (root gui_interact.c foot),
   summed into cpu_frontend_bytes by gui_ui_memory (gui_ui_mem.c). */
u32 gui_interact_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_INTERACT_H
