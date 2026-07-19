#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H
/*==============================================================================================

    runtime_service/gui/gui_internal.h -- Shared internal types for the gui unity build.

    gui is built as a unity translation unit (gui.c #includes every gui_*.c).  Historically
    each constituent file defined its record types inline and later-included files relied on the
    include ORDER to see them -- a window record defined in gui.c, a layout frame in gui_ctx.c,
    a viewport in gui_render.c, all folded by value into gui_context_t further down.  This
    header lifts that cross-file TYPE layer into one place so the dependency is explicit and
    order-independent: every constituent file includes this once, up front, and sees the full set.

    It holds ONLY types (and the capacities they embed) shared across more than one constituent
    file.  File-private record types (the GPU command, the font atlas, the style save-pairs, the
    text-edit scratch) stay in their owning .c file.  The per-file STATE instances (s_io, s_build,
    g_ctx, the stacks) also stay in their owning files -- this header declares their types, not the
    storage; the unity TU still resolves the statics.

    Include chain: gui_internal.h -> gui_host.h -> gui_api.h -> gui.h
    Also pulls rhi_api.h (gui_viewport_t holds GPU buffers/targets) and app_api.h (gui_io_t
    indexes app keys; the OS-event forwarders take an app_event_t; the render backend also
    calls app()->window_close directly when tearing down an owned floater's OS window).

==============================================================================================*/

#include "runtime_service/gui/gui_host.h"   // public gui types: gui_rect_t, gui_id_t, flags, enums
#include "runtime_service/rhi/rhi_api.h"    // rhi_buffer_t / rhi_texture_t for gui_viewport_t
#include "engine/app/app_api.h"             // app_key_t / app_event_t for gui_io_t + event forwarders

// clang-format off
/*==============================================================================================
    Shared capacities

    Fixed-array bounds embedded in the shared record types below.  (GUI_LAYOUT_COLS lives in the
    public gui.h; the per-file stack depths that are NOT embedded in a shared type -- the id stack,
    the item-flag stack -- stay private to their owning .c file.)
==============================================================================================*/

/* Per-context default pool sizes -- used to wire the static default context (slot 0).
   Secondary contexts may use different sizes passed via gui_ctx_config_t. */

#define GUI_DEFAULT_MAX_WINDOWS     32      // default persisted window pool
#define GUI_DEFAULT_POPUP_DEPTH     8       // default max nested popups
#define GUI_DEFAULT_STATE_SLOTS     512     // default keyed state pool capacity (power of two)
#define GUI_DEFAULT_DOCK_NODES      48      // default dock-tree nodes

/* Non-per-context capacities -- these size non-context structs and stay as fixed constants. */

#define GUI_LAYOUT_DEPTH            8       // max nested scroll regions (windows or children)
#define GUI_KEY_COUNT               128     // gui_io_t key arrays; must cover the full app_key_t range

#define GUI_DOCK_TABS_MAX           8       // windows co-docked (tabbed) in one leaf node
#define GUI_DOCK_NAME_CAP           28      // bytes of a tab's display name, copied at dock time

/* Keyed state pool slot payloads -- two size classes over one probe (core/gui_state.c).
   gui_state_get picks the class from the requested size; the caps are asserted against their
   largest tenants in gui_state.c / gui_table.c. */

#define GUI_STATE_CAP               24      // small-class payload bytes (max tenant: gui_region_t)
#define GUI_STATE_BIG_CAP           96      // big-class payload bytes (max tenant: gui_table_persist_t)
#define GUI_STATE_BIG_SLOTS         32      // big-class capacity, power of two (tables are the main tenant)

/* Render-surface ceiling: one gui viewport rides one OS window + one rhi context, so the pool
   default, the per-context cap, and the GPU buffer regions (allocated once at init, before any
   config) are all sized by the platform pair -- derived, not repeated.  The per-context runtime
   limit is still g_ctx->vp.max. */

#define GUI_MAX_VIEWPORTS APP_WIN_MAX       // one viewport per OS window / rhi context

ORB_STATIC_ASSERT( APP_WIN_MAX == RHI_CTX_MAX,
                   "a gui viewport pairs an OS window with an rhi context; the maxes must agree" );

/*==============================================================================================
    Input snapshot (core/gui_io.c)

    The frame's distilled IO state -- not exposed in the public header.  GUI_KEY_COUNT must cover
    the full app_key_t range; core/gui_io.c carries the static assert that verifies this.
==============================================================================================*/

typedef struct
{
    f64   time;                     // seconds since the first frame -- dt accumulated; backs get_time()
    f32   dt;                       // seconds since the last frame; backs get_delta_time()  
    i32   display_w, display_h;     // OS window client size (pixels); backs get_display_size()
    u32   mouse_viewport;           // surface the cursor is in (resolved from mouse-event win_id); persists

    f32   mouse_x, mouse_y;
    f32   mouse_wheel;
    bool  mouse_down    [ 3 ];
    bool  mouse_pressed [ 3 ];
    bool  mouse_released[ 3 ];
    bool  mouse_double  [ 3 ];

    bool  keys_down[ GUI_KEY_COUNT ];
    bool  keys_pressed[ GUI_KEY_COUNT ];            // initial press only
    bool  keys_pressed_repeat[ GUI_KEY_COUNT ];     // initial press + OS auto-repeat ticks
    bool  keys_released[ GUI_KEY_COUNT ];

    char  text[ 32 ];               // UTF-8 text input delivered this frame (APP_EV_TEXT), else empty
    char  paste[ 256 ];             // clipboard text delivered this frame (APP_EV_CLIPBOARD), else empty

} gui_io_t;

/*==============================================================================================
    Widget interaction (gui_paint_core.c)

    The interaction class picked at the call site.  Only the press-time behavior differs between
    widgets; everything else (hover/active/click) is uniform.  item_state's per-frame result
    is the PUBLIC gui_item_state_t (gui.h) -- stock widgets and gui()->item() callers read the
    same record, so a custom widget is built on exactly the substrate the built-ins use. */

typedef enum
{
    ITEM_BUTTON    = 0,   // press captures active; reports clicked
    ITEM_DRAG      = 1,   // press captures active; held for dragging
    ITEM_FOCUSABLE = 2,   // press also claims keyboard focus

} gui_item_kind_t;

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

} gui_scroll_link_t;

/*==============================================================================================
    Persisted window record (behavior in gui_window.c)

    One persisted window.  Geometry is owned here after the first appearance; the window pool that
    holds these lives in the bound context (gui_context_t).  Behavior is in gui_window.c.
==============================================================================================*/

typedef struct gui_window_t
{
    gui_id_t    id;             // id_hash(title); 0 = free slot
    f32         x, y;           // persisted top-left (updated by dragging)
    f32         w, h;           // persisted dimensions
    u32         z;              // paint order: higher = more recently raised = in front
    u32         viewport;       // target surface index (0 = main swapchain); set via window_set_next_viewport

    /* Popup / tooltip overlay: an anchored overlay on its surface, never the OS-window frame
       (window_is_native), never a nav or tab-drop target.  Stamped by the popup layer each begin,
       alongside a z in the reserved overlay band (see the z band map in surface/gui_surface.c) --
       the flag carries the TYPE fact so z stays pure paint order. */
    bool       overlay;

    gui_scroll_link_t scroll;   // persisted scroll offset + last-measured content extent

    bool       collapsed;       // title-bar-only when set; toggled by the arrow
    bool       closed;          // CLOSEABLE: hidden by the X until re-opened

    /* Maximize / minimize for a regular (non-native, non-docked) floater -- state transitions in
       gui_window_free.c (window_maximize_set / window_minimize_set), chrome in gui_window_end.c.
       Maximized pins the window to its surface work area every frame; minimized parks it as a
       title-bar chip on a shelf along the surface's bottom edge.  norm is the saved normal rect
       both states restore to; shelf_slot orders the chips (taken at minimize time, compacted to
       a paint position each frame as neighbours restore). */
    bool       maximized;
    bool       minimized;
    u32        shelf_slot;
    struct { f32 x, y, w, h; } norm;

    /* Maximize / minimize / restore transition: while state_anim is set, window_begin_ex eases the
       live rect (x/y/w/h) from anim_from toward the current state target, all four channels sharing
       one gui_anim_timer clock so they arrive together, then clears the flag once the timer reports
       done (window_anim_step in gui_window_free.c).  anim_from is the rect captured at the moment the
       transition started; the target is recomputed every frame (so a live surface resize retargets
       mid-flight).  With window animation disabled the timer settles on frame one -- an instant snap
       through the same path.  See s_win_anim / gui_window_anim_enable. */
    bool       state_anim;
    struct { f32 x, y, w, h; } anim_from;

    /* Collapse transition: the arrow / double-click toggles win->collapsed, and window_begin_ex tweens
       the SHOWN body height (disp_h) between the full height and the title bar rather than snapping --
       a single-channel height ease on its own gui_anim_timer clock (window_collapse_h /
       window_collapse_set in gui_window_free.c).  collapse_from is the height shown when the toggle
       fired (the tween's start, snapshotted from shown_h so an interrupted toggle is seamless);
       shown_h is the body height actually displayed last frame, kept for that snapshot.  Cancelled
       when a maximize / minimize / restore takes over the geometry (window_anim_begin). */
    bool       collapse_anim;
    f32        collapse_from;
    f32        shown_h;

    /* Re-open of a CLOSEABLE floater: closing it lets the abandoned-teardown free its OS window,
       reverting this record to viewport 0.  `floater` remembers it was one so the next begin
       re-spawns it.  The geometry is the floater's RESTORE (normal) state, sampled every frame it
       is not maximized -- so a floater closed while maximized re-opens maximized yet still
       restores to its previous normal size. */
    struct
    {
        bool floater;          // re-spawn as a floater on the next begin
        bool maximized;        // floater was maximized -- re-maximize after re-spawn
        i32  home_x, home_y;   // saved restore (normal) client-corner screen pos
        f32  w, h;             // saved restore (normal) size
    } reopen;

    gui_win_flags_t flags;              // behavior flags supplied to window_begin

    /* Next-window channel bookkeeping (see window_set_next_pos / _size).  last_frame drives the
       "appearing" test; the allow masks track which conditions a queued value may still fire. */

    u32        last_frame;              // frame index last begun; 0 = never begun

    u8         set_pos_allow;           // conds still permitted to set position (gui_cond_t bits)
    u8         set_size_allow;          // conds still permitted to set size (gui_cond_t bits)

} gui_window_t;

/*==============================================================================================
    Keyboard navigation state (driver in gui_nav.c)

    The nav cursor -- the persistent analogue of hover_id, moved by the arrow keys / Tab rather
    than the mouse -- plus the menu-bar state machine layered on top of it.  The instance is the
    bound context's nav member (g_ctx->nav), so each context keeps its own cursor.
==============================================================================================*/

/* One entry of the per-frame nav item list.  Every item of the nav window records itself here
   (emission order) as it passes through item_state; the resolvers in gui_nav.c consume the
   list at the NEXT nav_new_frame, so a move steps over last frame's items -- the same one-frame
   deferral hover_win uses.  region/line are the structural coordinate the layout engine stamped
   when it placed the item (cell_next_w), so moves are index math over real rows, not a
   spatial guess over rects; the rect remains only for the goal-column pick. */

typedef struct
{
    gui_id_t   id;       // widget id
    gui_rect_t rect;     // screen rect as emitted (goal-column pick + ring)
    u32        region;   // region sequence that placed it (a window body / child / strip region)
    u32        line;     // line sequence within the frame -- monotonic, so order == reading order
    bool       chrome;   // not layout-placed (title button, dock tab): the F6 chrome lane
    bool       drag_kind; // ITEM_DRAG (slider / drag box): eligible for row-solo auto-adjust

    /* Type-ahead label (gui_nav.c): lowercased, truncated, stamped by an opt-in widget right after
       it registers (gui_selectable) -- empty ("") means this item does not participate.  Kept on
       the nav item itself rather than a side table so type-ahead reuses the exact same one-frame-
       lagged list every other resolver (Up/Down, Tab, Home/End) already consumes. */
    char       label[ 16 ];

} gui_nav_item_t;

/* List capacity.  Every emitted item of the nav window registers -- including rows scrolled out
   of view (they still emit; only their draw is clipped) -- so this bounds the navigable item
   count of one window; overflow items simply drop off the keyboard map for that frame. */
#define GUI_NAV_ITEMS_MAX 1024

typedef struct
{
    gui_id_t    id;            // the highlighted item (keyboard cursor); persists across frames
    gui_id_t    win;           // window/popup nav is scoped to (the hover_win analogue)

    /* Keyboard-focused window (click / gui_window_set_nav / Ctrl+Tab / Alt-mnemonic), the SOLE
       authority for where the keyboard goes.  NONE means no window has focus -- a background/viewport
       click clears it -- and the keyboard then falls through to the app; there is no front-most
       fallback, so a defocused window does not silently keep the keyboard.  A tool that wants a window
       focused on first appearance sets it via gui_window_set_nav on its own GUI_COND_APPEARING edge. */
    gui_id_t    focused_win;

    /* Two visual states, the Dear ImGui NavDisableHighlight split.  active means a nav cursor
       position exists -> the outline ring is drawn at id (and follows clicks), persisting even in
       mouse mode so it keeps its location.  highlight means the keyboard is the *active* instrument
       right now -> the nav item also takes the fill (like a hovered button), mouse hover is
       suppressed (so the two never double-fill), and the keyboard is captured.  A nav key sets both;
       a mouse move or click drops highlight (back to ring-only), leaving active. */

    bool        active;        // a nav cursor exists -> draw the ring (cleared rarely)
    bool        highlight;     // keyboard is the active instrument -> fill + hover-suppress

    i32         move_dir;      // directional request this frame (gui_dir_t, or -1 for none)
    i32         tab;           // Tab linear move: +1 forward, -1 back, 0 none
    i32         page;          // PageUp/PageDown: -1 / +1 -- vertical hop by one view height
    i32         home;          // Home/End: -1 / +1 -- first / last item of the cursor's region
    bool        activate;      // Enter/Space -> fire id like a click this frame
    bool        lane;          // F6 -> hop between the body and the chrome strip this frame

    /* Keyboard value edit: activating a DRAG widget (slider, drag box) captures it -- the drag
       twin of focused_id text capture.  While captured, Left/Right step the value through
       gui_item_state_t.nav_adjust and Enter/Space/Esc (or a mouse press) release. */
    gui_id_t    edit_id;       // DRAG widget captured for keyboard value edit; 0 = none
    i32         edit_dir;      // value-edit arrow step this frame: -1 / +1 / 0

    /* Row-solo auto-adjust: a DRAG widget with no row neighbor on either side has nothing for
       Left/Right to navigate to, so those keys step its value directly instead of doing nothing --
       no Enter/Space capture required.  Up/Down are untouched (never fenced), unlike edit_id
       capture.  Resolved once per frame in nav_finish (last frame's list, before it resets) and
       consumed by nav_item_register during this frame's emission to mirror edit_id's captured
       presentation (st->focused, the COL_NAV_CAPTURE ring) and to gate st->nav_adjust. */
    gui_id_t    solo_drag_id;

    bool        id_seen;       // id was emitted in win this frame (else it went stale)
    gui_id_t    first_item;    // first layout-placed item this frame (first-focus / recovery)
    gui_id_t    body_id;       // body cursor to land back on when F6 leaves the chrome lane

    /* Goal column: the remembered x a run of Up/Down steers by, so vertical travel through rows
       of differing shapes does not drift sideways (the text-editor goal-column behavior).  Set
       lazily from the cursor item when a vertical run starts; any other adoption (horizontal
       move, Tab, click) clears it so the next run re-anchors. */
    f32         goal_x;
    bool        goal_set;

    /* Scroll-to-view request: set when a resolver adopts a new cursor item, consumed by
       nav_item_register when that item registers -- if its rect sits outside its region's view,
       the region (and its ancestors) scroll it into view (nav_scroll_chase, gui_paint_core.c).
       One-shot per adoption so the chase never fights the wheel or a scrollbar drag. */
    bool        scroll_chase;

    /* Type-ahead query (gui_nav.c): A-Z / 0-9 keys accumulate here while nav owns the keyboard,
       timed out and restarted after GUI_TYPEAHEAD_TIMEOUT of no typing.  A repeated single key is
       the Explorer-style cycle case (buf stays length 1; the resolver scans past the current
       cursor instead of from the top) rather than a two-letter prefix.  type_dirty marks a fresh
       keystroke this frame so nav_finish resolves it at most once. */
    char        type_buf[ 16 ];
    u32         type_len;
    f64         type_last_t;
    bool        type_dirty;

    /* The nav item list -- built during emission, resolved at the next nav_new_frame. */
    gui_nav_item_t items[ GUI_NAV_ITEMS_MAX ];
    u32            item_count;

    /* Menu-bar navigation -- a small state machine layered on the nav cursor + popup stack, entered
       by Alt (toggle) or an Alt+letter mnemonic.  While active, nav lives either on the bar entries
       (in_menus false: win is the bar window, a highlighted entry drops its menu) or inside the open
       menu popups (in_menus true: win is the top popup).  Down/Enter descend, Up at a top item and
       Left/Esc ascend -- always landing back on menu_owner so closing a menu returns to the bar
       entry that opened it (not the first entry).  See gui_nav.c + menu_begin. */

    gui_id_t    bar_win;       // menu-bar window nav is driving; 0 = not in menu-bar mode
    bool        in_menus;      // menu mode: false = on the bar entries, true = inside the popups
    gui_id_t    menu_owner;    // bar entry whose menu is open -- the ascend / close return target
    gui_id_t    prev_win;      // nav target to restore when Alt toggles out of the menu bar
    gui_id_t    prev_id;       // nav cursor to restore on Alt toggle-out (the last focus location)
    u8          mnemonic;      // pending Alt+letter mnemonic (uppercase ASCII); 0 = none

} gui_nav_state_t;

/*==============================================================================================
    Layout-frame (stack in gui_ctx.c)

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

/*==============================================================================================
    Persistent region state (gui_scroll.c)

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
    Window context (the `win` member of s_build, core/gui_ctx.c; stamped in window/)

    The flat "window currently between window_begin / window_end" record -- everything begin
    stamps and end consumes.  One named struct so the overlay seam (gui_overlay_save_t below)
    saves and restores the whole window context as a single assignment: a field added here is
    carried across the popup seam automatically.  The frame-global scratch that must SURVIVE
    that seam (wheel_used, the nav dispensers, the combo channel) lives beside it in s_build,
    outside this record.
==============================================================================================*/

typedef struct
{
    gui_id_t    id;            // id of the window currently between begin/window_end
    const char* title;         // title string, cached for window_end's deferred chrome
    bool        collapsed;     // current window is collapsed (title bar only this frame)
    bool        minimized;     // current window is a shelf chip (title bar only, chip chrome)
    bool        hidden;        // CLOSEABLE + closed: begin emitted nothing, end early-outs
    gui_win_flags_t flags;     // behavior flags supplied to window_begin
    f32         title_h;       // title bar height (0 if NOTITLEBAR)
    struct gui_window_t* rec;  // persisted window record; scroll write-back target

    /* Docking (gui_dock.c): the node hosting the current window, or NULL when it is
       free-floating.  When set, the window's geometry is owned by the node and its title bar is
       replaced by the node's tab strip; dock_active distinguishes the visible tab (draws a body)
       from a window docked behind another tab (window_begin returns false, draws nothing). */
    struct gui_dock_node_t* dock_node;
    bool        dock_active;

    f32         x, y;          // current window top-left (outer frame)
    f32         w, h;          // current window dimensions

    gui_rect_t  menubar_rect;  // reserved strip (WIN_MENUBAR); menu_bar_begin fills it

    u32         viewport;      // ambient viewport for new-window inheritance (stamped per window)

} gui_win_ctx_t;

/*==============================================================================================
    Interaction scope (the s_scope instance in core/gui_ctx.c)

    The declared contract between composition and behavior.  Everything item_state
    (interact/gui_item.c) consumes about "where is this item emitting" lives here, nowhere
    else: composition stamps the record at its seams (window_begin / child_begin / popup / table
    stamp win + clip; the resize/grip resolvers stamp the chrome suppression; the emit seam
    cell_next_w latches the per-item flags + nav stamp), and behavior reads only this
    record plus its own s_interaction -- never the composer scratch (s_build).  Behavior
    publishes its result back into the last_* fields, where the item-query readers
    (user/gui_query.c), drag sourcing, and context-menu anchoring pick it up.  Unlike the
    s_interaction arbitration fields (verb-only writes), the scope is stamped directly: the
    contract is the record itself.  The overlay seam saves and restores it wholesale, so a field
    added here only needs its per-frame reset.  Tier: frame scratch, same lifetime as s_build.
==============================================================================================*/

typedef struct
{
    /* scope -- stamped at the window/child/popup/table seams */
    gui_id_t   win;          // scope owner: hover domain (vs hover_win) and nav domain (vs g_ctx->nav.win)
    gui_rect_t clip;         // active interaction clip -- widget hover is gated by it
    u8         resize_hot;   // resize chrome hot this frame (GUI_RESIZE_* edge bits, plus
                             //   GUI_RESIZE_GRIP for the CAN_AUTOSIZE corner) -- any bit set
                             //   suppresses widget hover so the chrome owns the cursor

    /* item -- latched at the emit seam (cell_next_w), dropped at the chrome seams. */
    gui_item_flags_t flags;  // flags resolved for the item being emitted (item_flags_resolve)

    /* The nav structural coordinate passes through three roles on its way here: the frame-global
       DISPENSERS (s_build.nav_region_seq / nav_line_seq) mint sequence numbers, the open region's
       layout frame carries its live COORDINATE (layout_frame_t.nav_region / nav_line), and this
       group is the per-item STAMP the emit seam latches from the frame.  Latched, not one-shot:
       it stays across the several behavior calls one cell can make (a numeric's sub-fields are
       same-line siblings); item_flags_chrome_reset drops `placed` at every chrome seam -- so
       "was this item placed by the layout engine" is exactly the body/chrome split. */
    struct
    {
        u32  region;         // stamp: region that placed the item being emitted
        u32  line;           // stamp: its line sequence
        bool placed;         // stamp is live (false => the next behavior call is chrome)
        bool skip;           // one-shot: the next behavior call is no keyboard target at all
                             //   (scrollbar, drag strip) -- consumed by item_state
    } nav;

    /* result -- published by item_state for the most recent item (the Dear ImGui IsItem*
       model): the item-query readers report on "the widget just emitted" with no per-widget
       bookkeeping, the same anchor context menus / tooltips / drag sources hang from. */
    gui_id_t       last_id;      // id of the most recent widget emitted
    gui_rect_t     last_rect;    // its screen rect
    gui_item_state_t last_status;  // its resolved hover / active / clicked / focused / nav flags

} gui_scope_t;

/*==============================================================================================
    Draw scope (state in backend/pipeline/gui_emit_draw.c; accessors in gui_backend.h)

    The backend paint cursor as one record: the command segment tag (owning window, sort key,
    viewport, arena band -- the ambient font stays global by design) plus the ambient glyph-clip
    window (a table cell sets it for its span).  draw_scope / draw_scope_set read and write it
    wholesale for the overlay seam.
==============================================================================================*/

typedef struct
{
    gui_id_t window;         // s_draw.cur_win (retained-cache key)
    u32      sort_key;       // s_draw.cur_z (paint order)
    u32      viewport;       // s_draw.cur_vp (target surface routing)
    u32      band;           // s_draw.cur_band (arena band: debug UI isolation)
    f32      text_clip_x0;   // ambient glyph-clip window
    f32      text_clip_x1;

} gui_draw_scope_t;

/*==============================================================================================
    Popup stack (gui_ctx.c; driver in gui_popup.c)

    A popup is a top-level overlay begun while a parent window is still open but laid out,
    clipped, and painted independent of it.  gui_overlay_save_t is the parent context popup_end
    restores: whole-struct copies of the window context, the interaction scope, and the draw
    scope, plus the parent's top layout frame (its pen must survive the popup's region pop).
    Because the copies are wholesale, nothing here is maintained field-by-field -- a field added
    to any of those records rides the seam automatically.  The open set is a stack ordered
    parent -> child, held in gui_context_t.
==============================================================================================*/

typedef struct
{
    gui_win_ctx_t    win;      // s_build.win -- the flat window context
    gui_scope_t      scope;    // s_scope -- the interaction scope (behavior contract)
    gui_draw_scope_t draw;     // backend paint cursor + ambient glyph-clip window

    bool             had_parent;    // a layout region was open (parent frame valid)
    layout_frame_t   parent_frame;  // the parent's top frame, restored after the popup

} gui_overlay_save_t;

typedef struct
{
    gui_id_t            id;                 // popup window id (salted; matches s_build.win.id / hover_win)
    bool                modal;              // blocks input behind it + dims the background
    f32                 anchor_x;           // open point -- where a non-modal popup is placed
    f32                 anchor_y;           // 
    u32                 open_frame;         // frame popup_open ran -- "appearing" detection
    u32                 begun_frame;        // last frame popup_begin ran -- drives stale-close
    gui_rect_t          rect;               // on-screen rect last frame -- drives click-outside
    gui_overlay_save_t  saved;              // parent context to restore at popup_end

} gui_popup_t;

/*==============================================================================================
    Keyed state pool + per-context retained state (gui_ctx.c)

    The single store a widget uses to keep bytes alive across frames, keyed by its id (a
    region's scroll, a tree node's open flag, a combo's popup state, a table's column widths),
    plus the per-context id-salt and frame clock that stamp and age it.  Two open-addressing
    hash tables -- a small class and a big class -- walked by the one probe in gui_state.c,
    which also holds the reclamation contract.  Both slot types share the (id, seen_frame)
    header prefix the probe reads.
==============================================================================================*/

typedef struct
{
    gui_id_t    id;                       // 0 = empty slot
    u32         seen_frame;               // frame last touched -- drives stale reclamation

} gui_state_hdr_t;

typedef struct
{
    gui_id_t    id;                       // 0 = empty slot
    u32         seen_frame;               // frame last touched -- drives stale reclamation
    u8          data[ GUI_STATE_CAP ];    // payload; naturally 4-byte aligned (follows two u32 fields)

} gui_state_slot_t;

typedef struct
{
    gui_id_t    id;                       // 0 = empty slot
    u32         seen_frame;               // frame last touched -- drives stale reclamation
    u8          data[ GUI_STATE_BIG_CAP ];// payload for the rare large tenant (table persist)

} gui_state_big_slot_t;

typedef struct
{
    /* Per-context id namespace seed.  XOR'd into id_hash's FNV basis, so the same string hashes to a
       distinct id in each context and every id_combine built on it inherits the offset.  Keeps the
       ambient hover / active / focus ids -- compared globally across contexts -- from confusing a
       widget in one viewport with an identically-named widget in another.  0 is the default
       context's namespace and leaves id_hash byte-identical to the unsalted hash. */

    u32 id_salt;

    u32  frame;           // monotonic frame index, bumped each ctx_begin this context is built
    bool wants_redraw;    // set by gui_anim_f32 while mid-transition; cleared at ctx_begin

    gui_state_slot_t*   state;       // open-addressed keyed per-widget state; points into context alloc
    u32                 state_count; // capacity, power of two
    u32                 state_mask;  // state_count - 1, for bucket masking

    gui_state_big_slot_t* state_big; // big-class slots (GUI_STATE_BIG_SLOTS); points into context alloc
    u32                 big_mask;    // GUI_STATE_BIG_SLOTS - 1, for bucket masking

} gui_retained_t;

/*==============================================================================================
    Render viewport (behavior in gui_render.c)

    One render surface a context drives: GPU buffers + a color target, the OS window hosting it, and
    the routing/ownership bookkeeping for host-provided vs gui-owned (torn-off floater) surfaces.
    [0] is the main swapchain; the rest are floaters.  Held by value in gui_context_t vp.pool.
==============================================================================================*/

struct gui_dock_node_t;         // the dock tree node -- defined in full after gui_viewport_t below

/* Pool index of a dock node (into gui_context_t dock.pool), not to be confused with gui_dock_id_t
   (the stable id_hash-style handle exposed to callers).  The pool is fixed-size and never compacts,
   so an index stays valid across frames exactly like the pointer it replaces -- but at 2 bytes
   instead of 8.  dock_ref()/dock_at() (gui_dock_core.c) convert to/from a live pointer. */
typedef u16 gui_dock_ref_t;
#define GUI_DOCK_REF_NONE ( (gui_dock_ref_t)0xFFFFu )

typedef struct
{
    rhi_buffer_t  vb;           // CPU_TO_GPU vertex buffer, one region per frame-in-flight
    rhi_buffer_t  ib;           // CPU_TO_GPU index buffer (u16), one region per frame-in-flight

    /* Color target flush paints into: RHI_SWAPCHAIN_COLOR for the main viewport, a floater's own
       swapchain image otherwise.  Held per viewport so flush is target-agnostic. */
    rhi_texture_t target;

    /* OS window this surface is hosted by (app win_id_t), or -1 (APP_WIN_INVALID) if unassociated.
       Input routing maps a mouse event's win_id to this surface so the cursor's host viewport is
       known -- a window only hover-tests when the cursor is in the OS window hosting its viewport. */
    i32 win_id;

    /* rhi context driving this surface's swapchain (RHI_CTX_INVALID if none).  Only set for an
       gui-OWNED surface (a torn-off floater gui spawned): flush of a host-provided surface
       resolves RHI_SWAPCHAIN_COLOR from the host's cmd, so the host viewport leaves this invalid.
       An owned surface has no host driving it -- gui runs frame_begin/end on this ctx itself. */
    i32 rhi_ctx;

    /* true when gui created this surface's OS window + rhi context (tear-off floater) and must
       therefore destroy them.  false for the host-provided main surface (index 0) and any surface
       the host opened via viewport_open -- gui frees only the GPU buffers for those, never the
       window/context it does not own. */
    bool owned;

    /* Set when the user closes an owned floater's OS window (APP_EV_WIN_CLOSE): the surface is torn
       down at the next viewport_update, a safe point between the build and the present, so
       no in-flight draw list references a surface being freed.  Ignored for non-owned surfaces. */
    bool pending_close;

    /* Drawable size of this surface in pixels.  Set by the host (viewport 0 from frame_begin, floaters
       via viewport_resize) BEFORE the build so window_begin clips its windows against THIS surface's
       extent, not the main window's.  0 = unset -> window_begin falls back to the main display size
       (single-window behavior).  Distinct from the win_w/win_h passed to flush, which only sets the
       GPU viewport/scissor clamp at submit time; the clip baked into each draw command is built here. */
    i32 disp_w, disp_h;

    /* Top band (pixels) drawn by this surface's native host caption (the GUI_WIN_NATIVE shell
       window's title bar height), published each frame by that shell.  window_clamp keeps non-native
       windows' top edge at or below this inset so their title bars stay grabbable above the drawn
       chrome band.  0 until first published (no native shell or default OS-chrome main window).
       Sticky: NOT cleared each frame -- persists from the last frame the native shell was active so
       viewport_update always has a valid top bound regardless of build ordering. */
    f32 caption_inset;

    /* Main-menu-bar band on this surface: its height plus the frame it last emitted.  Emit-gated
       like a dockspace (bar_seen_frame against the current frame, one-frame tolerance) so a host
       code path that stops emitting the bar releases the band; window_work_top
       (gui_window_free.c) adds it to caption_inset to bound a maximized window's work area. */
    f32 bar_inset;
    u32 bar_seen_frame;

    /* Additional top band (pixels) the host reserves above the dock area -- a main menu bar, a
       toolbar strip -- published via gui()->dockspace_inset() before dockspace_over_viewport.
       Adds to caption_inset when the dock tree lays out.  Sticky like caption_inset: persists
       until the host publishes a new value (0 to reclaim).  Free-floating windows are
       unaffected -- only the dock tree's layout area shrinks. */
    f32 dock_inset;

    /* Per-surface dock tree root.  GUI_DOCK_REF_NONE = free-float placement (overlapping windows,
       including the main viewport); otherwise a ref into the context dock-node pool that tiles/tabs
       the windows on this surface.  Driven by dock/ (dock.c builds it, dock_drag.c re-tiles on drag,
       dock_serialize.c saves/loads it). */
    gui_dock_ref_t dock_root;

    /* Dockspace policy bits (gui_dockspace_flags_t), re-published by every dockspace_over_viewport
       call.  NO_SPLIT restricts the tree to tab docking: no split drop chips, split verbs refuse. */
    gui_dockspace_flags_t dock_flags;

    /* Frame stamp (g_ctx->retained.frame) of this viewport's last dockspace_over_viewport call.
       A dockspace is emit-gated like every immediate-mode element: the tree is ACTIVE only on
       frames the host emits it; on other frames it is DORMANT -- retained but inert.  Windows
       tabbed in a dormant tree suppress (inactive-tab semantics) instead of rendering into rects
       that no longer lay out, and drag-to-dock offers no chips (dock_vp_emitted, gui_dock_core.c).
       A host code path that stops running its dockspace thus parks the layout instead of
       corrupting it; only dock_clear destroys it. */
    u32 dock_seen_frame;

    /* Dockspace maximize: one LEAF pinned over the whole dock area (dock_max_id; 0 = none), fully
       obscuring the other tree nodes.  dock_max_on is the logical state -- false while the restore
       tween eases the node back to its tree rect, after which the id clears.  dock_max_settled is
       stamped by dockspace_over_viewport's tween step each emitted frame: only once the cover has
       SETTLED do the obscured nodes' windows suppress (inactive-tab semantics via the route seam)
       and the splitter / placeholder chrome stop emitting -- during the tween the siblings are
       still partially visible and keep drawing.  dock_max_from is the rect tween's FROM (captured
       at toggle); the target re-aims every frame, so a live surface resize is tracked mid-flight.
       Driven by dock_max_set / dock_max_node (gui_dock_core.c). */
    gui_dock_id_t dock_max_id;
    bool          dock_max_on;
    bool          dock_max_settled;
    gui_rect_t    dock_max_from;

} gui_viewport_t;

/*==============================================================================================
    Dock node (behavior in gui_dock.c)

    One node of a viewport's dock tree -- the machinery behind the dock_root seam above.  A node is
    either a LEAF (split == GUI_DOCK_SPLIT_NONE), which tabs one or more windows into a single region, or
    an INTERNAL split (GUI_DOCK_SPLIT_X / _Y), which divides its rect between two children at `ratio` with
    a draggable splitter between them.  Nodes live in a fixed per-context pool (gui_context_t
    dock.pool) so child / parent pool indices (gui_dock_ref_t) stay valid across frames; a freed
    slot has id == 0.

    rect / content are resolved every frame by dock_node_layout from the viewport extent down: rect is
    the node's whole box, content is the leaf's body below its tab strip (where the active window draws).
==============================================================================================*/

typedef enum
{
    GUI_DOCK_SPLIT_NONE = 0,    /* leaf -- tabs windows; child[] unused                 */
    GUI_DOCK_SPLIT_X,           /* internal -- vertical split, children side by side    */
    GUI_DOCK_SPLIT_Y,           /* internal -- horizontal split, children top / bottom  */

} gui_dock_split_t;

typedef struct gui_dock_node_t
{
    gui_id_t    id;                          /* stable node handle; 0 = free pool slot            */
    u32         viewport;                    /* surface (viewport index) this tree belongs to     */
    u8          split;                       /* gui_dock_split_t: NONE = leaf, else internal     */
    f32         ratio;                       /* child[0]'s fraction of the split axis (0.5 default) */

    gui_dock_ref_t parent;       /* owning split, or GUI_DOCK_REF_NONE for the tree root  */
    gui_dock_ref_t child[ 2 ];   /* internal only (GUI_DOCK_REF_NONE on a leaf)           */

    /* Leaf payload: the windows tabbed into this node.  Names are copied at dock time so the tab
       bar is self-sufficient (no dependence on a window emitting this frame or its title lifetime). */

    gui_id_t    tabs [ GUI_DOCK_TABS_MAX ];
    char        names[ GUI_DOCK_TABS_MAX ][ GUI_DOCK_NAME_CAP ];
    u32         tab_count;
    u32         active_tab;                   /* index of the visible tab                          */

    gui_rect_t rect;                       /* whole node box, resolved this frame               */
    gui_rect_t content;                    /* leaf body below the tab strip (active window's rect) */

    /* Floating tab group (gui_dock_float.c): a leaf living OUTSIDE any viewport tree -- windows
       tabbed onto one shared free-floating frame, no split panes.  Not reachable from dock_root,
       so dock_node_layout never touches it: `rect` doubles as its PERSISTED geometry (moved by the
       strip drag, sized by the edge resize), and `z` stacks it among the free windows (a tree node
       draws at z 0 behind them).  Always GUI_DOCK_SPLIT_NONE with parent GUI_DOCK_REF_NONE. */
    bool floating;
    u32  z;

    /* Hidden pane: every window tabbed into this leaf stopped emitting (menu-hidden, X-closed);
       on a split, both children are hidden.  Refreshed at ctx_end (dock_hidden_refresh,
       gui_dock_core.c) from the windows' last_frame stamps; dock_node_layout collapses a hidden
       child to a zero-extent slice so the visible sibling absorbs its space -- tree structure and
       ratio stay untouched, so a window that re-emits gets its exact pane back.  Derived state:
       recomputed every build, never serialized.  Always false on a floating group (not reachable
       from any dock_root; a group tears down through the float paths instead). */
    bool hidden;

} gui_dock_node_t;

/*==============================================================================================
    gui_context_t -- the bound per-context retained state ("bind and use").

    A context is the emission session the code binds once and emits ALL its windows into; it owns
    the state that must persist between frames for that UI.  Every retained access resolves through
    g_ctx via the aliases in gui_ctx.c -- g_ctx->retained, g_ctx->nav, the popup open-set -- so switching
    contexts is a single pointer assignment (ctx_bind): no copy, no backup/restore.

    Ambient state (s_interaction) and frame scratch (s_build, the stacks, s_draw) stay global by design
    -- tier 1 (one physical user) and tier 3 (scratch reused across contexts each frame); see
    ARCHITECTURE.md.  s_io (hardware input snapshot) is always shared.  The `listening` flag gates whether a bound context
    receives hover / click / nav updates -- a deaf context renders but returns inert widget state.
==============================================================================================*/

typedef struct gui_context_t
{
    gui_retained_t   retained;      // id salt, frame clock, keyed state pool (ptr into alloc)
    gui_nav_state_t  nav;           // nav cursor location + menu-bar mode

    struct                          /* popup/ -- the open-popup stack */
    {
        gui_popup_t* open;          // open popup set, ordered parent -> child; ptr into alloc
        u32          open_count;    // live open count
        u32          depth;         // capacity (max nesting depth)
    } popup;

    struct                          /* surface/ + window/ -- the persisted window records */
    {
        gui_window_t* pool;         // persisted window records; ptr into alloc
        u32           count;        // live records in the pool
        u32           max;          // capacity
        gui_window_t  scratch;      // transient fallback when the pool is full; stays embedded
        u32           z_counter;    // monotonic paint-order dispenser
        u32           cascade;      // default-spawn cascade slot (window_default_spawn)
    } win;

    struct                          /* frame/ -- render surfaces */
    {
        gui_viewport_t* pool;       // render surfaces: [0]=main swapchain; ptr into alloc
        u32             count;      // high-water slot count (compacted on close; iterate [0, count))
        u32             max;        // capacity
    } vp;

    struct                          /* dock/ -- the dock-tree node pool */
    {
        gui_dock_node_t* pool;      // dock-tree node pool; NULL when max == 0
        u32              count;     // high-water slot count in the pool
        u32              id_seq;    // monotonic node-id dispenser (0 = none)
        u32              max;       // capacity; 0 = docking disabled
    } dock;

    bool  listening;    // true: context receives hover/click/nav input this frame
    void* _alloc;       // the single ctx_alloc_slot malloc backing this header + all
                        // pool arrays; freed at teardown. Non-NULL for every context,
                        // slot 0 included (freed at shutdown; secondaries at ctx_destroy)
    u32   _alloc_size;  // byte size of the _alloc block; reported by gui_mem_stats()

} gui_context_t;

/*==============================================================================================
    Cross-file forward declarations

    A handful of helpers are called from a file included BEFORE the file that defines them (the
    unity TU resolves the static at link of the single object).  Declaring them here removes the
    hand-placed forward declarations that used to sit in gui.c.
==============================================================================================*/

/* The mouse-input path (core/gui_io.c) resolves an event's app win_id to the viewport hosting it,
   but the viewport pool lives on g_ctx (gui_ctx.c) included later.  Defined after g_ctx. */
static u32 viewport_index_for_window( i32 win_id );

/* OS resize / close events for an gui-OWNED floater are serviced against the viewport pool, so
   gui_event (core/gui_io.c) delegates them here.  Defined in gui_frame.c after g_ctx; returns
   true when win_id is an owned viewport (event consumed). */
static bool gui_owned_window_event( const app_event_t* ev );

/* Interaction gate predicates (interact/gui_item.c) -- the read half of the arbitration state,
   named once so compound gesture gates read as sentences.  Pure queries, no writes: interact/
   stays the only writer of s_interaction. */
static bool interact_idle      ( void );             /* nothing holds the pointer capture      */
static bool interact_held      ( gui_id_t id );      /* id's press-drag gesture is in flight   */
static bool interact_hover_bare( gui_id_t win_id );  /* cursor on win_id, no widget beneath it */

/* The window <-> dock route seam (implemented in dock/gui_dock_route.c).

   window/ is included BEFORE dock/, yet a window must ask the dock whether it owns placement.
   This seam is the ONLY door between them: window/ calls these five verbs and nothing else in
   dock/; a build without docking is a stub implementation of this block.  One verb per protocol
   point in the window lifecycle:

     resolve  (window_begin)  -- service any pending tab-group request, then answer "who places
                                 this window?".  route.node == NULL is the free-float path; a
                                 floating group's active tab also gets its frame resolved (strip
                                 drag / edge resize applied) and the hot edge mask back.
     drag     (free path,     -- preview a drop target (per-node 5-way overlay) while this free
               during drag)      window is title-dragged over a dockspace.
     commit   (window_end,    -- execute the drop computed by drag on the release edge; no-ops
               free path)        unless this is the dragged window releasing.
     chrome   (window_end,    -- tab strip + tabs + node border, drawn in place of a title bar;
               docked path)      reads the current window rect from s_build.
     raise    (press-to-front)-- the dock exception to raise-on-press: true when the window is
                                 dock-managed (a tile never reorders; a floating group raises
                                 its node's z as a whole -- done inside). */

typedef struct
{
    gui_dock_node_t* node;         // NULL = free-float; else the dock leaf that places this window
    bool             active;       // this window is the node's active tab (its body opens)
    u8               resize_hot;   // floating-group hot edge mask for s_scope; 0 otherwise

} gui_win_route_t;

static gui_win_route_t window_route_resolve( gui_id_t id, const char* title, gui_window_t* win );
static void            window_route_drag   ( gui_id_t id, gui_window_t* win );
static void            window_route_commit ( gui_id_t id, const char* title );
static void            window_route_chrome ( gui_dock_node_t* node );
static bool            window_route_raise  ( gui_id_t id );

/* gui_popup.c is included after the widgets/ files; selectable calls this to auto-close the enclosing
   popup on click (the Dear ImGui CloseCurrentPopup default behavior). */
void gui_popup_close_current( void );

/* Compound-widget bracket (interact/gui_item.c) -- the formal seam for "a widget made of
   widgets".  The outer widget runs item_state for ITS id/rect first, then brackets its inner
   emissions so the sub-items do not clobber what the outer item published:

     gui_item_sub_t s = gui_item_sub_begin();     // save s_scope.last_* + flags
     ...inner item_state calls / flag tweaks...   // e.g. |= GUI_ITEM_BUTTON_REPEAT
     gui_item_sub_end( s );                       // restore: is_item_* reports the OUTER widget

   gui_item_sub_layout_begin is the full compound: the same save plus an id scope rooted at the
   outer id and a transient sub-layout over the widget's rect (gui_push_layout_overlay), so the
   body can emit REAL widgets through the normal layout verbs -- the recursive completion of the
   three-seam recipe (cell -> item -> draw) inside one widget.  End with the same
   gui_item_sub_end; it unwinds the layout + id scope when `layout` is set. */

typedef struct
{
    gui_id_t         last_id;      // s_scope.last_* as published by the outer item's item_state
    gui_rect_t       last_rect;
    gui_item_state_t last_status;
    gui_item_flags_t flags;        // s_scope.flags at begin -- inner tweaks stay scoped
    bool             layout;       // full bracket: end also pops the sub-layout + id scope

} gui_item_sub_t;

gui_item_sub_t gui_item_sub_begin( void );
gui_item_sub_t gui_item_sub_layout_begin( gui_id_t id, gui_rect_t r );
void           gui_item_sub_end( gui_item_sub_t s );

/* The size-animate seam (gui_layout_core.c) eases a remembered extent toward its target through the
   animation pool, whose primitive (gui_anim_f32) lives in interact/gui_anim.c -- included AFTER the layout
   files.  Forward-declared so size_animate can reach it across the unity TU. */
static f32 gui_anim_f32( gui_id_t anim_id, f32 target, f32 speed );
static f32 gui_anim_f32_from( gui_id_t anim_id, f32 rest, f32 target, f32 speed );
static gui_anim4_t gui_anim4( gui_id_t id, gui_anim4_t rest, gui_anim4_t target, gui_anim4_t speed );

/* The region engine (compose/gui_scroll.c) emits the scrollbar widget into the gutter
   it reserved at layout_pop_region -- but the widget lives above it (widgets/gui_scrollbar.c),
   included AFTER the layout files.  Forward-declared so the pop can reach it across the unity
   TU: compose hands the track rect + scroll slot; the widget owns the feel and the look. */
static void scrollbar_widget( gui_id_t region_id, gui_rect_t track, bool vertical,
                              f32 content, f32 view, f32* scroll );

/* Edge bits shared by the edge-resize service (interact/gui_resize.c: hit test, grab, apply),
   its highlight painter (present/gui_paint_core.c: draw_resize_highlight), and the tier-4
   consumers.  Combined on a corner grab (e.g. R|B).  GRIP is the CAN_AUTOSIZE corner triangle --
   a resize affordance like the edges, carried in the same s_scope.resize_hot mask (the highlight
   painter ignores it; the R|B edge bits are promoted alongside it so the corner still bolds). */
#define GUI_RESIZE_L     ( 1u << 0 )
#define GUI_RESIZE_R     ( 1u << 1 )
#define GUI_RESIZE_T     ( 1u << 2 )
#define GUI_RESIZE_B     ( 1u << 3 )
#define GUI_RESIZE_GRIP  ( 1u << 4 )

/*==============================================================================================
    Shared stateless helpers

    Small pure scalar/geometry helpers used across both translation units (the UI unit and the
    render backend unit -- gui_emit_draw.c needs rect_intersect for clip nesting).  static inline so
    each TU gets its own copy with no linkage; they touch nothing but their arguments.
==============================================================================================*/

/* Clamp t to [0,1] -- the saturate used by slider + scrollbar drag mapping. */
static inline f32
saturate( f32 t ) { return t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t ); }

/* Clamp v to [lo,hi]. */
static inline f32
clampf( f32 v, f32 lo, f32 hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

/* Overlap of two rects (zero-size when they do not overlap).  Nested regions intersect their
   clip with the parent so a child never scissors or hit-tests past it. */
static inline gui_rect_t
rect_intersect( gui_rect_t a, gui_rect_t b )
{
    f32 x0 = a.x > b.x ? a.x : b.x;
    f32 y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = ( a.x + a.w < b.x + b.w ) ? a.x + a.w : b.x + b.w;
    f32 y1 = ( a.y + a.h < b.y + b.h ) ? a.y + a.h : b.y + b.h;
    f32 w  = x1 - x0 > 0.0f ? x1 - x0 : 0.0f;
    f32 h  = y1 - y0 > 0.0f ? y1 - y0 : 0.0f;
    return ( gui_rect_t ){ x0, y0, w, h };
}

// clang-format on
/*============================================================================================*/
#endif    // GUI_INTERNAL_H
