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

    Window flags are set per-window via the title string in begin_window.
    Can be set globally to set the starting default for all windows.
==============================================================================================*/

// typedef union window_flags_s            /* internal representation of window flags */
// {
//     u32 bits;
//     struct
//     {
//         u32 no_titlebar         : 1; /* disable title bar (and thus window dragging) */
//         // u32 no_resize        : 1; /* disable user resizing with the mouse */
//         // u32 no_close         : 1; /* disable user closing with the mouse (no effect on app-driven close) */
//         // u32 no_scrollbar     : 1; /* disable automatic vertical scrollbar when content exceeds window height */
//         // u32 no_scroll        : 1; /* disable all scrolling (content that exceeds window height is inaccessible) */
//     };
// 
// } window_flags_t;

typedef enum imgui_win_flags_e
{
    IMGUI_WIN_NOTITLEBAR   = 1 << 0,    /* disable title bar (and thus window dragging) */
 // IMGUI_WIN_NOCOLLAPSE   = 1 << 1,    /* ... */
 // IMGUI_WIN_NOMOVE       = 1 << 2,
 // IMGUI_WIN_AUTORESIZE   = 1 << 3,
 // IMGUI_WIN_MENUBAR      = 1 << 4,
 // IMGUI_WIN_NOINPUT      = 1 << 5,
 // IMGUI_WIN_HSCROLL      = 1 << 6,
 // IMGUI_WIN_NOSCROLL     = 1 << 7,

} imgui_win_flags_t;

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
    Per-frame input snapshot

    IMGUI_KEY_COUNT must cover the full app_key_t range.  A static assert in imgui_input.c
    verifies this at compile time.
==============================================================================================*/

#define IMGUI_KEY_COUNT 128

typedef struct
{
    f32     mouse_x, mouse_y;                       // client-area cursor position        
    f32     mouse_wheel;                            // signed scroll delta this frame     
    bool    mouse_down      [ 3 ];                  // left=0 right=1 middle=2, held      
    bool    mouse_pressed   [ 3 ];                  // true only on the first down frame  
    bool    mouse_released  [ 3 ];                  // true only on the first up frame    
    bool    keys_down       [ IMGUI_KEY_COUNT ];    // held keys, indexed by app_key_t
    bool    keys_pressed    [ IMGUI_KEY_COUNT ];    // true only on the first down frame
    char    text[ 32 ];                             // UTF-8 printable chars (NUL-term'd)
    f32     dt;                                     // delta time in seconds (for animations, etc.)
    i32     display_w, display_h;                   // current display size in pixels (for window-relative coords, etc.)

} imgui_io_t;

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
    IMGUI_FONT_BITMAP_20_JETBOLD,   /* 12x12 pixel glyphs */
    IMGUI_FONT_BITMAP_24_JETBOLD,   /* 14x33 pixel glyphs */
    IMGUI_FONT_BITMAP_24_CONSOLA,   /* 13x25 pixel glyphs */    
    
    IMGUI_FONT_BITMAP_MAX,          /* number of built-in bitmap fonts; */

} imgui_font_t;

// clang-format on
/*============================================================================================*/
#endif    // IMGUI_H
