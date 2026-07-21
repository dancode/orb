#ifndef GUI_CTX_H
#define GUI_CTX_H
/*==============================================================================================

    runtime_service/gui/core/gui_ctx.h -- the context aggregate (the core unit, late half).

    gui_context_t embeds records from most units by value (nav, popups, windows, viewports,
    dock nodes), so this header closes the include stack: it is the LAST type header
    gui_internal.h pulls, after chrome and frame.  The entanglement is the known hotspot the
    campaign shrinks increment by increment (GUI_SERVER_PLAN.md R4: the pane re-shape).

==============================================================================================*/

// clang-format off

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

    struct                          /* window/ -- GUI_WIN_MODAL fence (window_modal_apply) */
    {
        gui_id_t     win_id;        // id of the modal overlay window; 0 = none
        u32          seen_frame;    // frame it last emitted -- fence lapses when it stops
    } modal;

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

/* Frame-build scratch -- the "where am I emitting right now" context, rebuilt every frame as
   the widget tree is walked.  Nothing survives begin_frame; contexts build sequentially on one
   thread, so it stays a single global builder.  Field story at the definition (core/gui_ctx.c). */
typedef struct
{
    gui_win_ctx_t win;          // the window currently between window_begin / window_end

    bool          wheel_used;   // a region consumed the wheel this frame (innermost wins)

    u32           nav_region_seq;   // per-frame region dispenser (layout_seed_content)
    u32           nav_line_seq;     // per-frame line dispenser (a line-open takes the next)

    gui_item_flags_t item_flags;    // merged top-of-stack item flags
    gui_item_flags_t next_set;      // bits the next-item override controls
    gui_item_flags_t next_val;      // their values

    bool          combo_open;          // a combo dropdown body is currently being emitted
    bool          combo_item_clicked;  // a selectable in that body was clicked this frame

} gui_build_t;

extern gui_context_t* g_ctx;      /* core/gui_ctx.c -- the bound context     */
extern gui_build_t    s_build;    /* core/gui_ctx.c -- frame-build scratch   */

// clang-format on
/*============================================================================================*/
#endif    // GUI_CTX_H
