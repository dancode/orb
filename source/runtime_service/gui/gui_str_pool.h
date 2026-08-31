#ifndef GUI_STR_POOL_H
#define GUI_STR_POOL_H
/*==============================================================================================

    runtime_service/gui/gui_str_pool.h -- small growable name-string pool.

    A handful of GUI registries (icons, shapes, sprites, the font ship-scan, the font registry's
    display name) keep a short lookup-key string per entry.  Each used to reserve a fixed
    char[N] per slot sized for a worst case none of them approach, wasting most of the array.
    This is the shared replacement: names live in one bump-allocated buffer per registry, and an
    entry keeps a u32 BYTE OFFSET into it instead of a copy.

    Intern deduplicates by scanning the existing bytes for an identical string first (the pool
    stays small, so a linear scan is cheap) -- interning the same name twice returns the same
    offset rather than growing the pool.  That makes a registry whose entries only ever GROW
    (append, full reset on shutdown, never reassigned to different content one at a time) a safe
    fit: there is no way to reclaim a single entry's bytes out of the middle of the buffer, so a
    registry that overwrites an entry in place would slowly orphan the old string forever.

    Growth follows the same shape gui_res_atlas.c's res_resize already uses for the shared
    texture: allocate a bigger buffer, copy the old bytes forward, free the old one.  Existing
    offsets stay valid across a grow because they are byte offsets, not pointers.

    One instance per registry, not one pool shared across all of them -- so a full sprite set can
    never make an unrelated icon registration fail; each pool's capacity is a function of only
    its own registry's usage.

==============================================================================================*/

#include <stdlib.h>
#include <string.h>

// clang-format off

typedef struct
{
    char* buf;   // bump-allocated backing storage; NULL until the first intern
    u32   cap;   // bytes allocated at buf
    u32   top;   // bytes used; the offset the next new entry would land at

} gui_str_pool_t;

#define GUI_STR_POOL_NONE  0xffffffffu   // intern failure (allocator OOM) sentinel

/* Release the backing buffer and return the pool to its just-declared state.  Pair with the
   owning registry's own full reset (e.g. icon_atlas_shutdown) -- entries and their name offsets
   go invalid together. */
static inline void
gui_str_pool_reset( gui_str_pool_t* p )
{
    free( p->buf );
    p->buf = NULL;
    p->cap = 0;
    p->top = 0;
}

/* Find `s` if it is already interned, or bump-allocate a copy and return its offset.  Grows the
   backing buffer (alloc bigger, copy forward, free old) when the copy would not fit.  Returns
   GUI_STR_POOL_NONE only if the allocator itself fails. */
static inline u32
gui_str_pool_intern( gui_str_pool_t* p, const char* s )
{
    if ( !s ) s = "";
    u32 len = (u32)strlen( s );

    for ( u32 i = 0; i < p->top; )
    {
        u32 entry_len = (u32)strlen( p->buf + i );
        if ( entry_len == len && memcmp( p->buf + i, s, len ) == 0 )
            return i;
        i += entry_len + 1u;
    }

    u32 need = len + 1u;
    if ( p->top + need > p->cap )
    {
        u32 new_cap = p->cap ? p->cap * 2u : 256u;
        while ( new_cap < p->top + need )
            new_cap *= 2u;
        char* nb = (char*)malloc( new_cap );
        if ( !nb )
            return GUI_STR_POOL_NONE;
        if ( p->buf )
        {
            memcpy( nb, p->buf, p->top );
            free( p->buf );
        }
        p->buf = nb;
        p->cap = new_cap;
    }

    u32 off = p->top;
    memcpy( p->buf + off, s, need );
    p->top += need;
    return off;
}

/* The interned string at `off`, or "" for GUI_STR_POOL_NONE / an empty pool.  The returned
   pointer is only valid until the next gui_str_pool_intern on this pool (a grow can move buf) --
   callers use it immediately (a printf, a strcmp), never hold onto it. */
static inline const char*
gui_str_pool_cstr( const gui_str_pool_t* p, u32 off )
{
    return ( off == GUI_STR_POOL_NONE || !p->buf ) ? "" : p->buf + off;
}

// clang-format on
/*============================================================================================*/
#endif    // GUI_STR_POOL_H
