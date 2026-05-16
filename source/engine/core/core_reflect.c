/*==============================================================================================

    core/core_rs.c : Unity build entry point for the rs_ reflection system.

==============================================================================================*/

#include "orb.h"

/*============================================================================================*/

#include "sid/sid.h"
#include "reflect/rs.h"

/*==============================================================================================
    The registry is defined here so all rs_*.c translation units share it through the
    unity build. Tables are flat and append-only within a frame; rs_pop_frame() truncates
    them back to that frame's starting marks.
==============================================================================================*/


typedef struct rs_registry_s
{
    uint16_t    type_count;
    uint16_t    field_count;
    uint16_t    attr_count;
    uint16_t    enum_count;
    uint16_t    frame_count;
    uint8_t     _pad[ 2 ];

    rs_type_t   types[ RS_MAX_TYPES ];
    rs_field_t  fields[ RS_MAX_FIELDS ];
    rs_attrib_t attrs[ RS_MAX_ATTRS ];
    rs_enum_t   enums[ RS_MAX_ENUMS ];
    rs_frame_t  frames[ RS_MAX_FRAMES ];

    uint16_t    type_hash[ RS_TYPE_HASH_SIZE ];

    /* Parallel to fields[]: pending base-type hash for forward-ref resolve.
       Populated by rs_register_type, consumed by rs_finalize_frame. Kept around
       afterward only because truncation on pop is automatic via field_count. */
    uint32_t pending_type_hash[ RS_MAX_FIELDS ];

} rs_registry_t;

/*============================================================================================*/

#include "reflect/rs_registry.c"
#include "reflect/rs_access.c"
#include "reflect/rs_walk.c"
#include "reflect/rs_serialize.c"
#include "reflect/rs_print.c"
#include "reflect/rs_test.c"

/*============================================================================================*/
