#ifndef IMGUI_H
#define IMGUI_H
/*==============================================================================================

    runtime_service/imgui/imgui.h -- imgui module types.
    Include in DLL modules that use imgui through the vtable (imgui()->...).
    Include imgui_host.h instead for direct-call access (host, sandbox).

==============================================================================================*/

#include "orb.h"

/*==============================================================================================
    ID / geometry
==============================================================================================*/

typedef u32 imgui_id_t;
#define IMGUI_ID_NONE 0u

typedef struct
{
    f32 x, y;
} imgui_vec2_t;

typedef struct
{
    f32 x, y, w, h;
} imgui_rect_t;

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
    f32  mouse_x, mouse_y;    /* client-area cursor position        */
    f32  mouse_wheel;         /* signed scroll delta this frame     */
    bool mouse_down[ 3 ];     /* left=0 right=1 middle=2, held      */
    bool mouse_pressed[ 3 ];  /* true only on the first down frame  */
    bool mouse_released[ 3 ]; /* true only on the first up frame    */
    bool keys_down[ IMGUI_KEY_COUNT ];
    bool keys_pressed[ IMGUI_KEY_COUNT ];
    char text[ 32 ]; /* UTF-8 printable chars (NUL-term'd) */
    f32  dt;
    i32  display_w, display_h;

} imgui_io_t;

/*==============================================================================================
    Limits
==============================================================================================*/

#define IMGUI_MAX_VERTS  ( 64 * 1024 )
#define IMGUI_MAX_IDX    ( IMGUI_MAX_VERTS * 3 )
#define IMGUI_MAX_CMDS   1024
#define IMGUI_CLIP_DEPTH 32

/*==============================================================================================
    Font selection

    imgui_font_t selects which built-in bitmap atlas to use.
    The TrueType path is activated separately via imgui()->load_font(path).
==============================================================================================*/

typedef enum
{
    IMGUI_FONT_BITMAP_8 = 0,        /* 8x8   pixel glyphs -- compact, pixel-perfect at native size */
    IMGUI_FONT_BITMAP_16,           /* 16x16 pixel glyphs -- 2x larger version of 8x8 */
    IMGUI_FONT_BITMAP_12,           /* 8x12  pixel glyphs -- default, pixel-perfect at native size */    
    IMGUI_FONT_BITMAP_16_CLEAN,     /* 10x16 pixel glyphs -- blocky face, highly legible */

    IMGUI_FONT_BITMAP_12_CONSOLA,   /* 8x12 pixel glyphs -- Consolas, pixel-perfect at native size */    
    IMGUI_FONT_BITMAP_MAX,          /* number of built-in bitmap fonts; */

} imgui_font_t;

/*============================================================================================*/
#endif    // IMGUI_H
