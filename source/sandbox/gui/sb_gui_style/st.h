/*==============================================================================================

    sandbox/gui/sb_gui_style/st.h -- Shared contract for the style + look customization bench.

    sb_gui_style is the "make the engine look like your game" bench.  Four windows, one pipeline:

        Style Editor  -- tune the live style: seeds + ramp, the derived colour grid, every
                         metric / skin / shape var.  This is where a look is FOUND.
        Look Gallery  -- a wide sweep of stock widgets under whatever the editor just did, so a
                         knob's effect is visible on something other than the editor itself.
        Style Export  -- emit the live style as C source: a gui_theme_t table entry, or a runtime
                         setup function.  This is where a look is KEPT.
        Font Tool     -- find / bake / preview / export the FACE, the other half of a look
                         (stb quick bake for iteration, font_tool.exe for the shippable asset).

    Each window function takes no arguments, assumes an open context (between ctx_begin and
    ctx_end), and balances its own window_begin/window_end.  The registry in sb_gui_style.c
    owns the menu bar and the open flags.

==============================================================================================*/
#ifndef ST_H
#define ST_H

#include "orb.h"

/* A bench window draws itself; the registry in sb_gui_style.c is the menu the host shows. */
typedef void ( *st_window_fn )( void );

typedef struct
{
    const char*  name;    /* menu item label                                          */
    const char*  title;   /* window title -- the open-state sync key                  */
    const char*  desc;    /* one-line description, shown as the menu item's tooltip   */
    st_window_fn fn;      /* draws the window                                         */
    bool         open;    /* live visibility, driven by the menu + the titlebar X      */

} st_window_t;

#endif /* ST_H */
