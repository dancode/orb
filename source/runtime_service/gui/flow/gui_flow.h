#ifndef GUI_FLOW_H
#define GUI_FLOW_H
/*==============================================================================================

    runtime_service/gui/flow/gui_flow.h -- layout composition (the flow unit).

    The rect PRODUCER: metrics in, rects out.  Owns the layout-frame types, the region
    lifecycle, and the cell emitters every widget and chrome file composes over.  Included by
    each unit .c after interact/gui_interact.h; the layers above (element, chrome) consume
    the rects flow carves.  Its own unit (root gui_flow.c).

    Downward, flow reads the ambient records + the core services (the anim ease is core),
    the style metrics, interact's edge-resize mechanism, and the render clip
    stack (flow computes the view rect, so it owns the region scissor -- see the root
    banner).  The upward seams are enumerated at the bottom of this header; do not add more.

==============================================================================================*/

// clang-format off

#define GUI_LAYOUT_DEPTH            8       // max nested scroll regions (windows or children)

/* gui_scroll_link_t (the persisted scroll record) is in core/gui_ctx.h: it is
   retained-mode storage (gui_window_t embeds one), so it lives with the server's records.
   The machinery that drives it stays here (flow/gui_scroll.c). */

/*==============================================================================================
    Layout-frame (stack storage in flow/gui_layout_core.c)

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

/* Orthogonal modifiers -- gaps and alignment.  align (gui_align_t flags) is where a widget's
   natural-sized content sits in its cell; 0 = LEFT | TOP.  (The field split moved out to the
   ambient gui_field_t -- it is a set-once authority like a style, not a per-region modifier.) */

typedef struct
{
    f32 gap_x, gap_y;               // inter-cell spacing request; 0 = live theme default (mod_gap_x/_y)
    u8  align;                      // gui_align_t flags

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

    gui_rect_t rect_next;           // one-shot explicit cell (next_item_rect): the caller owns the
    bool       rect_next_set;       //   exact rect; cell_next_w returns it and moves no pen

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

extern layout_frame_t s_layout_stack[ GUI_LAYOUT_DEPTH ];  /* flow/gui_layout_core.c -- region
                                                              stack                          */
extern u32            s_layout_sp;  /* active frame count; top = s_layout_sp - 1 */

layout_frame_t* lf( void );         /* flow/gui_layout_core.c -- top frame (clamped, never NULL) */

/*==============================================================================================
    Persistent region state (flow/gui_scroll.c)

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

/* Pure geometry for one labeled ("pair") row: split `cell` into a label rect and a control
   rect for the given side / track units.  NONE lays the control across the cell with the label
   trailing at label_w; LEFT / RIGHT lay two resolved tracks (control floored at min_ctrl,
   RIGHT re-anchored so a squeezed row overflows instead of the label crawling under the
   control).  The caller measured label_w and paints -- flow never colors.  THE splitter every
   labeled widget resolves through, driven from field_effective (mod split or ambient field). */
void field_geom_split( gui_rect_t cell, gui_label_side_t side, f32 control_u, f32 label_w,
                       f32 min_ctrl, f32 pad, gui_rect_t* out_label, gui_rect_t* out_control );

/* Consume the skip_label one-shot (returns true once if armed) -- gui_field_row's escape hatch. */
bool field_skip_take( void );

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

/*==============================================================================================
    Table engine (flow/gui_table_engine.c) -- the widget-agnostic machinery behind chrome's
    table_* verbs, at the service tier so a second kit can build its own table look without
    chrome: persisted column tracks + the boundary pair-resize drag, the sort state machine +
    the stable display-order sort, and fixed-pitch row virtualization.  Chrome keeps the
    one-clip model, the header paint, stripes / dividers, and the scroll-region policy.
==============================================================================================*/

/* Per-table persistent state: column widths, sort choice, and scroll position survive frames.
   A big-class tenant of the keyed state pool (GUI_STATE), so every field's default must be its
   zero -- the zero-on-create contract: sort_col is 1-BASED with 0 = unsorted.  The scroll link
   is storage for the caller's scrolling body region (the engine never reads it). */
typedef struct
{
    f32        col_w[ GUI_TABLE_COLS_MAX ];   /* 0 = use the setup width / default   */
    i8         sort_col;                        /* 1-based sorted column; 0 = unsorted */
    i8         sort_dir;                        /* 0 = ascending, 1 = descending       */

    gui_scroll_link_t scroll;                   /* the caller's body-region scroll + extent */

} gui_table_persist_t;

ORB_STATIC_ASSERT( sizeof( gui_table_persist_t ) <= GUI_STATE_BIG_CAP,
                   "gui_table_persist_t is the big class's sizing tenant; grow GUI_STATE_BIG_CAP" );

/* Resolve column origins / widths into out_x / out_w.  Priority per column: user-resized
   persist width > setup fixed px (init_w[i] > 1) > stretch.  x / w = the column strip. */
void table_tracks_resolve( const gui_table_persist_t* p, const f32* init_w, i32 init_n,
                           i32 ncols, f32 x, f32 w, f32* out_x, f32* out_w );

/* Interior-boundary pair-resize drag over the resolved columns; mutates p->col_w and
   re-resolves col_x / col_w on a live drag.  pin_mask bit i pins the boundary on column i's
   right edge; band_box spans the grab bands vertically; `front` gates the hover.  Returns the
   hot / dragged boundary (-1 none).  The caller owns the hit-clip policy around the call. */
i32  table_resize_drag( gui_id_t id, gui_table_persist_t* p, const f32* init_w, i32 init_n,
                        i32 ncols, u32 pin_mask, gui_rect_t band_box, bool front,
                        f32 x, f32 w, f32* col_x, f32* col_w );

/* Header sort click on 0-based `col`: same column flips direction, a new column sorts it
   ascending.  Pure persist-state cycle; the caller owns any dirty flag. */
void table_sort_click( gui_table_persist_t* p, i32 col );

/* Stable reorder of a caller-owned display-order index array by (col, desc) via the public
   sort callbacks (cmp_fn wins over val_fn).  Pure; true when it reordered. */
bool table_order_sort( i32* order, i32 count, i32 col, bool desc,
                       gui_table_sort_value_fn val_fn, gui_table_sort_cmp_fn cmp_fn,
                       void* user );

/* Fixed-pitch visible span over the OPEN region from screen-space `top` (row height h,
   pitch h + WIDGET_GAP): reserves all `count` rows of extent so the scrollbar range sees the
   full run, jumps the pen past the culled head, and returns [first, last).  The caller seeds
   its own row iteration from the result. */
gui_span_t table_rows_span( i32 count, f32 h, f32 top );

/*==============================================================================================
    UPWARD SEAMS -- the flow unit's only calls above its layer.  Do not add more.

    Declared HERE (upward-seam declarations live with their LOWEST consumer; the
    units above see them through this header):

    scrollbar_widget (defined chrome/widgets/gui_scrollbar.c) -- the region gutter's ONE
        widget: layout_pop_region measured the content, so the bars land where the gutters
        were reserved.  The plan's one sanctioned flow -> chrome call.

    draw_child_bg / draw_child_border / draw_resize_highlight (defined
        stock/gui_adornment.c) -- child_begin/end paint the child box through these styled
        painters; the box decision and its paint land in the same call, like core's
        draw_nav_ring and interact's draw_drop_ring.
==============================================================================================*/

void scrollbar_widget( gui_id_t region_id, gui_rect_t track, bool vertical,
                       f32 content, f32 view, f32* scroll );

void draw_child_bg        ( gui_rect_t r );
void draw_child_border    ( gui_rect_t r );
void draw_resize_highlight( gui_rect_t r, u8 edges );

/* item_flags_resolve / item_flags_chrome_reset (stock/gui_adornment.c) -- the per-item
   ambient application wrappers this unit drives at its emit / chrome seams (cell_next_w,
   layout_pop_region); chrome and element reach them through this header. */
gui_item_flags_t item_flags_resolve( void );
void             item_flags_chrome_reset( void );

u32 gui_flow_unit_mem_bytes( void );               /* the flow unit's fixed statics           */

// clang-format on
/*============================================================================================*/
#endif    // GUI_FLOW_H
