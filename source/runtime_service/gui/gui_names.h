#ifndef GUI_NAMES_H
#define GUI_NAMES_H
/*==============================================================================================

    runtime_service/gui/gui_names.h -- the GUI's one shared resource-name pool.

    Every registry that keeps a short lookup-key string per entry -- icons, shapes, sprites, the
    font resolver's shipped-file scan, a loaded font's family-root display name -- interns into
    this single pool instead of each reserving its own fixed char[N] per slot.  An entry keeps
    the u16 byte offset gui_names_intern returns, capping the pool at 64 KB of interned bytes --
    entry counts across every registry (icons, shapes, sprites, fonts) leave that ceiling far
    out of reach, so the cap costs nothing today and buys every entry a 2-byte lookup key instead
    of 4.

    One pool rather than one per registry because dedup is shared too: the same name registered
    under two different registries -- or the same font family root re-landing after an eviction
    cycle -- collapses to one copy either way, and there is no per-registry capacity to run out
    of first (the pool grows on demand, up to the 64 KB ceiling).  gui_names.c holds the storage
    and documents how interning, dedup and growth work.

    A returned pointer is only valid until the next gui_names_intern -- a grow moves the backing
    buffer -- so callers use it immediately (a printf, a strcmp) and never hold onto it.

    Reset exactly once, at true gui_shutdown -- never from an individual registry's own
    shutdown, since the others may still be mid-teardown and reading their own (still valid)
    entries a moment longer costs nothing.

==============================================================================================*/

#define GUI_NAMES_NONE 0xffffu    // intern failure (allocator OOM or 64 KB pool ceiling) sentinel

u16         gui_names_intern( const char* s );    // dedup-intern; returns a stable byte offset
const char* gui_names_cstr  ( u16 off );          // "" for GUI_NAMES_NONE / an unset offset
void        gui_names_reset ( void );             // free the pool -- gui_shutdown only, once

/*============================================================================================*/
#endif    // GUI_NAMES_H
