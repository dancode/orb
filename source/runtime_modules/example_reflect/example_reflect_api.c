/*==============================================================================================

    example_reflect_api.c -- example_reflect module wiring.
    Implements the example_reflect_api_t vtable struct and the mod_desc_t lifecycle descriptor.

==============================================================================================*/

/*==============================================================================================
    Cached API pointers

    Declare one per consumed module using its MOD_USE_<NAME> macro (defined in its _api.h),
    then fetch in init() and reload() with MOD_FETCH_<NAME>:

        MOD_USE_CORE;                                    // file scope
        if ( !MOD_FETCH_CORE ) return false;             // in init() and reload()
==============================================================================================*/

/* none */

/*==============================================================================================
    Persistent state (allocated by the module system; preserved across hot-reloads)
==============================================================================================*/

typedef struct example_reflect_state_s
{
    ex_entity_t demo;
    ex_vec3_t   demo_velocity;
    ex_vec3_t   demo_slot[ 2 ];
    ex_npc_t    demo_npc;

} example_reflect_state_t;

static example_reflect_state_t* s = NULL;

/*==============================================================================================
    Implementation
==============================================================================================*/

/* Concrete callback wired into the demo NPC so on_damage is a non-null pointer. */
static void
ex_on_damage_impl( int32_t amount, const ex_vec3_t* hit_pos )
{
    UNUSED( amount );
    UNUSED( hit_pos );
}

static void
populate_demo( example_reflect_state_t* st )
{
    memset( &st->demo, 0, sizeof st->demo );

    st->demo.id     = 7;
    st->demo.facing = EX_FACING_EAST;
    st->demo.caps   = EX_CAPS_MOVE | EX_CAPS_RENDER | EX_CAPS_SCRIPT;

    strncpy( st->demo.name, "demo_entity", sizeof st->demo.name - 1 );

    st->demo.transform.position = ( ex_vec3_t ){ 1.0f, 2.0f, 3.0f };
    st->demo.transform.rotation = ( ex_vec3_t ){ 0.0f, 0.0f, 0.0f };
    st->demo.transform.scale    = ( ex_vec3_t ){ 1.0f, 1.0f, 1.0f };
    st->demo.health             = 42.5f;

    st->demo_velocity           = ( ex_vec3_t ){ 10.0f, 0.0f, -5.0f };
    st->demo.velocity           = &st->demo_velocity;
    st->demo.label              = "demo-label";

    st->demo_slot[ 0 ]          = ( ex_vec3_t ){ 0.5f, 0.5f, 0.5f };
    st->demo_slot[ 1 ]          = ( ex_vec3_t ){ 0.0f, 1.0f, 0.0f };
    st->demo.slots[ 0 ]         = &st->demo_slot[ 0 ];
    st->demo.slots[ 1 ]         = &st->demo_slot[ 1 ];
    st->demo.slots[ 2 ]         = NULL;
    st->demo.slots[ 3 ]         = NULL;

    /* Populate demo NPC with a live callback so the function pointer field is non-null. */
    memset( &st->demo_npc, 0, sizeof st->demo_npc );
    st->demo_npc.id        = 1;
    strncpy( st->demo_npc.name, "demo_npc", sizeof st->demo_npc.name - 1 );
    st->demo_npc.health    = 80.0f;
    st->demo_npc.on_damage = ex_on_damage_impl;
}

REF_API()
const ex_entity_t*
api_demo_entity( void )
{
    return s ? &s->demo : NULL;
}

REF_API()
const ex_npc_t*
api_demo_npc( void )
{
    return s ? &s->demo_npc : NULL;
}

REF_API()
int
this_is_a_function( void )
{
    return 123;
}

/*==============================================================================================
    API Struct
==============================================================================================*/

const example_reflect_api_t g_example_reflect_api_struct = {
    .demo_entity = api_demo_entity,
    .demo_npc    = api_demo_npc,
};

/*==============================================================================================
    Lifecycle
==============================================================================================*/

static bool
example_reflect_init( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s = ( example_reflect_state_t* )raw_state;
    populate_demo( s );
    return true;
}

static bool
example_reflect_reload( void* raw_state, get_api_fn get_api )
{
    UNUSED( get_api );
    s = ( example_reflect_state_t* )raw_state;
    return true;
}

static void
example_reflect_exit( void* raw_state )
{
    UNUSED( raw_state );
}

/*==============================================================================================
    Module descriptor
==============================================================================================*/

mod_desc_t*
example_reflect_get_mod_desc( void )
{
    static mod_desc_t desc = {
        .version       = 1,
        .state_size    = sizeof( example_reflect_state_t ),
        .func_api_size = sizeof( example_reflect_api_t ),
        .func_api      = &g_example_reflect_api_struct,
        .deps          = { 0 },
        .dep_count     = 0,
        .init          = example_reflect_init,
        .reload        = example_reflect_reload,
        .exit          = example_reflect_exit,
        .ref_register   = MOD_REFLECT_FUNC( example_reflect ),
    };
    return &desc;
}

MOD_DEFINE_EXPORTS( example_reflect )

/*============================================================================================*/
