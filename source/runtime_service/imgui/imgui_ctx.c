/*==============================================================================================

    runtime_service/imgui/imgui_ctx.c -- Immediate-mode context state.

    Tracks hover/active/focused widget IDs and the layout cursor.
    All widget code in imgui_widget.c reads and writes s_ctx directly.

    Included by imgui.c after imgui_input.c so s_io is in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

/* Forward tag: the full definition lives in imgui_window.c (included after this file).
   end_window writes scroll_y / content_h back through this pointer. */
struct imgui_window_t;

static struct
{
    imgui_id_t  hover_id;       // widget under the cursor this frame (rebuilt each frame)
    imgui_id_t  active_id;    // widget with the mouse button held (drag / hold)
    u8          active_button;  // which button holds active_id (0=left); reset to 0 on release
    imgui_id_t  focused_id;   // widget that owns keyboard input

    /* Window occlusion is resolved one frame deferred: the single window the cursor is
       over (front-most by z) is only known after every window has been submitted.  
       Each begin_window nominates itself into next_hover_win; ctx_new_frame promotes it to
       hover_win.  Next frame a window compares its id against hover_win at entry -- if it
       isn't the hover window it cannot be hit, so it (and all its widgets) skip hit-testing
       entirely.  Only the hover window does widget hit-testing, and within one window
       widgets don't overlap, so widget hover can be resolved immediately (no deferral). */

    imgui_id_t  hover_win;      // the window the cursor is over (resolved last frame)
    imgui_id_t  next_hover_win; // front-most window nominee gathered this frame
    u32         next_hover_win_z;

    imgui_id_t  win_id;             // id of the window currently between begin/end_window
    const char* win_title;          // title string, cached for end_window's deferred chrome
    bool        win_collapsed;      // current window is collapsed (title bar only this frame)
    imgui_win_flags_t win_flags;    // current window's behavior flags (begin_window arg)
    f32         win_title_h;        // current window's title bar height (0 if NOTITLEBAR)
    u8          win_resize_hot;     // resize edges hot this frame -- suppresses widget hover
    struct imgui_window_t* cur_win; // persisted window record; scroll write-back target

    f32  win_x, win_y;        // current window top-left (outer frame)
    f32  win_w, win_h;        // current window dimensions

    /* Layout pen + scroll region state now live on the layout-frame stack below; the
       window is just the root frame.  s_ctx keeps only the cross-cutting interaction
       state the chrome and widgets read regardless of which region is active. */

    imgui_rect_t clip_rect;   // active interaction clip -- widget hover is gated by it
    bool         wheel_used;  // a region consumed the wheel this frame (innermost wins)

} s_ctx;

/*----------------------------------------------------------------------------------------------
    Layout-frame stack

    - Every scrollable region (a window body or a begin_child box) pushes one frame.  
    - The top frame owns the layout pen and the content column the leaf widgets emit into.
    - The rest of the struct is the resolve context layout_pop_region needs to measure 
      content and draw the region's scrollbars.
    - The pen fields used to live flat in s_ctx; nesting moved them here.

    - Memory is just the fixed array -- a frame carries only what is needed to emit widget 
      rects and resolve scroll at pop, so a deep nesting costs nothing beyond these slots.
----------------------------------------------------------------------------------------------*/

#define IMGUI_LAYOUT_DEPTH 8    // max nested scroll regions (windows or children)

typedef struct
{
    f32 cursor_x, cursor_y;     // layout pen, top-left of the next widget (scroll-biased)
    f32 content_x, content_w;   // widget-row left edge + available width
    f32 content_max_x;          // rightmost edge reached this frame -- drives hscroll

    /* Active row template (imgui_layout / row sugar).  Persists and repeats: each widget fills
       the next cell, wrapping to a fresh row of the same shape when the columns run out.  A
       region opens with the default -- one flex column, auto height -- so a plain vertical
       stack needs no layout call.  See imgui_layout_t in imgui.h for the unit rule. */

    u32         lay_ncols;                       // column count
    f32         lay_row_h;                       // flow row height: 0 = auto, >0 = pixels
    f32         lay_gap_x, lay_gap_y;            // inter-cell spacing (resolved to a number)
    u32         lay_nrows;                       // row count; 0 => flow mode, else grid

    /* Field split (field_split / field_label_left): a labeled value widget splits its cell into a
       label track + a control track, resolved with the column unit rule.  side 0 = off (the
       label trails the control); 1 = label-left; 2 = label-right. */

    u8          lay_field_side;                  // imgui_label_side_t: 0 off, 1 left, 2 right
    f32         lay_field_label;                 // label track size   (overloaded unit)
    f32         lay_field_control;               // control track size (overloaded unit)

    /* Content alignment (align / layout.align): where a widget's natural-sized content sits in
       its cell.  Persists like the row template; 0 = LEFT | TOP (the original top-left). */

    u8          lay_align;                       // imgui_align_t flags

    /* Resolved cell geometry, computed once when a template is installed (the source track lists
       are not kept -- they are only needed to produce these).  Flow uses cellx/cellw for every
       row; grid uses cellx/cellw x rowy/rowh as the fixed matrix.  cols indexes [0,lay_ncols),
       rows [0,lay_nrows). */

    f32 cellx[ IMGUI_LAYOUT_COLS ];         // resolved cell left edges
    f32 cellw[ IMGUI_LAYOUT_COLS ];         // resolved cell widths
    f32 rowy [ IMGUI_LAYOUT_COLS ];         // resolved cell tops    (grid only)
    f32 rowh [ IMGUI_LAYOUT_COLS ];         // resolved cell heights (grid only)

    /* Iteration cursor.  Flow: (col) walks one row; a wrap advances cursor_y past row_h_cur and
       row_y/row_h_cur describe the live row.  Grid: (col,row) walk the pre-resolved matrix. */

    u32 col;                                // next column to emit (0 = at a row start)
    u32 row;                                // current grid row (with col, walks row-major)
    f32 row_y;                              // top of the current flow row
    f32 row_h_cur;                          // resolved height of the current flow row
    f32 content_y_max;                      // bottom of the content area -- grid band end

    /* Resolve context, set at push and read at pop. */

    imgui_id_t          region_id;          // base id for the region's scrollbar widget ids
    imgui_win_flags_t   flags;              // scroll policy bits (IMGUI_WIN_*SCROLL), reused
    imgui_rect_t        outer;              // the region box in screen space
    f32                 origin_x;           // unscrolled content origin -- measures content extent
    f32                 origin_y;
    f32                 view_w, view_h;     // gutter-adjusted visible extents (must match the bars)
    f32                 sb_w, sb_h;         // reserved gutter sizes (0 = no bar this frame)
    bool                show_v, show_h;     // a bar is shown this axis
    bool                pushed_clip;        // a draw clip was pushed (balance at pop)

    /* Persistent scroll state, owned by the caller (window record or region pool entry). */

    f32*                scroll_x;
    f32*                scroll_y;
    f32*                pcontent_w;         // write-back: measured content extent for next frame
    f32*                pcontent_h;

    imgui_rect_t        parent_clip;        // s_ctx.clip_rect to restore at pop
    u32                 id_restore;         // id-scope depth to restore at pop (see id stack below)

} layout_frame_t;

static layout_frame_t s_layout_stack[ IMGUI_LAYOUT_DEPTH ];
static u32            s_layout_sp;   // active frame count; top = s_layout_sp - 1

/* Top layout frame.  Valid between a begin_window/begin_child and its matching end.  When the
   stack is empty (a caller emitted a widget into a collapsed window despite the false return)
   slot 0 is returned instead of indexing out of bounds -- the stray widget draws into whatever
   the last frame's root region left there rather than crashing.  The read index is also clamped
   to the top slot so an over-deep nesting (capped in layout_push_region) never reads past the
   array. */

static layout_frame_t*
lf( void )
{
    u32 i = s_layout_sp ? s_layout_sp - 1 : 0;
    if ( i >= IMGUI_LAYOUT_DEPTH ) i = IMGUI_LAYOUT_DEPTH - 1;
    return &s_layout_stack[ i ];
}

/* Monotonic frame index, bumped each new_frame.  The keyed state pool below stamps entries with
   it and reclaims slots gone cold, so transient ids do not leak the pool. */
static u32 s_frame_counter;

/*----------------------------------------------------------------------------------------------
    Keyed state pool -- persistent per-id widget state.

    The single store a widget uses to keep a few bytes alive across frames, keyed by its id: a
    region's scroll offset, a tree node's open flag, a combo's popup state.  imgui_state_get hands
    back a stable, zero-on-create pointer to `size` bytes for `id`; the IMGUI_STATE( T, id ) sugar
    casts it to a typed struct.  The contract is the immediate-mode norm -- fetch your state every
    frame you are live and the pointer is stable; an id left unfetched goes cold and its slot is
    recycled, so there is nothing to free and nothing leaks.

    Storage is an open-addressing hash table keyed by id (ids already avalanche, so id & mask is a
    good bucket).  A lookup probes linearly from the home bucket to the first empty slot -- O(1) at
    the low load factor a UI runs at.  A slot untouched for more than one frame is a tombstone the
    next insert on its chain reclaims; reclamation only ever overwrites one occupied slot with
    another, never empties one, so no probe chain is broken and no sweep or free list is needed.
    The "more than one frame" gate (not "one or more") is deliberate: a slot seen last frame may
    still be revisited later this frame, so only entries two+ frames cold are fair game.
----------------------------------------------------------------------------------------------*/

#define IMGUI_STATE_SLOTS 512                       // open-addressing capacity (power of two)
#define IMGUI_STATE_MASK  ( IMGUI_STATE_SLOTS - 1 ) // bucket = id & mask
#define IMGUI_STATE_CAP   32                        // payload bytes per slot (max state struct)

typedef struct
{
    imgui_id_t id;          // 0 = empty slot
    u32        seen_frame;  // frame last touched -- drives stale reclamation
    union                   // force max alignment so any payload struct is correctly aligned
    {
        void* _p;
        f64   _d;
        u8    bytes[ IMGUI_STATE_CAP ];
    } data;

} imgui_state_slot_t;

static imgui_state_slot_t s_state[ IMGUI_STATE_SLOTS ];

/* Stable storage for `id`: the same pointer every frame the id stays live, zeroed the frame it is
   first seen or recycled.  size must fit IMGUI_STATE_CAP.  Never returns NULL. */
static void*
imgui_state_get( imgui_id_t id, u32 size )
{
    ORB_ASSERT( size <= IMGUI_STATE_CAP );
    if ( id == IMGUI_ID_NONE ) id = 1u;             // never key on the empty sentinel
    (void)size;

    u32                 bucket = id & IMGUI_STATE_MASK;
    imgui_state_slot_t* reuse  = NULL;              // first tombstone (cold slot) seen on the chain
    imgui_state_slot_t* dst    = NULL;              // where a fresh entry lands when id is absent

    for ( u32 i = 0; i < IMGUI_STATE_SLOTS; ++i )
    {
        imgui_state_slot_t* s = &s_state[ ( bucket + i ) & IMGUI_STATE_MASK ];

        if ( s->id == id )                          // live hit: restamp and hand back the storage
        {
            s->seen_frame = s_frame_counter;
            return s->data.bytes;
        }
        if ( s->id == IMGUI_ID_NONE )               // empty ends the probe: id is absent
        {
            dst = reuse ? reuse : s;                // reclaim a tombstone if we passed one, else grow
            break;
        }
        if ( !reuse && s_frame_counter - s->seen_frame > 1u )
            reuse = s;                              // two+ frames cold -> reclaimable in place
    }

    /* Absent: settle into dst.  If the table is wall-to-wall live entries (no empty slot and no
       tombstone -- 512 distinct persistent widgets in one frame), clobber the home bucket: a rare
       degradation rather than an overflow.  reuse covers the no-empty-but-some-cold case. */
    if ( !dst ) dst = reuse ? reuse : &s_state[ bucket ];

    dst->id         = id;
    dst->seen_frame = s_frame_counter;
    memset( dst->data.bytes, 0, sizeof dst->data.bytes );
    return dst->data.bytes;
}

/* Typed sugar: a zero-on-create T* persisted by id.  sizeof(T) must be <= IMGUI_STATE_CAP. */
#define IMGUI_STATE( T, id ) ( (T*)imgui_state_get( ( id ), (u32)sizeof( T ) ) )

/*----------------------------------------------------------------------------------------------
    id_hash -- FNV-1a 32-bit hash of a NUL-terminated string
----------------------------------------------------------------------------------------------*/

static imgui_id_t
id_hash( const char* str )
{
    u32 h = 0x811C9DC5u;
    for ( ; *str; ++str )
        h = ( h ^ (u8)*str ) * 0x01000193u;
    return h ? h : 1u;    /* never return IMGUI_ID_NONE (0) */
}

/*----------------------------------------------------------------------------------------------
    id_combine -- mix a scope seed with a local key into one id (boost-style hash_combine).

    The single rule for how an id is namespaced: every sub-id (a leaf widget under a region, a
    child region under its parent, a window's chrome control) is id_combine(scope, key).  Unlike
    a bare XOR it avalanches and is order-dependent, so distinct (scope, key) pairs stay distinct.
----------------------------------------------------------------------------------------------*/

static imgui_id_t
id_combine( imgui_id_t seed, u32 key )
{
    u32 h = seed ^ ( key + 0x9E3779B9u + ( seed << 6 ) + ( seed >> 2 ) );
    return h ? h : 1u;    /* never return IMGUI_ID_NONE (0) */
}

/*----------------------------------------------------------------------------------------------
    Id-scope stack

    The top of this stack is the seed every widget id combines against, so identical labels in
    different scopes never collide.  Regions seed it automatically (layout_push_region pushes the
    region id, layout_pop_region restores), and push_id / pop_id add temporary levels for repeated
    widgets inside one region (e.g. list rows keyed by index).  Reset to empty each frame.

    Over-deep pushes alias the top slot rather than writing past the array, and id_seed clamps its
    read the same way -- mirroring the layout stack, so deep nesting degrades instead of crashing.
----------------------------------------------------------------------------------------------*/

#define IMGUI_ID_STACK_DEPTH 32

static imgui_id_t s_id_stack[ IMGUI_ID_STACK_DEPTH ];
static u32        s_id_sp;

/* Current scope seed -- top of the stack, or NONE when empty (a bare top-level widget). */
static imgui_id_t
id_seed( void )
{
    if ( s_id_sp == 0 ) return IMGUI_ID_NONE;
    u32  i = s_id_sp - 1;
    if ( i >= IMGUI_ID_STACK_DEPTH ) i = IMGUI_ID_STACK_DEPTH - 1;
    return s_id_stack[ i ];
}

static void
id_push( imgui_id_t id )
{
    if ( s_id_sp < IMGUI_ID_STACK_DEPTH )
        s_id_stack[ s_id_sp ] = id;
    ++s_id_sp;    /* count truthfully so push/pop stay paired even past the cap */
}

static void
id_pop( void )
{
    if ( s_id_sp ) --s_id_sp;
}

/*----------------------------------------------------------------------------------------------
    rect_hit -- true when the mouse cursor (from s_io) is inside the given rect
----------------------------------------------------------------------------------------------*/

static bool
rect_hit( imgui_rect_t r )
{
    return s_io.mouse_x >= r.x && s_io.mouse_x < r.x + r.w
        && s_io.mouse_y >= r.y && s_io.mouse_y < r.y + r.h;
}

/* rect_intersect (rect overlap) is a shared geometry helper defined in imgui.c, ahead of the
   unity includes, so imgui_draw.c can use it for clip intersection too. */

/*----------------------------------------------------------------------------------------------
    window_nominate_hover -- begin_window calls this with its rect + z.  Keeps the front-most
    (highest z) window the cursor is over; promoted to hover_win next frame.
----------------------------------------------------------------------------------------------*/

static void
window_nominate_hover( imgui_id_t id, imgui_rect_t r, u32 z )
{
    /* Cheap z test gates the rect_hit; window z is unique so > / >= are equivalent. */
    if ( z >= s_ctx.next_hover_win_z && rect_hit( r ) )
    {
        s_ctx.next_hover_win   = id;
        s_ctx.next_hover_win_z = z;
    }
}

/*----------------------------------------------------------------------------------------------
    ctx_new_frame -- reset per-frame hover state; call at the start of each frame
----------------------------------------------------------------------------------------------*/

static void
ctx_new_frame( void )
{
    /* Widget hover is rebuilt every frame by the hover window's widget calls; clear it now. */
    s_ctx.hover_id = IMGUI_ID_NONE;

    /* Fresh layout stack each frame; no region is open until a begin_window/begin_child.
       The interaction clip starts at the full display, and the wheel is unclaimed -- the
       innermost scrollable region the cursor sits in consumes it (claimed at region pop). */
    s_layout_sp       = 0;
    s_id_sp           = 0;       /* fresh id-scope stack; regions/push_id reseed it */
    s_ctx.wheel_used  = false;
    s_ctx.clip_rect   = ( imgui_rect_t ){ 0.0f, 0.0f, (f32)s_io.display_w, (f32)s_io.display_h };
    ++s_frame_counter;

    /* Promote the window the cursor was over last frame, then start a fresh nomination.
       hover_win lags the cursor by one frame -- the only deferral, and it is what lets the
       front-most window be known before any widget hit-tests this frame. */
    s_ctx.hover_win        = s_ctx.next_hover_win;
    s_ctx.next_hover_win   = IMGUI_ID_NONE;
    s_ctx.next_hover_win_z = 0;

    /* Release active_id once its initiating button is up.  Most grabs use the left button
       (active_button 0); a middle-button window move sets active_button 2 so it releases on
       the middle button instead.  Keep it alive on the release-edge frame (mouse_released)
       so widgets can still observe the press+release pair this frame; it clears on the
       following frame.  Resetting active_button to 0 on release means every left-button grab
       site needs no bookkeeping -- only the middle grab raises it. */
    u8 ab = s_ctx.active_button;
    if ( !s_io.mouse_down[ ab ] && !s_io.mouse_released[ ab ] )
    {
         s_ctx.active_id     = IMGUI_ID_NONE;
         s_ctx.active_button = 0;
    }

    /* Drop keyboard focus on any press; the widget under the cursor re-claims
       it the same frame (input_text sets focused_id from hover_id + press).
       A press on a button or empty space thus leaves focus cleared. */
    if ( s_io.mouse_pressed[ 0 ] )
         s_ctx.focused_id = IMGUI_ID_NONE;
}

// clang-format on
/*============================================================================================*/
