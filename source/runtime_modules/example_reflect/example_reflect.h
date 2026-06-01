#ifndef EXAMPLE_REFLECT_H
#define EXAMPLE_REFLECT_H
/*==============================================================================================

    example_reflect.h - Public API and reflected types for the example_reflect module.

    Exercises the full feature set of the ref_ reflection system:
        - basic struct registration
        - nested struct fields
        - pointer / const / array / pointer-to-array fields
        - regular enum
        - bitset (flag-style enum)
        - type-level and field-level attributes (tag, int, float, string)
        - tagged union (discriminated variant: ex_event_t / ex_event_payload_t)

    Reflection data is exported from the generated TU as the well-known DLL symbol
    "ref_register". The ref_ system discovers and calls it automatically via the
    module system's DLL load callback. The module itself is blind to the registry.

==============================================================================================*/

#include "orb.h"
#include "engine/ref/ref_api.h"

/*==============================================================================================
    Reflected enums
==============================================================================================*/

REF_ENUM( tooltip = "Cardinal facings used for spawn placement." )
typedef enum ex_facing_e
{
    EX_FACING_NORTH = 0,
    EX_FACING_EAST  = 1,
    EX_FACING_SOUTH = 2,
    EX_FACING_WEST  = 3,

} ex_facing_t;

REF_BITSET( tooltip = "Per-entity capability bits." )
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

REF_STRUCT()
typedef struct ex_vec3_s
{
    REF_PROP() float x;
    REF_PROP() float y;
    REF_PROP() float z;

} ex_vec3_t;

REF_STRUCT( tooltip = "Position, rotation, and scale in world space." )
typedef struct ex_transform_s
{
    REF_PROP() ex_vec3_t position;
    REF_PROP() ex_vec3_t rotation;
    REF_PROP() ex_vec3_t scale;

} ex_transform_t;

REF_STRUCT( tooltip = "Single demo entity exercising the reflection feature surface." )
typedef struct ex_entity_s
{
    REF_PROP()                                int32_t        id;
    REF_PROP()                                ex_facing_t    facing;
    REF_PROP()                                ex_caps_t      caps;
    REF_PROP()                                char           name[ 32 ];
    REF_PROP()                                ex_transform_t transform;
    REF_PROP( range = 0.0, 100.0, step = 0.5, category = "Stats" ) float health;
    REF_PROP( transient )                     void*          scratch;
    REF_PROP()                                ex_vec3_t*     velocity;
    REF_PROP()                                const char*    label;
    REF_PROP()                                ex_vec3_t*     slots[ 4 ];

} ex_entity_t;

/*==============================================================================================
    Reflected tagged union  (game-engine variant pattern)

    ex_event_t is the canonical example: a discriminant field (kind) paired with a payload
    union whose active member is determined by that discriminant.  The reflection system
    records every variant's fields and offsets so tools (inspectors, serializers, diffing)
    can display the correct member without bespoke switch-case code.
==============================================================================================*/

REF_ENUM( tooltip = "Selects the active member of ex_event_payload_t." )
typedef enum ex_event_kind_e
{
    EX_EVENT_SPAWN  = 0,
    EX_EVENT_DAMAGE = 1,
    EX_EVENT_MOVE   = 2,

} ex_event_kind_t;

REF_STRUCT()
typedef struct ex_spawn_payload_s
{
    REF_PROP() ex_vec3_t   origin;
    REF_PROP() ex_facing_t facing;

} ex_spawn_payload_t;

REF_STRUCT()
typedef struct ex_damage_payload_s
{
    REF_PROP( range = 0, 1000, tooltip = "Raw hit-point loss before mitigation." ) int32_t  amount;
    REF_PROP()                                                                      uint32_t source_id;

} ex_damage_payload_t;

REF_STRUCT()
typedef struct ex_move_payload_s
{
    REF_PROP() ex_vec3_t from;
    REF_PROP() ex_vec3_t to;

} ex_move_payload_t;

REF_UNION( tooltip = "Per-event data; active member is selected by ex_event_t.kind." )
typedef union ex_event_payload_u
{
    /* @case values mirror ex_event_kind_t; numeric literals because the attr parser treats a
       bare identifier as a tag, not an integer. spawn=0, damage=1, move=2. */
    REF_PROP( case = 0 ) ex_spawn_payload_t  spawn;
    REF_PROP( case = 1 ) ex_damage_payload_t damage;
    REF_PROP( case = 2 ) ex_move_payload_t   move;

} ex_event_payload_t;

REF_STRUCT( tooltip = "An engine event carrying a discriminated payload union." )
typedef struct ex_event_s
{
    REF_PROP()                                uint32_t           id;
    REF_PROP()                                ex_event_kind_t    kind;
    REF_PROP( union_tag = "kind" )            ex_event_payload_t payload;

} ex_event_t;

/*==============================================================================================
    Reflected function signature and NPC type

    ex_on_damage_fn demonstrates REF_FUNC: the signature is registered as a
    REF_KIND_FUNCTION type whose fields are [return, param0, param1, ...].
    ex_npc_t.on_damage is a REF_MODS_FUNCTION field; its aux stores the sig type_id.
==============================================================================================*/

REF_FUNC()
typedef void ( *ex_on_damage_fn )( int32_t amount, const ex_vec3_t* hit_pos );

REF_STRUCT( tooltip = "NPC with a reflected damage callback field." )
typedef struct ex_npc_s
{
    REF_PROP()                     int32_t         id;
    REF_PROP()                     char            name[ 32 ];
    REF_PROP( range = 0.0, 100.0 ) float           health;
    REF_PROP()                     ex_on_damage_fn on_damage;

} ex_npc_t;

/*============================================================================================*/
#endif    // EXAMPLE_REFLECT_H
