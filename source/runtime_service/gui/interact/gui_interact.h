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
      - draw_drop_ring (stock/gui_adornment.c): the ONE adornment paint
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
    Single-line text edit engine (interact/gui_edit.c) -- the whole behavior of a text field
    behind input_text, minus the paint.  A gesture mechanism like move / resize: it mutates a
    caller buffer + edit state from io and paints nothing, but it MEASURES (glyph advances are
    math over the font/ resource, not drawing) so it owns the caret placement, the click-to-caret
    mapping, the mouse selection drag, and horizontal scroll as well as the keyboard path.  The
    one entry is edit_field(): the wrapping widget (chrome/widgets/gui_text_edit.c) supplies a
    content rect + item state, calls it once per frame, and then only paints the resolved state.
    The measurement + pure byte-offset helpers are shared with the multiline wrapper
    (gui_text_edit_multi.c).
==============================================================================================*/

/* Persisted per-id edit state.  cursor + anchor describe the selection ([min,max), equal = none).
   blink_t (caret-blink dt accumulator) and pan_x (horizontal pixel bias) are engine-owned
   presentation fields kept here so the whole field state is one keyed slot.  16 bytes -- fits
   within GUI_STATE_CAP.

   pan_x, NOT scroll_x: this is a field-internal pan in the text's own content space, unrelated to
   the REGION scroll of flow (f->scroll->scroll_x) and its canvas/screen anchors.  The two were
   named alike and read alike, and a widget that "corrected" one with the other would be wrong
   twice. */

typedef struct
{
    f32  blink_t;          // seconds since last caret-visibility reset (engine-owned)
    u16  cursor;           // byte offset of the caret
    u16  anchor;           // passive end of the selection; cursor == anchor -> none
    u16  dbl_lo, dbl_hi;   // double-clicked word bounds (word-drag mode)
    u16  pan_x;            // horizontal pan in px, text content space (engine-owned)
    u8   word_sel;         // nonzero while in a word-select drag
    u8   _pad;

} gui_edit_state_t;

/* edit_field result: changed on any buffer modification, enter on Enter submit.  Independent. */

typedef struct { bool changed; bool enter; } input_field_result_t;

/* The engine entry: run one full non-paint frame of a single-line field over a caller buffer.
   `content` is the text-area rect (the widget's box already inset); `st` is the item state.  The
   engine fetches its own keyed edit state by id, runs keys + mouse + pan + blink, and leaves
   cursor / anchor / pan_x / blink_t on that slot for the widget to paint. */

input_field_result_t edit_field( gui_id_t id, gui_rect_t content, gui_item_state_t st,
                                 char* buf, u32 bufsz );

/* Measurement + pure byte-offset helpers, shared with the single-line + multiline wrappers.
   text_x_at / text_offset_at are math over the active font's advances -- the interact-side
   hit-test the widgets reuse to paint the caret and selection. */

f32  text_x_at     ( const char* buf, u32 off );                           /* caret px at byte off  */
u32  text_offset_at( const char* buf, u32 len, f32 px );                    /* byte off nearest px   */
int  char_class    ( u8 c );                                                /* 0 ws / 1 word / 2 sym */
void word_bounds   ( const char* buf, u32 len, u32 off, u32* lo, u32* hi ); /* word run around off   */
u32  word_click_off( const char* buf, u32 len, u32 off );                   /* right-edge click fixup*/
u32  edit_strlen   ( const char* buf, u32 bufsz );                          /* capacity-bounded len  */
void edit_sel      ( const gui_edit_state_t* es, u32* lo, u32* hi, bool* has ); /* selection range */

/*==============================================================================================
    Multi-line text edit engine (interact/gui_edit_multi.c) -- the two-dimensional twin of the
    single-line engine, the field-internal core behind input_text_multiline.  Same shape as
    gui_edit.c: it mutates a caller buffer + editor state from io, measures (glyph + line
    geometry), pans the caret horizontally, and paints nothing.  The one entry is medit_edit().
    What it does NOT own is the VERTICAL scroll: that belongs to the enclosing child region
    (flow, above this layer), so the wrapping widget (chrome/widgets/gui_text_edit_multi.c)
    chases the caret's row by writing the region scroll and paints; medit_edit returns `active`
    to tell it when.  The line-geometry readers below are shared with that widget.
==============================================================================================*/

/* Persisted per-id editor state (big-class keyed slot).  cursor / anchor are byte offsets with
   the single-line selection contract ([min,max), equal = bare caret).  pan_x is the horizontal
   pan chasing the caret (vertical scroll belongs to the child region, not this state).  pref_x is
   the sticky preferred column for vertical caret movement (the x the caret aims for when Up / Down
   crosses a shorter line); pref_valid gates it because 0.0 is a real column.  36 bytes. */

typedef struct
{
    f32  blink_t;      // seconds since last caret-visibility reset
    u32  cursor;       // byte offset of the caret
    u32  anchor;       // passive end of the selection; cursor == anchor -> none
    u32  dbl_lo;       // word start of the double-clicked word (word-drag mode)
    u32  dbl_hi;       // word end of the double-clicked word  (word-drag mode)
    f32  pan_x;        // horizontal pan in px, text content space (caret chase)
    f32  pref_x;       // preferred caret column (pixels) for vertical movement
    u8   word_sel;     // nonzero while in word-select drag (set by double-click)
    u8   pref_valid;   // pref_x holds a live column (0.0 is a real column, so a flag)
    u8   _pad[ 2 ];

} gui_medit_state_t;

/* medit_edit result: changed on any buffer modification; active on any caret / edit activity this
   frame (the signal for the widget's vertical region-scroll chase and its own caret repaint). */

typedef struct { bool changed; bool active; } medit_result_t;

/* The engine entry: run one full field-internal frame of a multiline editor over a caller buffer.
   `inner` is the text content rect (the widget's cell already inset), `st` the item state,
   `vis_rows` the page-scroll size, `line_h` the row pitch.  The engine fetches its own keyed
   editor state by id and leaves cursor / anchor / pan_x / blink on it for the widget. */

medit_result_t medit_edit( gui_id_t id, gui_rect_t inner, gui_item_state_t st, u32 vis_rows,
                           f32 line_h, char* buf, u32 bufsz );

/* Line geometry + selection readers, shared with the widget's vertical chase + paint. */

void medit_sel        ( const gui_medit_state_t* es, u32* lo, u32* hi, bool* has ); /* selection  */
u32  medit_line_count ( const char* buf, u32 len );                    /* '\n' count + 1          */
u32  medit_line_end   ( const char* buf, u32 len, u32 off );           /* '\n' offset, or len     */
u32  medit_row_start  ( const char* buf, u32 len, u32 row );           /* start offset of `row`   */
void medit_caret_rowx ( const char* buf, u32 off, u32* row, f32* x );  /* (row, px) of the caret  */

/* Decentralized memory accounting -- this unit's fixed statics (root gui_interact.c foot),
   summed into cpu_frontend_bytes by gui_ui_memory (gui_ui_mem.c). */
u32 gui_interact_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_INTERACT_H
