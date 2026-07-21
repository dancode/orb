#ifndef GUI_FLOW_H
#define GUI_FLOW_H
/*==============================================================================================

    runtime_service/gui/flow/gui_flow.h -- layout composition (the flow unit).

    The rect PRODUCER: metrics in, rects out.  Owns the layout-frame types, the region
    lifecycle, and the cell emitters every widget and chrome file composes over.  Included by
    gui_internal.h after interact/gui_interact.h; the layers above (element, chrome) consume
    the rects flow carves.

    Downward, flow reads the ambient records + core services; its only two upward calls are
    scrollbar_widget (declared in chrome/gui_chrome.h) and the gui_anim_* ease
    (core/gui_core.h).  Implementation: compose/*.c today, flow/ after R7.

==============================================================================================*/

// clang-format off

#define GUI_LAYOUT_DEPTH            8       // max nested scroll regions (windows or children)

/*==============================================================================================
    Scroll link (compose/gui_scroll.c)

    Persistent scroll offset + last-measured content extent for one scrollable region.
    layout_push_region biases the pen by -scroll and writes content_w/content_h back at pop; the
    owner (gui_window_t, gui_region_t, gui_table_persist_t) holds one by value so layout_frame_t
    can reach it through a single pointer instead of four.
==============================================================================================*/

typedef struct
{
    f32 scroll_x, scroll_y;    // persisted scroll offset; 0 = top-left
    f32 content_w, content_h;  // content extent measured last frame

    /* Bottom-anchor tail-follow (GUI_WIN_ANCHOR_BOTTOM only): pinned_y is the scroll_y layout_push_region
       last left the region at, so a later external move (wheel / bar / scroll_by) is detectable; unstick
       latches once the user scrolls off the bottom and clears when they return to it.  Zero on both = the
       default "follow the tail" state a fresh region opens in. */
    f32  pinned_y;
    bool unstick;

} gui_scroll_link_t;

/*==============================================================================================
    Layout-frame (stack storage in core/gui_ctx.c)

    Every scrollable region (a window body or a child_begin box) pushes one frame.  The top frame
    owns the layout pen and the content column the leaf widgets emit into; the rest is the resolve
    context layout_pop_region needs to measure content and draw the region's scrollbars.
==============================================================================================*/

/* The three grouped lifetimes of a layout frame, named so each reset in gui_layout_core.c is a
   single struct assignment that cannot drift from the field list:

     layout_tmpl_t -- the installed shape; persists until the next header replaces it
     layout_mod_t  -- orthogonal modifiers; persist across installs, reset only by the full
                      clears (layout_clear / layout_set_default via layout_modifiers_reset)
     layout_line_t -- the iteration cursor + open-line record; re-zeroed by every install
                      (layout_template_reset) */

/* Active row template (the row / cols headers).  Persists and repeats: each widget fills the
   next cell, wrapping to a fresh row of the same shape when the columns run out.  See
   gui_layout_t in gui.h for the unit rule.  The resolved cell geometry is computed once when a
   template is installed (the source track list is kept only so indent can re-resolve): flow uses
   cellx/cellw for every row; grid uses cellx/cellw x rowy/rowh as the fixed matrix.  cols
   indexes [0,ncols), rows [0,nrows). */

typedef struct
{
    u32 ncols;                      // column count
    u32 nrows;                      // row count; 0 => flow mode, else grid
    f32 row_h;                      // flow row height: 0 = auto, >0 = pixels
    u32 seq;                        // install ordinal within the region -- keys the natural-track measures
    u8  nat_mask;                   // bit per column: a natural (0) track resolved from last frame's measure
    f32 cols[ GUI_LAYOUT_COLS ];    // source column units, kept so indent can re-resolve

    f32 cellx[ GUI_LAYOUT_COLS ];   // resolved cell left edges
    f32 cellw[ GUI_LAYOUT_COLS ];   // resolved cell widths
    f32 rowy [ GUI_LAYOUT_COLS ];   // resolved cell tops    (grid only)
    f32 rowh [ GUI_LAYOUT_COLS ];   // resolved cell heights (grid only)

} layout_tmpl_t;

/* Orthogonal modifiers -- gaps, alignment, field split.  align (gui_align_t flags) is where a
   widget's natural-sized content sits in its cell; 0 = LEFT | TOP.  The field split
   (field_split / field_label_left) makes a labeled value widget split its cell into a label
   track + a control track, resolved with the column unit rule. */

typedef struct
{
    f32 gap_x, gap_y;               // inter-cell spacing request; 0 = live theme default (mod_gap_x/_y)
    u8  align;                      // gui_align_t flags
    u8  field_side;                 // gui_label_side_t: 0 off (label trails), 1 left, 2 right
    f32 field_label;                // label track size   (overloaded unit)
    f32 field_control;              // control track size (overloaded unit)

} layout_mod_t;

/* The iteration cursor + the open line -- the one record behind flow rows, pack lines, and
   same_line continuations.  A flow row fixes ext when it opens (row_h, or the first item's
   height) and places items at the template cells; pack and continuations place at the running
   main pen and grow ext by max.  line_commit folds the line into pen_y.  In a strip (vertical
   pack) the axes flip: cross / ext are x / width, main is the y pen.

   fit_next (next_item_fit) is a one-shot overloaded unit that decides how big the very next cell
   item is *before* mod.align decides where it sits -- the explicit escape hatch for the implicit
   per-widget fit signal every emit carries in its own natural_w.  < 0 = unset (the common case):
   the widget's own natural_w wins, matching its type's default (a button hugs its label, a
   slider fills).  See cell_fit_resolve in gui_layout_core.c. */

typedef struct
{
    u32 col;                        // next column to emit (0 = at a row start)
    u32 row;                        // current grid row (with col, walks row-major)

    f32  cross;                     // cross-axis origin of the current / last line
    f32  ext;                       // its cross extent (fixed for a flow row, max for pack)
    f32  main;                      // running main-axis pen (past the last item + gap)
    f32  origin;                    // main-axis line start (the pack_nextline reset)
    bool open;                      // items may still join this line; commit closes it

    gui_rect_t prev_item;           // last cell emitted this region: same_line() reopens its
    bool cont_pending;              //   line, and the next emit is a one-shot pen placement

    u8  pack_dir;                   // gui_pack_dir_t: 0 horizontal (bar), 1 vertical (strip)
    f32 pack_size_next;             // pending main-axis size unit; < 0 = unset (natural)
    f32 fit_next;                   // pending cell-item fit unit; < 0 = unset (implicit)
    f32 h_next;                     // pending one-shot item-height unit; < 0 = unset (caller's h)

    /* One-shot next_item_align: the verb swaps the override into mod.align (so the item's own
       paint reads it too) and arms the next emit; the emit AFTER that one restores the base. */
    u8   align_restore;             // mod.align to restore once the armed item has emitted
    bool align_swap;                // an override sits in mod.align (restore pending)
    bool align_armed;               // ... and its item has not emitted yet

    bool wrap;                      // pack: auto-break the line when the next item overruns it

} layout_line_t;

typedef struct
{
    /* COORDINATE SPACES.  Every scalar below lives in exactly one of two spaces, and mixing
       them in one formula is a bug (it splices the scroll offset into the result):
         CANVAS -- scroll-biased screen coordinates: the content's position after the
                   -scroll bias, sliding under the view as the region scrolls.  The pen,
                   content column, and highwater are canvas values; so is every cell rect
                   handed to a widget.
         SCREEN -- fixed to the glass: outer, view, origin_*, band_bottom, parent_clip.
       A width/height derived canvas-from-canvas (content_avail) or screen-from-screen
       (view.w - pads) is scroll-free; anchoring a canvas point against a screen edge bakes
       the live scroll into the number -- the multiline box once grew wider as its window
       scrolled for exactly this reason.  When a rule needs both spaces, convert explicitly
       through scroll->scroll_x/y. */

    /* The PEN (content_x, pen_y; x has no independent motion -- a line always starts at
       content_x) is where the next item goes; the HIGHWATER (high_x, high_y) is the monotonic
       bounding-box max the region measures at pop to size its scrollbars / autosize.  Forward
       flow advances both together (content_reach); a pen REPOSITION -- layout_pen_jump for a
       table row or a menu-bar restore -- moves pen_y alone, so the highwater never rewinds.
       extent_track grows the highwater; cell_reach is its x-only face for a leaf widget
       that overflows its cell.  pen_y is carried live at the exact content end (committed lines
       plus the open line) -- a gap is owed *before* the next line (gap_pending), never appended
       after content, so measurement at pop needs no trailing-gap correction.  gap_pending is the
       one flow fact that survives a template install: content committed above still owes its gap
       to whatever shape comes next. */

    f32  content_x;         // CANVAS: content column left edge; lines start here
    f32  content_w;         // width of the column (can exceed the view when content overflowed)
    f32  pen_y;             // CANVAS: pen -- y the next line opens at
    f32  high_x, high_y;    // CANVAS: highwater -- far corner the content reached
    f32  band_bottom;       // SCREEN: bottom of the content area (view bottom - pad.b) -- grid band end
    bool gap_pending;       // content committed above -- the next line owes a gap
    f32  anchor_bias;       // GUI_WIN_ANCHOR_BOTTOM: px the content block was dropped to bottom-justify
                            //   it (0 unless underfilled); pop subtracts it so the measure stays true

    gui_layout_mode_t mode; // declared next-item methodology; NONE until a header

    layout_tmpl_t tmpl;     // the installed shape (persists until replaced)
    layout_mod_t  mod;      // orthogonal modifiers (persist across installs)
    layout_line_t line;     // iteration cursor + the open line (re-zeroed per install)
    u32           tmpl_seq; // install-ordinal dispenser (tmpl.seq source; 0 each region open)

    /* Keyboard-nav structural coordinate (see gui_nav_item_t).  nav_region is dispensed once per
       region open (layout_seed_content); nav_line is re-dispensed from the frame-global counter
       every time a line opens -- a flow row, a pack line, a grid row, a pen jump -- so every
       placed item carries "which row of which container" with no per-widget code. */

    u32             nav_region;         // this region's sequence number (frame-global dispenser)
    u32             nav_line;           // line sequence the next placement stamps
    bool            nav_line_pin;       // an imperative host (table) owns nav_line: opens reuse it

    /* Resolve context, set at push and read at pop. */

    gui_id_t        region_id;          // base id for the region's scrollbar widget ids
    gui_win_flags_t flags;              // scroll policy bits (GUI_WIN_*SCROLL), reused
    gui_rect_t      outer;              // SCREEN: the region box
    gui_pad_t       pad;                // seed inset (space-free width) -- joins the measured canvas at pop

    /* THE visible view (SCREEN): outer inset by the border, minus the reserved scrollbar
       gutters.  Computed ONCE at push (layout_push_region / sublayout_open) and read
       everywhere a "what can the user see / hit" rect is needed -- the draw clip, the
       interaction clip, the content-track derivation (layout_seed_content), the scrollbar
       tracks (which sit exactly on its right / bottom edges), and the nav scroll chase.
       Never re-derive these extents from outer; drift between derivations is how content ends
       up interacting under a scrollbar. */
    gui_rect_t      view;

    f32             origin_x;           // SCREEN: unscrolled content origin (canvas position at
    f32             origin_y;           //   scroll 0) -- pop measures content extent against it
    f32             sb_w, sb_h;         // reserved gutter sizes (0 = no bar this frame)
    bool            show_v, show_h;     // a bar is shown this axis
    bool            pushed_clip;        // a draw clip was pushed (balance at pop)

    /* Persistent scroll state, owned by the caller (window record or region pool entry); scroll
       biases the pen at push, content_w/content_h are written back at pop for next frame. */

    gui_scroll_link_t* scroll;

    gui_rect_t      parent_clip;        // SCREEN: s_scope.clip to restore at pop
    u32             id_restore;         // id-scope depth to restore at pop (see id stack below)

    /* Child edge-resize (child_begin CHILD_RESIZE_*): the armed/hot edges of this child's border
       and the s_scope.resize_hot to restore at child_end.  child_begin sets both (0 for a
       non-resizeable child); child_end bolds child_resize_edge and restores the saved hot, so a
       hot edge suppresses body widgets only while inside this child, never its siblings. */

    u8              child_resize_edge;       // hot/armed resize edges for this child (0 = none)
    u8              child_resize_saved_hot;  // s_scope.resize_hot to restore at child_end

} layout_frame_t;

extern layout_frame_t s_layout_stack[ GUI_LAYOUT_DEPTH ];  /* core/gui_ctx.c -- region stack */
extern u32            s_layout_sp;  /* active frame count; top = s_layout_sp - 1 */

layout_frame_t* lf( void );         /* core/gui_ctx.c -- top layout frame (clamped, never NULL) */

/*==============================================================================================
    Persistent region state (compose/gui_scroll.c)

    A child_begin region's scroll offset and last-measured content size, kept across frames in the
    keyed state pool (gui_ctx.c), keyed by region id.  Windows keep these inline in gui_window_t.
==============================================================================================*/

typedef struct
{
    // persisted scroll offset (fractional: scrollbar drag is t * max_scroll)
    // + content extent measured last frame (gui_scroll_link_t* passed to
    // layout_push_region)
    gui_scroll_link_t scroll;

    // user-resized size in pixels; 0 = none, use the passed / auto size.  f32 (not i16) so a
    // programmatic resize can ease sub-pixel through the size_animate animation seam.
    f32 user_w, user_h;

} gui_region_t;

/* Persistent heights for one split panel pair, stored in the keyed state pool.
   left_h / right_h are the content heights measured last frame so the current
   frame can pre-allocate the correct rects before any widgets emit. */
typedef struct { f32 left_h; f32 right_h; } gui_split_entry_t;

/*==============================================================================================
    Composition's emit surface -- the cell emitters and region lifecycle
==============================================================================================*/

gui_rect_t cell_next_w( f32 natural_w, f32 h );    /* THE universal emit seam                 */
gui_rect_t cell_next  ( f32 h );                   /* fill the track cell                     */
void       cell_reach ( f32 right_x );             /* stretch the content high-water mark     */

/* Split a cell into control + trailing/field label geometry -- the seam every "control +
   label" widget routes through; its painting companion (draw_field_label) is a draw routine. */
bool cell_split_field( gui_rect_t cell, f32 min_control_w, f32* out_label_x,
                       f32* out_label_w, gui_rect_t* out_control );

void extent_track   ( layout_frame_t* f, f32 x, f32 y );
f32  layout_next_y  ( layout_frame_t* f );
void layout_pen_jump( layout_frame_t* f, f32 y );
void layout_row_break( layout_frame_t* f );
void layout_set_default( layout_frame_t* f );
void layout_resolve_tracks( const f32* tracks, u32 n, f32 origin, f32 extent, f32 gap,
                            f32* out_pos, f32* out_size );

void layout_push_region( gui_id_t id, gui_rect_t outer, gui_pad_t region_pad,
                         gui_win_flags_t flags, gui_scroll_link_t* scroll, bool own_clip );
void layout_pop_region ( void );

/* Default region padding (the inset every window body / child opens with) -- the window
   chrome opens its body region with it. */
#define REGION_PAD_DEFAULT ( ( gui_pad_t ){ WIDGET_PAD, WIDGET_PAD, WIDGET_GAP, WIDGET_GAP } )

u32 gui_flow_unit_mem_bytes( void );               /* the flow unit's fixed statics           */

// clang-format on
/*============================================================================================*/
#endif    // GUI_FLOW_H
