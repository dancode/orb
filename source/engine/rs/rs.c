/*==============================================================================================

    engine/rs/rs.c - Unity build entry point for the rs_ reflection library.

    engine_rs is a standalone static library with zero link-time dependency on engine_core.
    The host supplies two callbacks at rs_init(): an intern function (string -> rs_name_t)
    and a cstr function (rs_name_t -> string).  rs_hash_str is a local static inline so
    hash computation needs no pointer indirection.

==============================================================================================*/

#include "orb.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "engine/sys/sys.h"          /* lib_handle_t, sys_library_get_symbol */
#include "engine/rs/rs.h"

/*==============================================================================================
    Registry storage  (shared across all TUs in this unity build)
==============================================================================================*/

typedef struct rs_registry_s
{
    uint16_t    type_count;
    uint16_t    field_count;
    uint16_t    attr_count;
    uint16_t    enum_count;
    uint16_t    frame_count;
    uint8_t     _pad[ 2 ];

    rs_intern_fn intern;             /* string -> rs_name_t, set by rs_init() */
    rs_cstr_fn   cstr;               /* rs_name_t -> string, set by rs_init() */

    rs_type_t   types[ RS_MAX_TYPES ];
    rs_field_t  fields[ RS_MAX_FIELDS ];
    rs_attrib_t attrs[ RS_MAX_ATTRS ];
    rs_enum_t   enums[ RS_MAX_ENUMS ];
    rs_frame_t  frames[ RS_MAX_FRAMES ];

    uint16_t    type_hash[ RS_TYPE_HASH_SIZE ];

} rs_registry_t;

/*============================================================================================*/

#include "engine/rs/rs_registry.c"
#include "engine/rs/rs_access.c"
#include "engine/rs/rs_walk.c"
#include "engine/rs/rs_serialize.c"
#include "engine/rs/rs_print.c"

/* rs_test.c is intentionally NOT part of the library unity build.  It uses
   sid_intern_cstr directly and is compiled as a separate TU in the test
   sandbox target (sb_engine_core_reflect). */

/*============================================================================================*/
