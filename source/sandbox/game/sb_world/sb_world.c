/*==============================================================================================

    sandbox/game/sb_world/sb_world.c -- world framework test bed (world / ent / comp / query).

    Exercises game/framework: entity alloc + generation recycling, ref-described component
    pools, attach/detach/get, dense-pool queries with probe, mutation-during-iteration,
    world reset, rebind (the hot-reload path), multi-world independence, and a component
    serialization round trip through ref_.

    Components here are hand-registered into a ref frame (the reflect_tool would normally
    generate this from REF_STRUCT annotations) so the sandbox has no codegen dependency
    and doubles as a reference for what generated registration produces.

==============================================================================================*/

#include <stdio.h>
#include <string.h>

#include "orb.h"
#include "engine/mod/mod_host.h"
#include "engine/sys/sys_host.h"
#include "engine/core/core_host.h"
#include "engine/ref/ref_host.h"
#include "engine/ref/ref_import.h"    // REF_OFFSETOF / REF_SIZEOF helpers for hand registration

#include "game/framework/framework.c"

/*==============================================================================================
    Check helper (matches sb_net / sb_prof / sb_core house style)
==============================================================================================*/

static int s_checks = 0;
static int s_fails  = 0;

static void
sb_check( bool ok, const char* what )
{
    s_checks++;
    if ( !ok )
    {
        s_fails++;
        printf( "    FAIL: %s\n", what );
    }
}

/*==============================================================================================
    Demo components

    Plain reflected PODs.  sbw_info_t shows the naming/parenting model: identity data is a
    component like any other -- the world core knows nothing about names or hierarchy.
==============================================================================================*/

typedef struct sbw_pos_s
{
    f32 x;
    f32 y;

} sbw_pos_t;

typedef struct sbw_vel_s
{
    f32 dx;
    f32 dy;

} sbw_vel_t;

typedef struct sbw_info_s
{
    sid_t name;      // interned display name
    ent_t parent;    // hierarchy edge: handle to the parent entity (ENT_INVALID = root)

} sbw_info_t;

/*==============================================================================================
    Hand-rolled ref registration (what reflect_tool generates from REF_STRUCT/REF_PROP)
==============================================================================================*/

static ref_field_t
sbw_field( const char* name, const char* type, u16 offset, u16 size )
{
    ref_field_t f = { 0 };
    f.name_id     = ref_intern( name );
    f.type_hash   = ref_hash_str( type );
    f.offset      = offset;
    f.size        = size;
    f.mods        = REF_NO_MODS;
    return f;
}

static u16
sbw_register_struct( const char* name, u16 size, u8 align, const ref_field_t* fields, u16 n )
{
    ref_type_t t = { 0 };
    t.name_id    = ref_intern( name );
    t.name_hash  = ref_hash_str( name );
    t.size       = size;
    t.align      = align;
    t.kind       = REF_KIND_STRUCT;
    return ref_register_type( &t, fields, n );
}

static bool
sbw_register_types( void )
{
    u16 frame = ref_push_frame( "sb_world" );

    {
        ref_field_t f[ 1 ] = {
            sbw_field( "off", "uint32_t", REF_OFFSETOF( sid_t, off ), REF_FIELD_SIZE( sid_t, off ) ),
        };
        sbw_register_struct( "sid_t", REF_SIZEOF( sid_t ), REF_ALIGNOF( sid_t ), f, 1 );
    }
    {
        ref_field_t f[ 2 ] = {
            sbw_field( "index", "uint32_t", REF_OFFSETOF( ent_t, index ), REF_FIELD_SIZE( ent_t, index ) ),
            sbw_field( "gen",   "uint32_t", REF_OFFSETOF( ent_t, gen ),   REF_FIELD_SIZE( ent_t, gen ) ),
        };
        sbw_register_struct( "ent_t", REF_SIZEOF( ent_t ), REF_ALIGNOF( ent_t ), f, 2 );
    }
    {
        ref_field_t f[ 2 ] = {
            sbw_field( "x", "float", REF_OFFSETOF( sbw_pos_t, x ), REF_FIELD_SIZE( sbw_pos_t, x ) ),
            sbw_field( "y", "float", REF_OFFSETOF( sbw_pos_t, y ), REF_FIELD_SIZE( sbw_pos_t, y ) ),
        };
        sbw_register_struct( "sbw_pos_t", REF_SIZEOF( sbw_pos_t ), REF_ALIGNOF( sbw_pos_t ), f, 2 );
    }
    {
        ref_field_t f[ 2 ] = {
            sbw_field( "dx", "float", REF_OFFSETOF( sbw_vel_t, dx ), REF_FIELD_SIZE( sbw_vel_t, dx ) ),
            sbw_field( "dy", "float", REF_OFFSETOF( sbw_vel_t, dy ), REF_FIELD_SIZE( sbw_vel_t, dy ) ),
        };
        sbw_register_struct( "sbw_vel_t", REF_SIZEOF( sbw_vel_t ), REF_ALIGNOF( sbw_vel_t ), f, 2 );
    }
    {
        ref_field_t f[ 2 ] = {
            sbw_field( "name",   "sid_t", REF_OFFSETOF( sbw_info_t, name ),   REF_FIELD_SIZE( sbw_info_t, name ) ),
            sbw_field( "parent", "ent_t", REF_OFFSETOF( sbw_info_t, parent ), REF_FIELD_SIZE( sbw_info_t, parent ) ),
        };
        sbw_register_struct( "sbw_info_t", REF_SIZEOF( sbw_info_t ), REF_ALIGNOF( sbw_info_t ), f, 2 );
    }

    return ref_finalize_frame( frame );
}

/*==============================================================================================
    A system is a plain function over pools -- nothing registers or schedules it.
==============================================================================================*/

static void
sys_move( world_id_t w, comp_id_t c_vel, comp_id_t c_pos, f32 dt )
{
    for ( world_iter_t it = world_query( w, c_vel ); world_iter_next( w, &it ); )
    {
        sbw_vel_t* v = it.data;
        sbw_pos_t* p = comp_get( w, it.ent, c_pos );    // "with pos": probe the other pool
        if ( !p )
            continue;
        p->x += v->dx * dt;
        p->y += v->dy * dt;
    }
}

/*==============================================================================================
    Tests
==============================================================================================*/

static world_t s_world_a;    // owners embed world_t; a module would put this in its state block
static world_t s_world_b;

static void
test_entities( world_id_t w )
{
    printf( "\n=== entities ===\n\n" );

    ent_t a = ent_create( w );
    sb_check( a.index == 1, "first entity gets index 1 (0 is reserved invalid)" );
    sb_check( ent_alive( w, a ), "created entity is alive" );
    sb_check( !ent_valid( ENT_INVALID ), "zeroed handle is invalid" );
    sb_check( !ent_alive( w, ENT_INVALID ), "zeroed handle is not alive" );

    ent_t b = ent_create( w );
    sb_check( b.index == 2, "second entity gets index 2" );
    sb_check( world_ent_count( w ) == 2, "ent_count tracks creates" );

    ent_destroy( w, a );
    sb_check( !ent_alive( w, a ), "destroyed handle is dead" );
    sb_check( world_ent_count( w ) == 1, "ent_count tracks destroys" );

    ent_t a2 = ent_create( w );
    sb_check( a2.index == a.index, "slot is recycled" );
    sb_check( a2.gen == a.gen + 1, "generation bumped on recycle" );
    sb_check( !ent_alive( w, a ), "old handle to recycled slot stays dead" );
    sb_check( ent_alive( w, a2 ), "new handle to recycled slot is alive" );

    ent_destroy( w, a2 );
    ent_destroy( w, b );
    sb_check( world_ent_count( w ) == 0, "world drains to zero" );
}

static void
test_components( world_id_t w, comp_id_t c_pos, comp_id_t c_vel )
{
    printf( "\n=== components ===\n\n" );

    sb_check( comp_register( w, "sbw_pos_t", 256 ) == c_pos, "re-register returns the same comp id" );
    sb_check( comp_find( w, "sbw_pos_t" ) == c_pos, "comp_find resolves by type name" );
    sb_check( comp_find( w, "nope_t" ) == COMP_INVALID, "comp_find rejects unknown types" );
    sb_check( comp_register( w, "nope_t", 8 ) == COMP_INVALID, "register rejects unreflected types" );
    sb_check( comp_type_id( w, c_pos ) == ref_find_type_by_name( "sbw_pos_t" ), "comp_type_id matches ref" );

    ent_t e = ent_create( w );

    sbw_pos_t* p = comp_attach( w, e, c_pos );
    sb_check( p != NULL, "attach returns data" );
    sb_check( p->x == 0.0f && p->y == 0.0f, "attached data is zeroed" );
    sb_check( comp_has( w, e, c_pos ), "comp_has after attach" );
    sb_check( comp_count( w, c_pos ) == 1, "pool count after attach" );

    p->x = 7.0f;
    sb_check( ( ( sbw_pos_t* )comp_get( w, e, c_pos ) )->x == 7.0f, "get returns written data" );
    sb_check( ( ( sbw_pos_t* )comp_attach( w, e, c_pos ) )->x == 7.0f, "re-attach hands back existing data" );
    sb_check( comp_count( w, c_pos ) == 1, "re-attach does not grow the pool" );

    sb_check( !comp_has( w, e, c_vel ), "comp_has false for unattached comp" );
    sb_check( comp_get( w, e, c_vel ) == NULL, "get NULL for unattached comp" );

    comp_detach( w, e, c_pos );
    sb_check( !comp_has( w, e, c_pos ), "comp_has false after detach" );
    sb_check( comp_get( w, e, c_pos ) == NULL, "get NULL after detach" );
    sb_check( comp_count( w, c_pos ) == 0, "pool count after detach" );

    comp_attach( w, e, c_pos );
    comp_attach( w, e, c_vel );
    ent_destroy( w, e );
    sb_check( comp_count( w, c_pos ) == 0 && comp_count( w, c_vel ) == 0, "destroy detaches every component" );
}

static void
test_stale_component_access( world_id_t w, comp_id_t c_pos )
{
    printf( "\n=== stale handles vs pools ===\n\n" );

    /* Two entities in one pool; destroying the first swap-moves the second's data. A stale
       handle must not reach through the recycled slot into the moved data. */
    ent_t a = ent_create( w );
    ent_t b = ent_create( w );

    sbw_pos_t* pa = comp_attach( w, a, c_pos );
    sbw_pos_t* pb = comp_attach( w, b, c_pos );
    pa->x         = 1.0f;
    pb->x         = 2.0f;

    ent_destroy( w, a );
    sb_check( comp_get( w, a, c_pos ) == NULL, "stale handle cannot reach pool data" );

    sbw_pos_t* pb2 = comp_get( w, b, c_pos );
    sb_check( pb2 && pb2->x == 2.0f, "survivor's data intact after swap-remove" );

    ent_destroy( w, b );
}

static void
test_queries( world_id_t w, comp_id_t c_pos, comp_id_t c_vel )
{
    printf( "\n=== queries + systems ===\n\n" );

    /* 100 entities with pos; velocity on the even half. */
    ent_t ents[ 100 ];
    for ( u32 i = 0; i < 100; i++ )
    {
        ents[ i ]    = ent_create( w );
        sbw_pos_t* p = comp_attach( w, ents[ i ], c_pos );
        p->x         = ( f32 )i;
        if ( ( i & 1 ) == 0 )
        {
            sbw_vel_t* v = comp_attach( w, ents[ i ], c_vel );
            v->dx        = 10.0f;
        }
    }

    u32 visited = 0;
    f32 sum     = 0.0f;
    bool all_alive = true;
    for ( world_iter_t it = world_query( w, c_pos ); world_iter_next( w, &it ); )
    {
        sbw_pos_t* p = it.data;
        all_alive    = all_alive && ent_alive( w, it.ent );
        sum += p->x;
        visited++;
    }
    sb_check( all_alive, "every iterated entity is alive" );
    sb_check( visited == 100, "query visits every pool element" );
    sb_check( sum == 4950.0f, "query sees every value exactly once" );

    /* Newest-attached-first order: the first visit is the last attach. */
    world_iter_t first = world_query( w, c_pos );
    world_iter_next( w, &first );
    sb_check( ent_eq( first.ent, ents[ 99 ] ), "iteration order is newest-attached first" );

    /* The movement system: only entities with BOTH vel and pos advance. */
    sys_move( w, c_vel, c_pos, 0.5f );
    sb_check( ( ( sbw_pos_t* )comp_get( w, ents[ 0 ], c_pos ) )->x == 5.0f, "system moved a vel+pos entity" );
    sb_check( ( ( sbw_pos_t* )comp_get( w, ents[ 1 ], c_pos ) )->x == 1.0f, "system skipped a pos-only entity" );

    /* Detach the CURRENT element mid-iteration: backward walk makes swap-remove safe. */
    u32 seen = 0;
    for ( world_iter_t it = world_query( w, c_vel ); world_iter_next( w, &it ); )
    {
        seen++;
        comp_detach( w, it.ent, c_vel );
    }
    sb_check( seen == 50, "detach-current during iteration visits everything once" );
    sb_check( comp_count( w, c_vel ) == 0, "detach-current during iteration empties the pool" );

    /* Destroy the current entity mid-iteration: same guarantee. */
    seen = 0;
    for ( world_iter_t it = world_query( w, c_pos ); world_iter_next( w, &it ); )
    {
        seen++;
        ent_destroy( w, it.ent );
    }
    sb_check( seen == 100, "destroy-current during iteration visits everything once" );
    sb_check( world_ent_count( w ) == 0 && comp_count( w, c_pos ) == 0, "world drained by iterated destroy" );
}

static void
test_hierarchy( world_id_t w, comp_id_t c_info )
{
    printf( "\n=== naming + parenting (info component) ===\n\n" );

    ent_t root  = ent_create( w );
    ent_t child = ent_create( w );

    sbw_info_t* ri = comp_attach( w, root, c_info );
    ri->name       = core()->sid_intern_cstr( "root" );

    sbw_info_t* ci = comp_attach( w, child, c_info );
    ci->name       = core()->sid_intern_cstr( "child" );
    ci->parent     = root;    // hierarchy is just a stored handle -- no tree structure anywhere

    /* Walk up: resolve the parent handle like any other. */
    sbw_info_t* walk = comp_get( w, child, c_info );
    sb_check( ent_alive( w, walk->parent ), "child's parent handle resolves" );
    sb_check( strcmp( core()->sid_cstr( ( ( sbw_info_t* )comp_get( w, walk->parent, c_info ) )->name ), "root" ) == 0,
              "parent's interned name reads back" );

    /* Destroying the parent orphans the child SAFELY: the stored handle goes stale, it
       never dangles.  Whatever policy a game wants (reparent, destroy subtree) is a
       system's decision, not the world's. */
    ent_destroy( w, root );
    walk = comp_get( w, child, c_info );
    sb_check( !ent_alive( w, walk->parent ), "orphaned parent handle reads as dead, never dangles" );

    ent_destroy( w, child );
}

static void
test_serialization( world_id_t w, comp_id_t c_pos )
{
    printf( "\n=== component serialization via ref ===\n\n" );

    /* The scene-file seam: any component can round-trip through the ref serializer using
       only its comp_type_id -- no per-type code.  Scenes (M3) are this, per entity. */
    ent_t      e = ent_create( w );
    sbw_pos_t* p = comp_attach( w, e, c_pos );
    p->x         = 3.5f;
    p->y         = -1.25f;

    u8     buf[ 256 ];
    size_t n = ref_write( p, comp_type_id( w, c_pos ), buf, sizeof buf );
    sb_check( n > 0, "component writes through ref serializer" );

    p->x = 0.0f;
    p->y = 0.0f;

    size_t          consumed = 0;
    ref_io_status_t st       = ref_read( p, comp_type_id( w, c_pos ), buf, n, &consumed );
    sb_check( st == REF_IO_OK, "component reads back through ref serializer" );
    sb_check( p->x == 3.5f && p->y == -1.25f, "round trip restores component data" );

    ent_destroy( w, e );
}

static void
test_reset_and_rebind( world_id_t wa )
{
    printf( "\n=== reset / rebind / multi-world ===\n\n" );

    comp_id_t c_pos = comp_find( wa, "sbw_pos_t" );

    ent_t      e = ent_create( wa );
    sbw_pos_t* p = comp_attach( wa, e, c_pos );
    p->x         = 42.0f;

    /* Rebind the same storage (what init()/reload() does after a hot-reload): everything
       survives because world_t holds no pointers. */
    world_id_t wa2 = world_bind( 0, &s_world_a );
    sb_check( wa2 == wa, "rebind returns the same world id" );
    sb_check( comp_register( wa, "sbw_pos_t", 256 ) == c_pos, "re-registration after rebind keeps the id" );
    sb_check( ent_alive( wa, e ), "entities survive rebind" );
    sb_check( ( ( sbw_pos_t* )comp_get( wa, e, c_pos ) )->x == 42.0f, "component data survives rebind" );

    /* A second, independent world in another slot. */
    world_id_t wb = world_bind( 1, &s_world_b );
    sb_check( wb == 1, "second world binds to slot 1" );
    comp_id_t cb_pos = comp_register( wb, "sbw_pos_t", 8 );
    ent_t     eb     = ent_create( wb );
    comp_attach( wb, eb, cb_pos );
    sb_check( world_ent_count( wb ) == 1 && world_ent_count( wa ) == 1, "worlds are independent" );

    /* Pool cap: the 9th attach into a cap-8 pool must fail cleanly. */
    bool full_ok = true;
    for ( u32 i = 0; i < 7; i++ )
        full_ok &= ( comp_attach( wb, ent_create( wb ), cb_pos ) != NULL );
    sb_check( full_ok, "pool accepts up to its cap" );
    sb_check( comp_attach( wb, ent_create( wb ), cb_pos ) == NULL, "pool rejects past its cap" );

    /* Reset: handles die, registrations and pools remain usable. */
    world_reset( wa );
    sb_check( !ent_alive( wa, e ), "reset kills outstanding handles" );
    sb_check( world_ent_count( wa ) == 0 && comp_count( wa, c_pos ) == 0, "reset empties the world" );
    sb_check( comp_find( wa, "sbw_pos_t" ) == c_pos, "registrations survive reset" );
    ent_t e2 = ent_create( wa );
    sb_check( e2.index == 1 && ent_alive( wa, e2 ), "post-reset allocation restarts at index 1, alive" );
}

/*==============================================================================================
    main
==============================================================================================*/

int
main( int argc, char** argv )
{
    UNUSED( argc );
    UNUSED( argv );

    printf( "=== sb_world ===\n" );

    mod_system_init();
    ref_wire_mod_callbacks();

    mod_static( sys );
    mod_static( ref );
    mod_static( core );

    if ( !mod_init_all() )
    {
        fprintf( stderr, "init: %s\n", mod_last_error() );
        return 1;
    }

    if ( !sbw_register_types() )
    {
        fprintf( stderr, "ref registration failed\n" );
        return 1;
    }

    world_id_t w = world_bind( 0, &s_world_a );
    sb_check( w == 0, "world binds to slot 0" );

    comp_id_t c_pos  = comp_register( w, "sbw_pos_t", 256 );
    comp_id_t c_vel  = comp_register( w, "sbw_vel_t", 256 );
    comp_id_t c_info = comp_register( w, "sbw_info_t", 256 );
    sb_check( c_pos == 0 && c_vel == 1 && c_info == 2, "comp ids follow registration order" );

    test_entities( w );
    test_components( w, c_pos, c_vel );
    test_stale_component_access( w, c_pos );
    test_queries( w, c_pos, c_vel );
    test_hierarchy( w, c_info );
    test_serialization( w, c_pos );
    test_reset_and_rebind( w );

    printf( "\nsb_world: %d checks, %d failed\n", s_checks, s_fails );

    mod_system_exit();
    return s_fails ? 1 : 0;
}

/*============================================================================================*/
