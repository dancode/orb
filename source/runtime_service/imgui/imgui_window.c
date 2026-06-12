/*==============================================================================================

    runtime_service/imgui/imgui_window.c -- Persistent per-window state.

    begin_window()'s x/y/w/h arguments are the window's *initial* geometry, used only
    the first frame a given title is seen.  From then on this registry owns the window's
    position so it survives across frames -- the foundation for dragging (now) and for
    collapse / scroll / saved-layout state (later).  Windows are keyed by id_hash(title).

    Drag interaction itself lives in imgui_widget.c alongside begin_window / end_window,
    where the layout dimensions (title-bar height, padding) are in scope.  This file owns
    the table, the active drag mode, the in-flight drag offset, and raise-to-front.

    Included by imgui.c after imgui_ctx.c so s_ctx (hover_win) and s_io are in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    State
----------------------------------------------------------------------------------------------*/

#define IMGUI_MAX_WINDOWS 64

/* One persisted window.  Geometry is owned here after the first appearance.
   The struct is tagged so imgui_ctx.c (included earlier) can hold a pointer to it. */
typedef struct imgui_window_t
{
    imgui_id_t id;              /* id_hash(title); 0 = free slot                  */
    f32        x, y;            /* persisted top-left (updated by dragging)       */
    f32        w, h;            /* persisted dimensions                           */
    u32        z;               /* paint order: higher = more recently raised = in front */

    f32        scroll_y;        /* vertical scroll offset; 0 = top                */
    f32        content_h;       /* total content height measured last frame       */

    bool       collapsed;       /* title-bar-only when set; toggled by the arrow  */

    imgui_win_flags_t flags;    /* behavior flags supplied to begin_window        */

} imgui_window_t;

static imgui_window_t       s_windows[ IMGUI_MAX_WINDOWS ];
static u32                  s_window_count;
static imgui_window_t       s_window_scratch;   /* fallback when the table is full (not persisted) */

/* Monotonic z dispenser.  Each new or raised window takes the next value, so the
   most recently raised window always has the highest z and sorts to the front. */

static u32                  s_z_counter;

/* Drag configuration + in-flight drag offset (mouse - window pos at grab time).
   The window currently being dragged is tracked via s_ctx.active_id == window id. */

static imgui_win_drag_t     s_win_drag_mode = IMGUI_WIN_DRAG_TITLEBAR;
static f32                  s_drag_off_x, s_drag_off_y;

/* In-flight edge resize.  The window being resized holds active_id == (id ^ RESIZE_SALT);
   s_resize_edges names which edges follow the cursor (IMGUI_RESIZE_* bits, set in
   imgui_widget.c).  s_resize_off keeps the grabbed edge under the cursor without a jump;
   s_resize_fix pins the opposite edge so a left/top drag grows from the far side. */

static u8                   s_resize_edges;
static f32                  s_resize_off_x, s_resize_off_y;
static f32                  s_resize_fix_x, s_resize_fix_y;

/*----------------------------------------------------------------------------------------------
    window_get -- find the window for this id, or create it from the initial geometry.
    Never returns NULL; an overflowing table falls back to a transient scratch entry.
----------------------------------------------------------------------------------------------*/

static imgui_window_t*
window_get( imgui_id_t id, f32 x, f32 y, f32 w, f32 h )
{
    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == id )
            return &s_windows[ i ];

    /* First time seen: seed from the caller's initial geometry, place on top. */
    imgui_window_t* win = ( s_window_count < IMGUI_MAX_WINDOWS )
                        ? &s_windows[ s_window_count++ ]
                        : &s_window_scratch;   /* table full: transient, not persisted */
    win->id        = id;
    win->x         = x;
    win->y         = y;
    win->w         = w;
    win->h         = h;
    win->z         = ++s_z_counter;
    win->collapsed = false;   /* reset matters only for a reused scratch slot */
    return win;
}

/*----------------------------------------------------------------------------------------------
    window_raise_on_press -- a press brings the window under the cursor to the front.

    hover_win (the window the cursor is over) was resolved last frame, so this runs at the
    top of the frame -- before any begin_window stamps its z -- and the raise therefore
    takes effect this same frame: clicking a window's exposed area brings it up at once.
    Called from imgui_new_frame() right after ctx_new_frame() promotes hover_win.
----------------------------------------------------------------------------------------------*/

static void
window_raise_on_press( void )
{
    /* Either button raises: left for the normal click/drag, middle for the convenience move
       grab (imgui_widget.c end_window), so a middle-grabbed window also comes to the front. */
    if ( ( !s_io.mouse_pressed[ 0 ] && !s_io.mouse_pressed[ 2 ] )
         || s_ctx.hover_win == IMGUI_ID_NONE )
        return;

    for ( u32 i = 0; i < s_window_count; ++i )
        if ( s_windows[ i ].id == s_ctx.hover_win )
        {
            /* Already on top?  Don't burn a z value. */
            if ( s_windows[ i ].z != s_z_counter )
                s_windows[ i ].z = ++s_z_counter;
            break;
        }
}

/*----------------------------------------------------------------------------------------------
    set_window_drag -- select the global drag mode; call between frames.
----------------------------------------------------------------------------------------------*/

void
imgui_set_window_drag( imgui_win_drag_t mode )
{
    s_win_drag_mode = mode;
}

// clang-format on
/*============================================================================================*/
