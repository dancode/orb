/*==============================================================================================

    reflect.c

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "orb.h"

/*============================================================================================*/

#include "sid/sid.h"
#include "reflect/reflect.h"

/*============================================================================================*/

typedef struct rf_module_s
{
    sid_t    name_sid;          // Module name
    uint32_t version;           // Module version
    uint32_t load_count;        // Number of times loaded

    uint16_t first_type_id;     // First type index (for iteration) -- rename type_index (like other code)
    uint16_t type_count;        // Types defined by this module

    uint8_t  state;             // rf_module_state_t
    uint8_t  reserved[ 3 ];     // Temp padding

    void*    dll_handle;        // Platform-specific handle

} rf_module_t;

/*============================================================================================*/

typedef struct registry_s
{
    uint16_t    type_count;                             // How many registered
    uint16_t    field_count;                            // How many registered fields
    uint16_t    attr_count;                             // How many registered attributes
    uint16_t    reserved;                               // Padding

    rf_type_t   type_array      [ MAX_TYPES ];          // All registered types
    rf_field_t  field_array     [ MAX_FIELDS ];         // All registered fields
    rf_attrib_t attrib_array    [ MAX_ATTRIBUTES ];     // All registered attributes
    uint16_t    type_hash       [ TYPE_HASH_SIZE ];     // Index hash into type array (next chained)

    rf_module_t module_array    [ MAX_MODULES ];        // module info
    uint8_t     module_count;                           // number of registered modules
    uint8_t     transaction_active;                     // 1 = in transaction
    uint16_t    transaction_checkpoint;                 // Rollback point

} registry_t;

/*============================================================================================*/

#include "reflect/reflect_registry.c"
#include "reflect/reflect_module.c"
#include "reflect/reflect_access.c"
#include "reflect/reflect_print.c"
#include "reflect/reflect_test.c"

/*============================================================================================*/
