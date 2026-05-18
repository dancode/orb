#ifndef EXAMPLE_REFLECT_H
#define EXAMPLE_REFLECT_H
/*==============================================================================================

    example_reflect.h - Public API and reflected types for the example_reflect module.

    Exercises the full feature set of the rs_ reflection system:
        - basic struct registration
        - nested struct fields
        - pointer / const / array / pointer-to-array fields
        - regular enum
        - bitset (flag-style enum)
        - type-level and field-level attributes (tag, int, float, string)

    Reflection data is exported from the generated TU as the well-known DLL symbol
    "rs_register". The rs_ system discovers and calls it automatically via the
    module system's DLL load callback. The module itself is blind to the registry.

==============================================================================================*/

#include "orb.h"
#include "engine/mod/mod.h"
#include "engine/rs/rs.h"

/*==============================================================================================
    Reflected enums
==============================================================================================*/

RS_ENUM( tooltip = "Cardinal facings used for spawn placement." )
typedef enum ex_facing_e
{
    EX_FACING_NORTH = 0,
    EX_FACING_EAST  = 1,
    EX_FACING_SOUTH = 2,
    EX_FACING_WEST  = 3,

} ex_facing_t;

RS_BITSET( tooltip = "Per-entity capability bits." )
typedef enum ex_caps_e
{
    EX_CAPS_NONE   = 0,
    EX_CAPS_MOVE   = 1 << 0,
    EX_CAPS_RENDER = 1 << 1,
    EX_CAPS_SOLID  = 1 << 2,
    EX_CAPS_SCRIPT = 1 << 3,

} ex_caps_t;

/*==============================================================================================
    Reflected structs
==============================================================================================*/

RS_STRUCT()
typedef struct ex_vec3_s
{
    RS_PROP() float x;
    RS_PROP() float y;
    RS_PROP() float z;

} ex_vec3_t;

RS_STRUCT( tooltip = "Position, rotation, and scale in world space." )
typedef struct ex_transform_s
{
    RS_PROP() ex_vec3_t position;
    RS_PROP() ex_vec3_t rotation;
    RS_PROP() ex_vec3_t scale;

} ex_transform_t;

RS_STRUCT( tooltip = "Single demo entity exercising the reflection feature surface." )
typedef struct ex_entity_s
{
    RS_PROP()                                int32_t        id;
    RS_PROP()                                ex_facing_t    facing;
    RS_PROP()                                ex_caps_t      caps;
    RS_PROP()                                char           name[ 32 ];
    RS_PROP()                                ex_transform_t transform;
    RS_PROP( range = 0.0, 100.0 )            float          health;
    RS_PROP( transient )                     void*          scratch;
    RS_PROP()                                ex_vec3_t*     velocity;
    RS_PROP()                                const char*    label;
    RS_PROP()                                ex_vec3_t*     slots[ 4 ];

} ex_entity_t;

/*==============================================================================================
    Public module API
==============================================================================================*/

typedef struct example_reflect_api_s
{
    /* Returns a populated demo entity (pointer is stable for the module's lifetime). */
    const ex_entity_t* ( *demo_entity )( void );

} example_reflect_api_t;

/*============================================================================================*/

#if defined( BUILD_STATIC ) || defined( EXAMPLE_REFLECT_STATIC )
MOD_GATEWAY_STATIC( example_reflect_api_t, example_reflect )
mod_desc_t* example_reflect_get_mod_desc( void );
#else
MOD_GATEWAY_DYNAMIC( example_reflect_api_t, example_reflect )
#endif

#if defined( BUILD_STATIC ) || defined( EXAMPLE_REFLECT_STATIC )
    #define MOD_USE_EXAMPLE_REFLECT    /* static build */
    #define MOD_FETCH_EXAMPLE_REFLECT  true
#else
    #define MOD_USE_EXAMPLE_REFLECT    MOD_DEFINE_API_PTR( example_reflect_api_t, example_reflect )
    #define MOD_FETCH_EXAMPLE_REFLECT  MOD_FETCH_API( example_reflect_api_t, example_reflect )
#endif

/*============================================================================================*/
#endif    // EXAMPLE_REFLECT_H
