/*==============================================================================================

    sandbox/vulkan/sb_vulkan_gui.h -- gui feature demos.

    Each demo is a single self-contained function that opens its own window and exercises one
    feature group of the gui system (basic widgets, layout rows, field forms, grids, child
    regions, alignment, sub-layout, multiple windows).  The host (sb_vulkan.c) drives them: it
    owns the frame_begin/ctx_begin .. ctx_end/frame_end bracket and render(), and selects which
    demo to draw with a key switch -- number keys 1..9 jump to a demo, +/- step through the table.

    A demo function takes no arguments and assumes it runs inside an open context (between
    gui()->ctx_begin() and ctx_end()); it must balance its own window_begin/window_end (and
    child_begin, push_id, push_layout) calls.

    Usage (host):
        gui()->frame_begin( dt );
        gui()->ctx_begin( GUI_CTX_DEFAULT );
        sb_gui_demos[ active ].fn();      // draw the active demo
        sb_gui_demo_picker( active );     // draw the picker overlay
        gui()->ctx_end();
        gui()->frame_end();
        gui()->render( vp0, cmd );

==============================================================================================*/
#ifndef SB_VULKAN_GUI_H
#define SB_VULKAN_GUI_H

#include "orb.h"

/* A demo is just a name + a draw function; the table below is the menu the host steps through. */
typedef void ( *sb_gui_demo_fn )( void );

typedef struct
{
    const char*      name; /* short label shown in the picker            */
    const char*      desc; /* one-line description of the feature group  */
    sb_gui_demo_fn fn;   /* draws the demo window(s) for this feature  */

} sb_gui_demo_t;

/* Null-name-terminated table of demos, indexable 0..count-1. */
extern const sb_gui_demo_t sb_gui_demos[];

/* Number of entries in sb_gui_demos[]. */
int sb_gui_demo_count( void );

/* Draw the picker overlay: a small window listing every demo with the active one highlighted,
   plus the key hints.  Call once per frame after the active demo.  Returns the demo index to
   show next: `active` unchanged, or the index of a row the user clicked this frame. */
int sb_gui_demo_picker( int active );

#endif /* SB_VULKAN_GUI_H */
