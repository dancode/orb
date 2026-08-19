/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_demos.h -- gui feature demos.

    The exhaustive gui test suite: every feature group of the gui system has one demo window,
    grouped into categories, and a main menu bar (one menu per category) toggles each window on
    and off -- the Dear ImGui demo idea, split into mini-clusters instead of one mega window.
    Docking is deliberately absent: it has its own dedicated test bed (sb_gui_dock).

    Each demo is a single self-contained function that owns one primary window (plus any helper
    windows it needs).  Demo windows are CLOSEABLE (the titlebar X works) and default-resizable
    on both axes; the registry keeps the open flag in sync with the X through window_is_open.
    Where a widget has parametric capabilities, the demo wraps it in an interactive test suite:
    the widget's own parameters are driven live by sibling widgets.

    A demo function takes no arguments and assumes it runs inside an open context (between
    gui()->ctx_begin() and ctx_end()); it must balance its own window_begin/window_end (and
    child_begin, push_id, push_layout) calls.

    Usage (host):
        if ( gui()->frame_begin( dt ) )   // returns frame_dirty; emit only when true
        {
            gui()->ctx_begin( GUI_CTX_DEFAULT );
            ex_frame();                       // menu bar + every open demo window
            gui()->ctx_end();
        }
        gui()->frame_end();

==============================================================================================*/
#ifndef EX_DEMOS_H
#define EX_DEMOS_H

#include "orb.h"

/* A demo draws its window(s); the registry in ex_demos.c is the menu the host shows. */
typedef void ( *ex_demo_fn )( void );

typedef struct
{
    const char* category; /* menu the demo lives under ("Widgets", "Layout", ...)      */
    const char* name;     /* menu item label                                           */
    const char* title;    /* primary window title -- the open-state sync key           */
    const char* desc;     /* one-line description of the feature group                 */
    ex_demo_fn  fn;       /* draws the demo window(s) for this feature                 */
    bool        open;     /* live visibility flag, driven by the menu + the X button   */

} ex_demo_t;

/* Draw one frame of the explorer: the main menu bar plus every open demo window.  Call once
   per frame inside an open context. */
void ex_frame( void );

/* Scripted STYLE-RECORD CENSUS sweep (-census): walk every demo under every built-in theme, one
   demo at a time on a fixed frame budget, and dump the census once per theme.  The palette bake
   table is written from those dumps, and comparing them by the per-record hash is what separates
   a record a style var reaches from one built out of literals.  See ex_census.c.

       if ( census && !ex_census_start() ) return 1;    // before the loop
       ...
       if ( census && !ex_census_frame() ) break;       // once per frame, after ex_frame()

   ex_census_start returns false if the sweep cannot run; ex_census_frame returns false when the
   last theme's dump is out, which is the host's cue to quit. */
bool ex_census_start( void );
bool ex_census_frame( void );

#endif /* EX_DEMOS_H */
