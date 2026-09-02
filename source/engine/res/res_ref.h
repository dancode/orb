/*==============================================================================================

    engine/res/res_ref.h - The reference section of a cooked file.

    Every cooked format (.orb_font, .tex, .oshd) carries the ids of the resources its content
    refers to -- the textures a material samples, the meshes a level places -- as an array of
    rid_t placed immediately after the format's fixed header:

        [ fixed header, with a ref_count field ][ rid_t refs[ ref_count ] ][ payload ... ]

    Each id is stored as a little-endian u32.  The cooker writes the array, since only it
    understands the content; the loader hands the ids to res when the file is read, so a name
    referenced by content alone still resolves at runtime, and the packager can walk from a
    target's source references through its content to everything it drags in without parsing
    a single format.  No cooker emits a reference yet: every file written today has ref_count
    0, and every loader steps over the (empty) section.

    A format may pad the section for the alignment of what follows it; the format header says
    so where it does (oshd_ref_bytes).  Header-only, so the format headers -- shared with
    tools that link base + sys only -- can include it.

==============================================================================================*/
#ifndef RES_REF_H
#define RES_REF_H

#include "engine/res/res.h"

// clang-format off

/* Most references one cooked file may carry.  A count past this is corrupt; readers reject the
   file on the count alone, before it feeds any size arithmetic. */
#define RES_REF_MAX           4096u

/* On-disk size of one reference: rid_t as a little-endian u32. */
#define RES_REF_SIZE          ( ( u32 )sizeof( rid_t ) )

static inline bool
res_ref_count_ok( u32 count )
{
    return count <= RES_REF_MAX;
}

/* Byte length of an unpadded section holding `count` ids.  u64 so the caller's size check
   cannot wrap even on a count it has not bounded yet. */
static inline u64
res_ref_bytes( u32 count )
{
    return ( u64 )count * RES_REF_SIZE;
}

// clang-format on
/*============================================================================================*/
#endif    // RES_REF_H
