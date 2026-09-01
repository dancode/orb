/*==============================================================================================

    runtime_service/gui/gui_names.c -- GUI_NAMES translation unit: the shared name pool.

    Storage and implementation for the one string pool every GUI resource registry's lookup-key
    string interns into (gui_names.h).  Depends on nothing but the C runtime, so it sits at the
    bottom of the stack next to gui_rect.c / gui_log.c and is reached the same way -- through
    the public gui.h chain -- rather than by each consumer including it directly.

    Names live in one bump-allocated buffer and an entry keeps the u16 BYTE OFFSET returned by
    gui_names_intern instead of a char[N] copy.  Interning deduplicates by scanning the existing
    bytes for an identical string first; the pool stays small, so the linear scan is cheap.

    The u16 offset caps the pool at 64 KB: gui_names_intern refuses (returns GUI_NAMES_NONE) once
    the next entry would land at or past that ceiling, same as an allocator OOM.  s_names.top
    itself stays u32 so the ceiling check has room to compare past it without wrapping.

    There is no way to reclaim a single entry's bytes out of the middle of the buffer, so only a
    registry whose entries GROW -- append, full reset at shutdown, never reassigned to different
    content one at a time -- is a safe client.  A registry that overwrote an entry in place would
    slowly orphan the old string forever.

    A grow reallocates the buffer to the next multiple of GUI_NAMES_GROW that fits.  Capacity
    climbs in fixed 512 B steps rather than doubling: the whole pool tops out at 64 KB, so the
    handful of extra reallocs costs less than the overshoot a doubling curve would hold.
    Existing offsets stay valid across a grow because they are byte offsets, not pointers.

==============================================================================================*/

#include <stdlib.h>
#include <string.h>

#include "orb.h"

#include "runtime_service/gui/gui_names.h"

static struct
{
    char* buf;    // bump-allocated backing storage; NULL until the first intern
    u32   cap;    // bytes allocated at buf
    u32   top;    // bytes used; the offset the next new entry would land at

} s_names;

#define GUI_NAMES_GROW 512u    // capacity is always a multiple of this; grows in fixed steps

u16
gui_names_intern( const char* s )
{
    if ( !s ) s = "";
    u32 len = (u32)strlen( s );

    /* search for existing matching entry */
    for ( u32 i = 0; i < s_names.top; )
    {
        u32  entry_len = (u32)strlen( s_names.buf + i );
        if ( entry_len == len && memcmp( s_names.buf + i, s, len ) == 0 )
             return (u16)i;
        i += entry_len + 1u;
    }

    /* storage overflow -- 64 KB u16-offset ceiling reached */
    if ( s_names.top >= GUI_NAMES_NONE )
         return GUI_NAMES_NONE;

    u32 need = len + 1u;
    if ( s_names.top + need > s_names.cap )
    {
        u32 want    = s_names.top + need;
        u32 new_cap = (( want + ( GUI_NAMES_GROW - 1u )) / GUI_NAMES_GROW ) * GUI_NAMES_GROW;

        char* new_buf = (char*)realloc( s_names.buf, new_cap );
        if ( !new_buf )
            return GUI_NAMES_NONE;

        s_names.buf = new_buf;
        s_names.cap = new_cap;
    }

    u32 off = s_names.top;
    memcpy( s_names.buf + off, s, need );
    s_names.top += need;

    return ( u16 )off; /* string pool offset */
}

const char*
gui_names_cstr( u16 off )
{
    return ( off == GUI_NAMES_NONE || !s_names.buf ) ? "" : s_names.buf + off;
}

void
gui_names_reset( void )
{
    free( s_names.buf );
    s_names.buf = NULL;
    s_names.cap = 0;
    s_names.top = 0;
}

/*============================================================================================*/
