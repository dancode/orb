#ifndef GUI_CORE_H
#define GUI_CORE_H
/*==============================================================================================

    runtime_service/gui/core/gui_core.h -- INTERACT SERVER internals (the core unit).

    The interact server's cross-unit surface: io routing, the id namespace, the keyed state
    pool, the ambient interaction record, the item protocol, the pane/z contest, and the
    retained-state animation utilities.  Knows nothing of style, themes, or drawing.

    One header per section (GUI_SERVER_PLAN.md): every unit .c lists its sub-stack of unit
    headers in stack order (R11), so each header may assume the ones BELOW it (rect, the
    public gui_host.h set) and never the ones above.  The server's retained-mode storage
    (the records + gui_context_t) lives in the companion core/gui_ctx.h, included after this.

==============================================================================================*/

// clang-format off
/*==============================================================================================
    Loud-overflow reporting

    Every fixed pool in the gui follows the same saturation rule: never fail hard, never be
    silent.  The overflowing site degrades gracefully (drop / share / evict) but reports ONCE
    per run so the symptom traces to its cap instead of reading as a rendering or input bug.
    This macro is the report half: printf so the message reaches plain consoles (the engine log
    may not be up yet), fflush so it lands before a follow-up ORB_ASSERT_MSG_ONCE can trap.
==============================================================================================*/

/* GUI_WARN_ONCE moved to rect/gui_rect.h at R11: the render server's pools follow the same
   saturation rule and it must not reach into this header for the report macro -- the leaf
   shared kit is the one header every unit stands on. */

/*==============================================================================================
    Server capacities

    Fixed-array bounds for the interact server's pools.  (GUI_LAYOUT_COLS lives in the public
    gui.h; the flow / chrome / frame caps live in those units' headers; the per-file stack
    depths that are NOT embedded in a shared type stay private to their owning .c file.)
==============================================================================================*/

/* Per-context default pool sizes -- used to wire the static default context (slot 0).
   Secondary contexts may use different sizes passed via gui_ctx_config_t. */

#ifdef GUI_STRESS_TEST

/* Stress-bench build (gui_stress lib): 4x the load-bearing pools; the sensible defaults below
   stay the shipping values.  Popup depth and dock nodes are not load axes and keep theirs. */

#define GUI_DEFAULT_MAX_WINDOWS     128
#define GUI_DEFAULT_STATE_SLOTS     2048

#else

#define GUI_DEFAULT_MAX_WINDOWS     32      // default persisted window pool (32)
#define GUI_DEFAULT_STATE_SLOTS     512     // default keyed state pool capacity

#endif

#define GUI_DEFAULT_POPUP_DEPTH     8       // default max nested popups
#define GUI_DEFAULT_DOCK_NODES      48      // default dock-tree nodes

#define GUI_KEY_COUNT               128     // gui_io_t key arrays; must cover the full app_key_t range

/* Keyed state pool slot payloads -- two size classes over one probe (core/gui_state.c).
   gui_state_get picks the class from the requested size; the caps are asserted against their
   largest tenants in gui_state.c / gui_table.c. */

#define GUI_STATE_TINY_CAP          8       // tiny-class payload bytes (anim dampers/timers, open flags,
                                            //   open_frame stamps -- the one-or-two-word renters)
#define GUI_STATE_CAP               32      // small-class payload bytes (max tenant: gui_region_t --
                                            //   scroll link + user_w/h + the anchor tail-follow pair)
#define GUI_STATE_BIG_CAP           96      // big-class payload bytes (max tenant: gui_table_persist_t)
#ifdef GUI_STRESS_TEST
#define GUI_STATE_BIG_SLOTS         128     // stress-bench build: 4x
#else
#define GUI_STATE_BIG_SLOTS         32      // big-class capacity (tables are the main tenant)
#endif

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

extern gui_io_t s_io;               /* core/gui_io.c -- the frame's distilled input */
extern bool s_viewport_dirty;       /* core/gui_io.c -- a floater surface resized; set by the
                                       frame unit (gui_owned_window_event), consumed by
                                       io_frame_begin's dirty check */

/*==============================================================================================
    Widget interaction kind (behavior in core/gui_item.c)

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
    Ambient interaction state -- the one live hover / active / focus, persisting across frames.

    One pointer, one keyboard, one mouse, so none of it is per-viewport or per-context: a single
    global shared by every context, into which listening contexts nominate hover / active during
    their emit.  Tier: ambient singular (see ARCHITECTURE.md sec 1, state tiers).  Field story
    lives with the definition site (core/gui_ctx.c).  interact/ stays the only WRITER of the
    arbitration fields; everything else reads.
==============================================================================================*/

typedef struct
{
    gui_id_t  hover_id;         // widget under the cursor this frame (rebuilt each frame)
    gui_id_t  active_id;        // widget with the mouse button held (drag / hold)
    gui_id_t  active_id_prev;   // active_id as of the end of the previous frame
    u8        active_button;    // which button holds active_id (0=left); reset to 0 on release
    gui_id_t  focused_id;       // widget that owns keyboard input
    gui_id_t  focused_win;      // window that owns focused_id (exclusive-scope focus lock)
    bool      focus_has_selection; // focused text field holds a live selection this frame

    f32       repeat_t;         // auto-repeat: held time since last fire
    bool      repeat_on;        // initial delay elapsed -> faster rate

    gui_id_t  hover_win;        // the window the cursor is over (resolved last frame)
    gui_id_t  next_hover_win;   // front-most window nominee gathered this frame
    u32       next_hover_win_z;

    app_cursor_t mouse_cursor;  // hardware-cursor nominee; last writer wins, flushed next frame

    gui_id_t  focused_id_at_frame_start;   // focus-departure tracking for
    bool      focused_id_edited;           //   is_item_deactivated_after_edit -- see the
    gui_id_t  focus_ended_id;              //   state block in core/gui_ctx.c
    bool      focus_ended_edited;

} gui_interaction_t;

extern gui_interaction_t s_interaction;   /* core/gui_ctx.c -- hover / active / focus */

/*==============================================================================================
    Interaction scope (the s_scope instance in core/gui_ctx.c)

    The declared contract between composition and behavior.  Everything item_state
    (core/gui_item.c) consumes about "where is this item emitting" lives here, nowhere
    else: composition stamps the record at its seams (window_begin / child_begin / popup / table
    stamp win + clip; the resize/grip resolvers stamp the chrome suppression; the emit seam
    cell_next_w latches the per-item flags + nav stamp), and behavior reads only this
    record plus its own s_interaction -- never the composer scratch (s_build).  Behavior
    publishes its result back into the last_* fields, where the item-query readers
    (core/gui_query.c), drag sourcing, and context-menu anchoring pick it up.  Unlike the
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

extern gui_scope_t s_scope;     /* core/gui_ctx.c -- composition->behavior scope */

/*==============================================================================================
    Keyed state pool slots (core/gui_state.c)

    The single store a widget uses to keep bytes alive across frames, keyed by its id (a
    region's scroll, a tree node's open flag, a combo's popup state, a table's column widths).
    Three open-addressing hash tables -- a tiny class (the hot one-or-two-word renters: anim
    dampers / timers, open flags), a small class, and a big class -- walked by the one probe in
    gui_state.c, which also holds the reclamation contract and picks the class from the
    requested size.  All slot types share the (id, seen_frame) header prefix the probe reads.
    Tiny slots are 16 bytes (4 per cache line), which is the point: lookup cost is probe-chain
    cache misses, and the tiny class carries the highest-traffic tenants.
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
    u8          data[ GUI_STATE_TINY_CAP ]; // payload; 16-byte slot, 4 per cache line

} gui_state_tiny_slot_t;

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

    /* The three class tables, all pointing into the context alloc.  Counts need not be powers
       of two: the probe picks its home bucket by multiply-shift range reduction and wraps by
       increment (gui_state.c), so the partition can be tuned freely (small = 3/4 of tiny). */

    gui_state_tiny_slot_t* state_tiny;  // tiny class (state_slots slots)
    u32                    tiny_count;
    gui_state_slot_t*      state;       // small class (3/4 of state_slots)
    u32                    state_count;
    gui_state_big_slot_t*  state_big;   // big class (GUI_STATE_BIG_SLOTS)
    u32                    big_count;

} gui_retained_t;

/*==============================================================================================
    Server service seams -- identity, io, state, item protocol, pane contest, anim utilities
==============================================================================================*/

/* identity (core/gui_id.c) -- the id namespace verbs. */
gui_id_t id_hash   ( const char* str );   /* FNV-1a of the full string */
gui_id_t id_combine( gui_id_t seed, u32 key );
gui_id_t id_seed   ( void );
void     id_push   ( gui_id_t id );
void     id_pop    ( void );
extern u32 s_id_sp;                       /* core/gui_ctx.c -- id-scope stack pointer */

/* io (core/gui_io.c) -- modifier reads, key claims, outbound clipboard, and the input-frame
   bracket the orchestrator drives (gui_frame_begin/end pump the io frame into this server). */
bool io_shift( void );
bool io_ctrl ( void );
bool io_alt  ( void );
bool key_claim( app_key_t k );                        /* claim a key edge      */
void gui_clipboard_set( const char* s, u32 n );       /* outbound clipboard    */
void io_frame_begin( i32 win_w, i32 win_h, f32 dt );  /* sample polled input   */
void io_frame_end  ( void );                          /* clear one-frame input */
bool io_dirty      ( void );                          /* any input change this frame */

/* keyed state pool (core/gui_state.c) + the typed sugar over it.  gui_state_get: zero-on-create
   T* persisted by id; gui_state_peek: read-only, non-allocating, non-stamping probe (NULL when
   absent).  sizeof(T) must be <= GUI_STATE_BIG_CAP. */
void*       gui_state_get ( gui_id_t id, u32 size );
const void* gui_state_peek( gui_id_t id, u32 size );
#define GUI_STATE( T, id )      ( (T*)gui_state_get( ( id ), (u32)sizeof( T ) ) )
#define GUI_STATE_PEEK( T, id ) ( (const T*)gui_state_peek( ( id ), (u32)sizeof( T ) ) )

/* frame scratch accessors + item seams (core/gui_ctx.c).  The flag seams are the PURE halves;
   the style/draw application wrappers keeping the old names (item_flags_resolve,
   item_flags_chrome_reset) live in element/gui_adornment.c and are declared in
   style/gui_style.h -- this server never touches a style value or the draw state. */
bool             rect_hit( gui_rect_t r );         /* cursor (s_io) inside r                  */
gui_item_flags_t item_flags_take( void );          /* per-item flag merge + scope latch       */
void             item_flags_chrome_drop( void );   /* clear the item scope at chrome seams    */
void             item_flag_push( gui_item_flags_t flag, bool enable );   /* bracketing stack  */
void             item_flag_pop ( void );
void             item_flag_next( gui_item_flags_t flag, bool enable );   /* one-shot override */
void             cursor_set( app_cursor_t c );     /* hardware-cursor nomination              */
void             item_mark_edited( void );         /* focus edit latch                        */

extern bool s_replay_mode;          /* core/gui_ctx.c -- volatile idle-replay phase flag      */
extern u32  s_popup_begin_count;    /* core/gui_ctx.c -- popup nesting depth (per-frame)      */

/* Interaction gate predicates (core/gui_item.c) -- the read half of the arbitration state,
   named once so compound gesture gates read as sentences.  Pure queries, no writes: interact/
   stays the only writer of s_interaction.  Non-static since the TU split (the flow unit's
   region wheel gate reads interact_idle). */
bool interact_idle      ( void );             /* nothing holds the pointer capture      */
bool interact_held      ( gui_id_t id );      /* id's press-drag gesture is in flight   */
bool interact_hover_bare( gui_id_t win_id );  /* cursor on win_id, no widget beneath it */
void interact_claim( gui_id_t id, u8 button );/* claim the capture -- the one door for a
                                                 higher tier to start a press-drag (R6)  */

/* Exclusive input mode (focus scope) -- true while a GUI_WIN_MODAL window is live (emitted this
   frame or last).  Defined in core/gui_ctx.c; read by focus_allowed (core/gui_item.c) to
   confine focus to the mode.  See the exclusive-mode block in gui_ctx.c for the full model. */
static bool gui_modal_scope_live( void );

/* the item protocol (core/gui_item.c) -- the behavior seam every widget rides. */
gui_item_state_t item_state( gui_id_t id, gui_rect_t r, gui_item_kind_t kind );
void             item_focus_release( void );
void             nav_item_stamp_label( gui_id_t id, const char* label );

/* chrome grab + modal fence (core/gui_item.c). */
bool item_grab( gui_id_t id, gui_rect_t r, bool gate, bool* active );
void interact_hover_fence( gui_id_t owner );

/* Compound-widget bracket (core/gui_item.c) -- the formal seam for "a widget made of
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

/* Widget label grammar -- the id half (core/gui_id.c since R4: a label's id is identity
   derivation).  "Text##key" displays "Text" with a distinct id; "###key" re-roots the id hash. */
gui_id_t    item_id( const char* label );        /* label -> widget id per the grammar        */
u32         label_vis_len( const char* s );      /* visible byte count (up to the first "##") */
const char* label_id_str( const char* s );

/* the surface service (core/gui_surface.c) -- the hover contest and the z band map every
   stacked entity's z lives in (see the band story at the definitions).  The pane BRACKET
   (pane_tag, gui_pane_begin/end) lives with the frame orchestrator (frame/gui_pane.c): it
   stamps BOTH servers, which neither server may do itself.  pane_tag is declared here --
   the go-between verb's consumers (chrome's window opens, flow's region opens) all sit on
   this header. */
void surface_hover_nominate( gui_id_t id, gui_rect_t r, u32 z, u32 viewport );
u32  surface_z_raise( u32 z );
u32  surface_z_overlay( u32 depth );
void pane_tag( gui_id_t id, u32 z, u32 vp, u32 band );   /* defined frame/gui_pane.c */

#define GUI_REGION_BG_Z  0x00000000u
#define GUI_REGION_Z     0x40000000u
#define GUI_Z_OVERLAY    0x80000000u
#define GUI_REGION_FG_Z  0xF0000000u

/* retained-state animation utilities (core/gui_anim.c since R4: dampers and timers are
   keyed-state tenants, server-side so style blends and interact tweens share them). */
typedef f32 ( *gui_ease_fn )( f32 );
f32  gui_anim_timer( gui_id_t id, gui_ease_fn ease, bool* out_active );
void gui_anim_timer_start( gui_id_t id, f32 duration );
f32  gui_anim_f32( gui_id_t anim_id, f32 target, f32 speed );
f32  gui_anim_f32_from( gui_id_t anim_id, f32 rest, f32 target, f32 speed );
gui_anim4_t gui_anim4( gui_id_t id, gui_anim4_t rest, gui_anim4_t target, gui_anim4_t speed );

/* The timer slot payload -- shared so the feat_* kit (interact/gui_feature.c) can PEEK a
   timer's remaining duration without starting one. */
typedef struct { f32 elapsed; f32 duration; } gui_anim_timer_t;

/* Public vtable adapters over the anim utilities (core/gui_anim.c); wired by gui_api.c. */
f32        gui_api_anim_ease ( gui_id_t id, gui_ease_t ease, bool* out_active );
u32        gui_api_anim_color( gui_id_t id, u32 target_abgr, f32 speed );
gui_vec2_t gui_api_anim_vec2 ( gui_id_t id, gui_vec2_t target, f32 speed );
gui_rect_t gui_api_anim_rect ( gui_id_t id, gui_rect_t target, f32 speed );

/* Keyed-state pool usage introspection (core/gui_state.c) -- a full-table walk; the perf
   overlay (frame unit) calls it when displaying, not per frame. */
typedef struct
{
    u32 tiny_live,  tiny_used,  tiny_cap;
    u32 small_live, small_used, small_cap;
    u32 big_live,   big_used,   big_cap;

} gui_state_usage_t;

gui_state_usage_t gui_state_usage( void );

/*==============================================================================================
    Upward seams -- the interact server's few documented calls above its layer (the inc-10
    discipline: explicit and few, listed here so the whole set is visible at once).

    draw_nav_ring    the ONE system-adornment paint this server invokes (nav_item_register,
                     core/gui_item.c): the ring must land beneath the item's own fill and no
                     presentation seam after behavior exists that every widget passes through.
                     Behavior picks the MOMENT; the paint policy (color, thickness) lives with
                     the skin (element/gui_adornment.c since R8).  Do not add more.
    nav_scroll_chase the keyboard scroll-into-view (nav_item_register, core/gui_item.c):
                     walking the open region stack and moving scroll offsets is composition
                     machinery, so the act lives with flow (flow/gui_scroll.c) and behavior
                     only picks the moment -- the same split as draw_nav_ring (R11).
    gui_owned_window_event   the io pump's ONE call up into its orchestrator (frame unit):
                     OS resize / close events for a gui-OWNED floater are serviced against
                     the viewport pool the orchestrator manages.  Returns true when win_id
                     is an owned viewport (event consumed).  Defined in frame/gui_frame_loop.c.
    DBG_* / STEP_SET_OWNER   debug capture stamps (debug/gui_debug.h) -- severable tooling,
                     compiled away outside Debug.
==============================================================================================*/

#define NAV_RING 2.0f   /* focus-ring inset outside the item rect; the nav scroll chase keeps
                           the ring clear of the view edge with it */
void draw_nav_ring( gui_rect_t r, bool captured );
void nav_scroll_chase( gui_rect_t r );
bool gui_owned_window_event( const app_event_t* ev );

/* Decentralized memory accounting: the core unit's fixed statics (ambient records, io snapshot,
   id/flag stacks, context pool array), summed at the foot of gui_core.c for gui_ui_memory. */
u32 gui_core_unit_mem_bytes( void );

// clang-format on
/*============================================================================================*/
#endif    // GUI_CORE_H
