#ifndef IMGUI_H
#define IMGUI_H
/*==============================================================================================

    runtime_service/imgui/imgui.h -- imgui module types.
    Include in DLL modules that use imgui through the vtable (imgui()->...).
    Include imgui_host.h instead for direct-call access (host, sandbox).

==============================================================================================*/

#include "orb.h"

// clang-format off
/*==============================================================================================
    ID / geometry
==============================================================================================*/

typedef u32 imgui_id_t;
#define IMGUI_ID_NONE 0u

typedef struct { f32 x, y; }        imgui_vec2_t;
typedef struct { f32 x, y, w, h; }  imgui_rect_t;

/* Edge insets, in pixels.  Region padding -- the gap between a region's box and where its layout
   starts (see imgui_pad).  Breathing room *inside* a widget's frame is a per-widget style concern
   (WIDGET_PAD), not a layout one; spacing *between* cells is gap_x / gap_y. */

typedef struct { f32 l, r, t, b; }  imgui_pad_t;

/*==============================================================================================
    Layout template

    A region (a window body or a begin_child box) lays widgets out by carving its content area
    into cells.  imgui_layout() installs a template that *persists and repeats*: every widget
    fills the next cell.  The default -- a single flex column of auto height -- is the classic
    vertical stack, so existing code needs no changes.

    Two modes, chosen by whether `rows` is set:

      Flow  (rows empty)  -- `cols` describe one row; it repeats *downward*, the pen accumulates,
                             and content grows + scrolls.  The everyday lists / forms / panels.

      Grid  (rows set)    -- `cols` x `rows` partition a *bounded* box (the region's content area
                             from the current pen to its bottom) into a fixed matrix, resolved up
                             front.  Widgets fill cells row-major; both axes are fixed, nothing
                             scrolls.  Titlebars, toolbars, split panes, dashboards, image grids.

    Column / row sizes use one overloaded f32 (the same rule on both axes):
        > 1.0       fixed pixels
        (0.0, 1.0]  fraction of the gap-adjusted available extent
        == 0.0      flex -- an equal share of whatever space is left
        <  0.0      IMGUI_END, the track-list terminator

    Gaps sit *between* cells and are subtracted before distribution, so a widget never sees or
    reasons about spacing -- it just fills the rect it is handed.
==============================================================================================*/

#define IMGUI_LAYOUT_COLS 8                     // max tracks on one axis (columns or rows)
#define IMGUI_END (-1.0f)                       // track-list terminator (any negative value)s

/*----------------------------------------------------------------------------------------------
    Content alignment -- where a widget's natural-sized content sits inside the cell it is handed.

    Two independent axes, ORed together; 0 (LEFT | TOP) is the default and matches the original
    behavior.  A region carries one alignment (imgui()->align, or the `align` field of a layout
    descriptor), persisting like the row template until changed.  It governs *content* placement
    (a text label, an image) -- a widget whose frame fills the cell (button, input) still fills it,
    and only its label/glyphs follow the alignment.  rect_align() is the single placement seam.
----------------------------------------------------------------------------------------------*/

typedef enum
{
    IMGUI_ALIGN_LEFT    = 0,            /* horizontal: against the left edge (default)  */
    IMGUI_ALIGN_HCENTER = 1 << 0,       /* horizontal: centered                         */
    IMGUI_ALIGN_RIGHT   = 1 << 1,       /* horizontal: against the right edge           */

    IMGUI_ALIGN_TOP     = 0,            /* vertical: against the top edge (default)      */
    IMGUI_ALIGN_VCENTER = 1 << 2,       /* vertical: centered                            */
    IMGUI_ALIGN_BOTTOM  = 1 << 3,       /* vertical: against the bottom edge             */

    IMGUI_ALIGN_CENTER  = IMGUI_ALIGN_HCENTER | IMGUI_ALIGN_VCENTER,   /* both axes centered */

} imgui_align_t;

typedef struct
{
    f32             cols[ IMGUI_LAYOUT_COLS ];  // column tracks, IMGUI_END-terminated (see unit rule)
    f32             rows[ IMGUI_LAYOUT_COLS ];  // row tracks; empty/NULL => flow mode, else grid mode
    f32             row_h;                      // flow only -- row height: 0 = auto, >0 = pixels
    f32             gap_x, gap_y;               // inter-cell spacing; 0 = theme default
    imgui_align_t   align;                      // content alignment within each cell (0 = LEFT | TOP)

} imgui_layout_t;

/*==============================================================================================
    Field label side -- where a labeled value widget (input_text / slider_float / checkbox) puts
    its label when a field split is active (imgui()->field_split / field_label_left).  The label and
    control are two tracks resolved across the widget's cell with the same overloaded unit as
    columns; `side` only decides which track sits on which edge.  NONE is the default: the label
    trails the control at its natural width on the right.
==============================================================================================*/

typedef enum
{
    IMGUI_LABEL_NONE  = 0,      /* off -- natural-width label trailing on the right (default) */
    IMGUI_LABEL_LEFT  = 1,      /* label track on the left, control fills the right */
    IMGUI_LABEL_RIGHT = 2,      /* label track on the right, control fills the left */

} imgui_label_side_t;

/*==============================================================================================
    Window drag mode -- how a window may be repositioned by the mouse.
    Selected globally via imgui()->set_window_drag(); default is TITLEBAR.
==============================================================================================*/

typedef enum
{
    IMGUI_WIN_DRAG_NONE     = 0,    /* windows are fixed in place                          */
    IMGUI_WIN_DRAG_TITLEBAR = 1,    /* drag only by the title bar (default)                */
    IMGUI_WIN_DRAG_BODY     = 2,    /* drag from anywhere in the window not over a widget  */

} imgui_win_drag_t;

/*==============================================================================================
    Window flags

    Passed as the final argument to begin_window to customize a single window's behavior.
    They mostly switch off default behavior; pass 0 (IMGUI_WIN_NONE) for the defaults.
==============================================================================================*/

typedef enum
{
    IMGUI_WIN_NONE              = 0,         /* default behavior */
    IMGUI_WIN_NOTITLEBAR        = 1 << 0,    /* no title bar: body fills the top; no collapse, no titlebar drag */
    IMGUI_WIN_NOCOLLAPSE        = 1 << 1,    /* no collapse arrow; the window stays expanded */
    IMGUI_WIN_NORESIZE          = 1 << 2,    /* disable user resizing from the border edges */
    IMGUI_WIN_NOMOVE            = 1 << 3,    /* disable user drag moving the window from anywhere */

    /* scrollbars -- dynamic: vertical bar and mouse wheel input are enabled by default */

    IMGUI_WIN_NOSCROLL          = 1 << 4,    /* disable all scroll bars (keep mouse input) */
    IMGUI_WIN_HSCROLL           = 1 << 5,    /* enable dynamic horizontal scroll bar (off by default) */
    IMGUI_WIN_NOMOUSESCROLL     = 1 << 6,    /* disable mouse wheel scolling */
    
    /* scrollbars - static: override dynamic bar flags */

    IMGUI_WIN_ALWAYS_VSCROLL    = 1 << 7,    /* always show vertical scroll bar -- override*/
    IMGUI_WIN_ALWAYS_HSCROLL    = 1 << 8,    /* always show horizontal scroll bar -- override */
    
    /* auto-resize -- size the window to its content instead of a fixed w/h */

    IMGUI_WIN_ALWAYS_AUTOSIZE   = 1 << 9,    /* hug content every frame: no user resize, no scrollbars */
    IMGUI_WIN_CAN_AUTOSIZE      = 1 << 10,   /* show a corner size-grip; double-click it to fit content */

 // IMGUI_WIN_MENUBAR      = 1 << 4,
 // IMGUI_WIN_NOINPUT      = 1 << 5,


} imgui_win_flags_t;

/*==============================================================================================
    Debug overlay layers

    Bitmask passed to imgui()->debug_set_layers().  Each bit enables one bolt-on debug
    visualization, emitted into a separate draw list and painted last, on top of the UI.
    The overlay is compiled in for Debug builds only (IMGUI_DEBUG_OVERLAY); in a Release
    build set_layers is a no-op and get_layers returns 0.  These constants stay defined in
    every build so call sites compile unchanged.
==============================================================================================*/

typedef enum
{
    IMGUI_DBG_NONE     = 0,         /* overlay off                                          */
    IMGUI_DBG_WINDOW   = 1 << 0,    /* window outer frames; the hover window stands out     */
    IMGUI_DBG_INTERACT = 1 << 1,    /* per-widget interaction rects (hover/active tinted)   */
    IMGUI_DBG_RESIZE   = 1 << 2,    /* window edge-resize grab bands; hot when armed        */
    IMGUI_DBG_CLIP     = 1 << 3,    /* clip (scissor) rectangle stack, colored by depth     */

    IMGUI_DBG_ALL      = IMGUI_DBG_WINDOW | IMGUI_DBG_INTERACT | IMGUI_DBG_RESIZE | IMGUI_DBG_CLIP,

} imgui_dbg_layer_t;

/*==============================================================================================
    Color packing

    IMGUI_COLOR(r,g,b,a) packs 0-255 byte values into a u32 such that memory byte order
    is [R, G, B, A], matching VK_FORMAT_R8G8B8A8_UNORM vertex attribute layout.
==============================================================================================*/

#define IMGUI_COLOR( r, g, b, a ) \
    ( ( ( u32 )( a ) << 24 ) | ( ( u32 )( b ) << 16 ) | ( ( u32 )( g ) << 8 ) | ( u32 )( r ) )

/*==============================================================================================
    Draw vertex  (20 bytes, single interleaved binding)

    Vertex attribute layout (matches the imgui pipeline):
        location 0 : float2  (x, y)       offset  0   -- pixel-space position
        location 1 : float2  (u, v)       offset  8   -- texture UV [0..1]
        location 2 : UNORM4  (abgr u32)   offset 16   -- packed color, R8G8B8A8_UNORM
==============================================================================================*/

typedef struct
{
    f32 x, y; /* pixel position */
    f32 u, v; /* texture UV     */
    u32 abgr; /* packed color   */

} imgui_draw_vert_t;

/*==============================================================================================
    Draw command  (one GPU draw call)

    A new command is opened whenever the bound texture index or clip rectangle changes.
    tex_idx == 0 means use the 1x1 opaque-white pixel (solid colour draws).
==============================================================================================*/

typedef struct
{
    u32          elem_count; /* number of indices to emit */
    u32          tex_idx;    /* bindless texture slot     */
    imgui_rect_t clip_rect;  /* scissor rect (pixels)     */

} imgui_draw_cmd_t;

/*==============================================================================================
    Limits
==============================================================================================*/

/* 16K verts is far above what a debug UI emits per frame, and keeps vertex indices
   well within u16 range (64K would sit right at the 65535 ceiling).  The per-frame
   region sizes that fall out of these (VB 320 KB, IB 96 KB) are both 256-byte
   aligned, so each frame-in-flight region stays independently addressable -- note
   that this only matters if the VB/IB are ever moved off HOST_COHERENT memory, in
   which case regions would need rounding up to nonCoherentAtomSize to flush apart. */

#define IMGUI_MAX_VERTS  ( 16 * 1024 )
#define IMGUI_MAX_IDX    ( IMGUI_MAX_VERTS * 3 )
#define IMGUI_MAX_CMDS   1024
#define IMGUI_CLIP_DEPTH 32

/*==============================================================================================
    GPU resource memory usage (bytes), reported by imgui()->mem_stats().
==============================================================================================*/

typedef struct
{
    u32 vertex_bytes;   /* vertex buffer -- all frames-in-flight regions */
    u32 index_bytes;    /* index buffer  -- all frames-in-flight regions */
    u32 texture_bytes;  /* font atlases + 1x1 white pixel                */
    u32 total_bytes;    /* sum of the above                              */

} imgui_mem_stats_t;

/*==============================================================================================
    Font selection

    imgui_font_t selects which built-in bitmap atlas to use.
    The TrueType path is activated separately via imgui()->load_font(path).

    The number of the pixel height (not font size in .ttf)
==============================================================================================*/

typedef enum
{
    IMGUI_FONT_BITMAP_8 = 0,        /* 8x8   pixel glyphs -- compact, pixel-perfect at native size */
    IMGUI_FONT_BITMAP_16,           /* 16x16 pixel glyphs -- 2x larger version of 8x8 */
    IMGUI_FONT_BITMAP_12,           /* 8x12  pixel glyphs -- default, pixel-perfect at native size */
    IMGUI_FONT_BITMAP_16_PROGGY,    /* 9x16  tiny font */  
    IMGUI_FONT_BITMAP_20_PROGGY,    /* 12x20 tiny font */
    IMGUI_FONT_BITMAP_16_JETBOLD,   /* 10x16 pixel glyphs */
    IMGUI_FONT_BITMAP_20_JETBOLD,   /* 12x20 pixel glyphs */
    IMGUI_FONT_BITMAP_24_JETBOLD,   /* 14x33 pixel glyphs */
    IMGUI_FONT_BITMAP_24_CONSOLA,   /* 13x25 pixel glyphs */    
    
    IMGUI_FONT_BITMAP_MAX,          /* number of built-in bitmap fonts; */

} imgui_font_t;

// clang-format on
/*============================================================================================*/
#endif    // IMGUI_H
