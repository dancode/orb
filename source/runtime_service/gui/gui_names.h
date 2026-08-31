#ifndef GUI_NAMES_H
#define GUI_NAMES_H
/*==============================================================================================

    runtime_service/gui/gui_names.h -- the GUI's one shared resource-name pool.

    Every registry that keeps a short lookup-key string per entry -- icons, shapes, sprites, the
    font resolver's shipped-file scan, a loaded font's family-root display name -- interns into
    this single pool instead of each reserving its own fixed char[N] per slot.  An entry keeps
    the u32 byte offset gui_names_intern returns.

    Built on the generic bump-allocator in gui_str_pool.h (see that header for how interning,
    dedup and growth work).  One pool rather than one per registry because dedup is shared too:
    the same name registered under two different registries -- or the same font family root
    re-landing after an eviction cycle -- collapses to one copy either way, and there is no
    per-registry capacity to run out of first (gui_str_pool grows on demand).

    Reset exactly once, at true gui_shutdown -- never from an individual registry's own
    shutdown, since the others may still be mid-teardown and reading their own (still valid)
    entries a moment longer costs nothing.

==============================================================================================*/

u32         gui_names_intern( const char* s );   // dedup-intern; returns a stable byte offset
const char* gui_names_cstr  ( u32 off );          // "" for GUI_STR_POOL_NONE / an unset offset
void        gui_names_reset ( void );             // free the pool -- gui_shutdown only, once

/*============================================================================================*/
#endif    // GUI_NAMES_H
