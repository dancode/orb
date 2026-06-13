/*==============================================================================================

    sandbox/vulkan/sb_vulkan_imgui.h -- imgui feature demos.

    Each demo is a single self-contained function that opens its own window and exercises one
    feature group of the imgui system (basic widgets, layout rows, field forms, grids, child
    regions, alignment, sub-layout, multiple windows).  The host (sb_vulkan.c) drives them: it
    owns new_frame()/render() and selects which demo to draw with a key switch -- number keys
    1..9 jump to a demo, +/- step back and forth through the table.

    A demo function takes no arguments and assumes it runs between imgui()->new_frame() and
    imgui()->render(); it must balance its own begin_window/end_window (and begin_child, push_id,
    push_layout) calls.

    Usage (host):
        imgui()->new_frame( w, h, dt );
        sb_imgui_demos[ active ].fn();      // draw the active demo
        sb_imgui_demo_picker( active );     // draw the picker overlay
        imgui()->render( cmd, w, h );

==============================================================================================*/
#ifndef SB_VULKAN_IMGUI_H
#define SB_VULKAN_IMGUI_H

#include "orb.h"

/* A demo is just a name + a draw function; the table below is the menu the host steps through. */
typedef void ( *sb_imgui_demo_fn )( void );

typedef struct
{
    const char*      name; /* short label shown in the picker            */
    const char*      desc; /* one-line description of the feature group  */
    sb_imgui_demo_fn fn;   /* draws the demo window(s) for this feature  */

} sb_imgui_demo_t;

/* Null-name-terminated table of demos, indexable 0..count-1. */
extern const sb_imgui_demo_t sb_imgui_demos[];

/* Number of entries in sb_imgui_demos[]. */
int sb_imgui_demo_count( void );

/* Draw the picker overlay: a small window listing every demo with the active one highlighted,
   plus the key hints.  Call once per frame after the active demo.  Returns the demo index to
   show next: `active` unchanged, or the index of a row the user clicked this frame. */
int sb_imgui_demo_picker( int active );

#endif /* SB_VULKAN_IMGUI_H */
