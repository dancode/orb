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
    imgui_id_t  hover_id;       /* widget under the cursor this frame (rebuilt each frame) */
    imgui_id_t  active_id;    /* widget with the mouse button held (drag / hold)       */
    u8          active_button;  /* which button holds active_id (0=left); reset to 0 on release */
    imgui_id_t  focused_id;   /* widget that owns keyboard input                       */

    /* Window occlusion is resolved one frame deferred: the single window the cursor is
       over (front-most by z) is only known after every window has been submitted.  
       Each begin_window nominates itself into next_hover_win; ctx_new_frame promotes it to
       hover_win.  Next frame a window compares its id against hover_win at entry -- if it
       isn't the hover window it cannot be hit, so it (and all its widgets) skip hit-testing
       entirely.  Only the hover window does widget hit-testing, and within one window
       widgets don't overlap, so widget hover can be resolved immediately (no deferral). */

    imgui_id_t  hover_win;      /* the window the cursor is over (resolved last frame)   */
    imgui_id_t  next_hover_win; /* front-most window nominee gathered this frame         */
    u32         next_hover_win_z;

    imgui_id_t  win_id;             /* id of the window currently between begin/end_window   */
    const char* win_title;          /* title string, cached for end_window's deferred chrome */
    bool        win_collapsed;      /* current window is collapsed (title bar only this frame) */
    imgui_win_flags_t win_flags;    /* current window's behavior flags (begin_window arg)   */
    f32         win_title_h;        /* current window's title bar height (0 if NOTITLEBAR)  */
    u8          win_resize_hot;     /* resize edges hot this frame -- suppresses widget hover */
    struct imgui_window_t* cur_win; /* persisted window record; scroll write-back target */

    f32  win_x, win_y;        /* current window top-left (outer frame)                 */
    f32  win_w, win_h;        /* current window dimensions                             */

    /* Layout pen + scroll region state now live on the layout-frame stack below; the
       window is just the root frame.  s_ctx keeps only the cross-cutting interaction
       state the chrome and widgets read regardless of which region is active. */

    imgui_rect_t clip_rect;   /* active interaction clip -- widget hover is gated by it */
    bool         wheel_used;  /* a region consumed the wheel this frame (innermost wins) */

} s_ctx;

/*----------------------------------------------------------------------------------------------
    Layout-frame stack

    Every scrollable region (a window body or a begin_child box) pushes one frame.  The top
    frame owns the layout pen and the content column the leaf widgets emit into; the rest of
    the struct is the resolve context layout_pop_region needs to measure content and draw the
    region's scrollbars.  The pen fields used to live flat in s_ctx; nesting moved them here.

    Memory is just the fixed array -- a frame carries only what is needed to emit widget rects
    and resolve scroll at pop, so a deep nesting costs nothing beyond these slots.
----------------------------------------------------------------------------------------------*/

#define IMGUI_LAYOUT_DEPTH 16

typedef struct
{
    f32 cursor_x, cursor_y;     /* layout pen, top-left of the next widget (scroll-biased) */
    f32 content_x, content_w;   /* widget-row left edge + available width                  */
    f32 content_max_x;          /* rightmost edge reached this frame -- drives hscroll      */

    /* Resolve context, set at push and read at pop. */
    imgui_id_t   region_id;     /* base id for the region's scrollbar widget ids           */
    imgui_win_flags_t flags;    /* scroll policy bits (IMGUI_WIN_*SCROLL), reused          */
    imgui_rect_t outer;         /* the region box in screen space                          */
    f32          origin_x;      /* unscrolled content origin -- measures content extent     */
    f32          origin_y;
    f32          pad;           /* inner content padding                                    */
    f32          view_w, view_h;/* gutter-adjusted visible extents (must match the bars)    */
    f32          sb_w, sb_h;    /* reserved gutter sizes (0 = no bar this frame)            */
    bool         show_v, show_h;/* a bar is shown this axis                                 */
    bool         pushed_clip;   /* a draw clip was pushed (balance at pop)                  */

    /* Persistent scroll state, owned by the caller (window record or region pool entry). */
    f32* scroll_x;
    f32* scroll_y;
    f32* pcontent_w;            /* write-back: measured content extent for next frame       */
    f32* pcontent_h;

    imgui_rect_t parent_clip;   /* s_ctx.clip_rect to restore at pop                        */

} layout_frame_t;

static layout_frame_t s_layout_stack[ IMGUI_LAYOUT_DEPTH ];
static u32            s_layout_sp;   /* active frame count; top = s_layout_sp - 1 */

/* Top layout frame.  Valid between a begin_window/begin_child and its matching end.  When the
   stack is empty (a caller emitted a widget into a collapsed window despite the false return)
   slot 0 -- which ctx_new_frame leaves zeroed -- is returned instead of indexing out of
   bounds, so the stray widget draws a harmless zero-size rect rather than crashing. */
static layout_frame_t* lf( void ) { return &s_layout_stack[ s_layout_sp ? s_layout_sp - 1 : 0 ]; }

/* Monotonic frame index, bumped each new_frame.  The region pool stamps entries with it and
   recycles the least-recently-seen slot, so transient child ids do not leak the pool. */
static u32 s_frame_counter;

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
    rect_hit -- true when the mouse cursor (from s_io) is inside the given rect
----------------------------------------------------------------------------------------------*/

static bool
rect_hit( imgui_rect_t r )
{
    return s_io.mouse_x >= r.x && s_io.mouse_x < r.x + r.w
        && s_io.mouse_y >= r.y && s_io.mouse_y < r.y + r.h;
}

/*----------------------------------------------------------------------------------------------
    rect_intersect -- the overlap of two rects (zero-size when they do not overlap).
    Nested regions intersect their clip with the parent so a child never spills past it.
----------------------------------------------------------------------------------------------*/

static imgui_rect_t
rect_intersect( imgui_rect_t a, imgui_rect_t b )
{
    f32 x0 = a.x > b.x ? a.x : b.x;
    f32 y0 = a.y > b.y ? a.y : b.y;
    f32 x1 = ( a.x + a.w < b.x + b.w ) ? a.x + a.w : b.x + b.w;
    f32 y1 = ( a.y + a.h < b.y + b.h ) ? a.y + a.h : b.y + b.h;
    f32 w  = x1 - x0 > 0.0f ? x1 - x0 : 0.0f;
    f32 h  = y1 - y0 > 0.0f ? y1 - y0 : 0.0f;
    return ( imgui_rect_t ){ x0, y0, w, h };
}

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
