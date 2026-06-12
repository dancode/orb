/*==============================================================================================

    runtime_service/imgui/imgui_widget_core.c -- Shared widget primitives + theme.

    The foundation the rest of the widget layer is built on: the layout-derived size
    macros, the color palette, the per-widget interaction state machine, and the layout
    cursor helpers.  Both the leaf widgets (imgui_widget.c) and the window chrome
    (imgui_widget_window.c) draw and interact through these, so they live here, ahead of
    both in the unity build.

    Interaction uses the classic hover/active/focused model:
        hover   : the cursor is over the widget this frame
        active  : the primary button is held with this widget as the target
        focused : this widget owns keyboard input (input_text)

    Included by imgui.c after imgui_window.c so s_ctx, s_io, s_layout, rect_hit, and the
    draw helpers are all in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Layout accessors  (read from s_layout, computed by layout_compute() in imgui.c)
----------------------------------------------------------------------------------------------*/

#define WIDGET_H      ( (f32)s_layout.line_size     )
#define WIDGET_GAP    ( (f32)s_layout.widget_gap    )
#define WIDGET_PAD    ( (f32)s_layout.widget_pad    )
#define WIN_TITLE_H   ( (f32)s_layout.win_title_h   )
#define WIN_BORDER    ( (f32)s_layout.win_border    )
#define CHECKBOX_SZ   ( (f32)s_layout.checkbox_sz   )
#define SLIDER_KNOB_W ( (f32)s_layout.slider_knob_w )

/*----------------------------------------------------------------------------------------------
    Color palette (IMGUI_COLOR: byte order R,G,B,A in memory = ABGR u32)
----------------------------------------------------------------------------------------------*/

#define COL_WIN_BG       IMGUI_COLOR( 0x24, 0x24, 0x24, 0xE8 )
#define COL_TITLE_BG     IMGUI_COLOR( 0x10, 0x60, 0xA0, 0xFF )
#define COL_BORDER       IMGUI_COLOR( 0x80, 0x80, 0x80, 0xFF )
#define COL_TEXT         IMGUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF )
#define COL_TEXT_DIM     IMGUI_COLOR( 0xA0, 0xA0, 0xA0, 0xFF )
#define COL_WIDGET_BG    IMGUI_COLOR( 0x40, 0x40, 0x40, 0xFF )
#define COL_WIDGET_HOT   IMGUI_COLOR( 0x50, 0x80, 0xB0, 0xFF )
#define COL_WIDGET_ACT   IMGUI_COLOR( 0x30, 0x60, 0x90, 0xFF )
#define COL_WIDGET_FG    IMGUI_COLOR( 0x20, 0x90, 0xD0, 0xFF )
#define COL_CHECK_MARK   IMGUI_COLOR( 0x20, 0xC0, 0x60, 0xFF )
#define COL_SLIDER_TRACK IMGUI_COLOR( 0x30, 0x30, 0x30, 0xFF )
#define COL_RESIZE_HOT   IMGUI_COLOR( 0x40, 0xA0, 0xF0, 0xFF )   /* bold edge line when a resize border is hot */
#define COL_INPUT_BG     IMGUI_COLOR( 0x38, 0x38, 0x38, 0xFF )
#define COL_INPUT_FOCUS  IMGUI_COLOR( 0x20, 0x50, 0x70, 0xFF )
#define COL_CURSOR       IMGUI_COLOR( 0xF0, 0xF0, 0x50, 0xFF )

/*----------------------------------------------------------------------------------------------
    Layout cursor helpers
----------------------------------------------------------------------------------------------*/

static f32 widget_right( void ) { return s_ctx.content_x + s_ctx.content_w; }

/* Grow the window's measured horizontal extent to include a widget that draws out to
   right_x (in screen coords).  end_window cancels the scroll bias and compares the total
   against the view to decide the horizontal scrollbar, mirroring how cursor_y travel
   measures content height.  Most widgets fill content_w, but text draws its natural width
   and can reach past the view -- that overflow is what the horizontal bar scrolls. */
static void
widget_track_width( f32 right_x )
{
    if ( right_x > s_ctx.content_max_x )
        s_ctx.content_max_x = right_x;
}

/* Begin a new widget row; returns the rect for the full widget. */
static imgui_rect_t
widget_next_rect( f32 h )
{
    imgui_rect_t r = {
        .x = s_ctx.content_x,
        .y = s_ctx.cursor_y,
        .w = s_ctx.content_w,
        .h = h,
    };
    s_ctx.cursor_y += h + WIDGET_GAP;
    widget_track_width( r.x + r.w );   /* baseline extent: a full-width row reaches the view edge */
    return r;
}

/*----------------------------------------------------------------------------------------------
    Interaction state machine
----------------------------------------------------------------------------------------------*/

/* Interaction class for a widget, selected at the call site.  Only the press-time
   behavior differs between widgets; everything else (hover/active/click) is uniform. */
typedef enum
{
    WIDGET_KIND_BUTTON    = 0,   /* press captures active; reports clicked   */
    WIDGET_KIND_DRAG      = 1,   /* press captures active; held for dragging */
    WIDGET_KIND_FOCUSABLE = 2,   /* press also claims keyboard focus         */

} widget_kind_t;

/* Result of one frame of interaction for a widget.  Every widget drives its
   visuals and value changes from these flags instead of touching s_ctx directly. */
typedef struct
{
    bool hover;       /* cursor is over the widget this frame                  */
    bool active;    /* primary button held with this widget as the target    */
    bool pressed;   /* primary button went down on the widget this frame     */
    bool clicked;   /* press + release completed with the cursor still over  */
    bool focused;   /* widget owns keyboard input (focusable widgets)        */

} widget_state_t;

/* Unified hover/active/focus/click state machine.  Call once per widget with the
   hit rect and the desired interaction kind; the returned flags are all a widget
   needs for drawing and value updates. */

static widget_state_t
widget_behavior( imgui_id_t id, imgui_rect_t r, widget_kind_t kind )
{
    widget_state_t st = { 0 };

    /* Hot only when this widget belongs to the window the cursor is over (hover_win,
       resolved last frame).  Widgets in any other window short-circuit before rect_hit,
       so occluded windows do no hit-testing at all -- occlusion is decided once, at the
       window level, not per widget.

       Modal-while-dragging: once any item owns active_id (a slider, scrollbar, or window
       drag is in flight) every other item is frozen -- only the active item may hover.
       The active item keeps interacting through st.active below, which reads active_id
       directly, so a drag stays live while the cursor sweeps over inert neighbours. */
    bool can_hover = ( s_ctx.active_id == IMGUI_ID_NONE || s_ctx.active_id == id );
    if ( can_hover && s_ctx.win_id == s_ctx.hover_win && !s_ctx.win_resize_hot && rect_hit( r ) )
        s_ctx.hover_id = id;

    /* Press: capture active (and focus for focusable widgets) on button-down. */
    if ( s_ctx.hover_id == id && s_io.mouse_pressed[ 0 ] )
    {
        s_ctx.active_id = id;
        st.pressed      = true;
        if ( kind == WIDGET_KIND_FOCUSABLE )
            s_ctx.focused_id = id;
    }

    st.hover   = ( s_ctx.hover_id == id );
    st.active  = ( s_ctx.active_id == id );
    st.focused = ( s_ctx.focused_id == id );
    st.clicked = s_io.mouse_released[ 0 ] && s_ctx.hover_id == id && s_ctx.active_id == id;

    return st;
}

/* Background color for a pushbutton / knob style widget, from its interaction state. */
static u32
widget_bg_color( widget_state_t st )
{
    if ( st.active ) return COL_WIDGET_ACT;
    if ( st.hover  ) return COL_WIDGET_HOT;
    return COL_WIDGET_BG;
}

// clang-format on
/*============================================================================================*/
