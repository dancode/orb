/*==============================================================================================

    c11_di.c -- Isolated C11 designated initializer tests.

    Included by c11_test.c (unity build). Zero engine dependencies.
    Compiled with /std:c11 /TC -- same flags as all other orb sources.

    Goal: confirm whether MSVC squiggles on designated initializers are a
    compiler/language reject or a VS IntelliSense parse artifact.

==============================================================================================*/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/*==============================================================================================
    Test 1: basic flat struct
==============================================================================================*/

typedef struct
{
    int   x;
    int   y;
    float z;
} vec3i_t;

static void
test_basic( void )
{
    /* Basic designated initializer -- C99/C11 standard, should always compile. */
    vec3i_t v = { .x = 1, .y = 2, .z = 3.0f };
    printf( "test_basic: x=%d y=%d z=%.1f\n", v.x, v.y, v.z );
}

/*==============================================================================================
    Test 2: static initializer (global storage)
==============================================================================================*/

static vec3i_t g_origin = { .x = 0, .y = 0, .z = 0.0f };

static void
test_static_global( void )
{
    printf( "test_static_global: x=%d y=%d z=%.1f\n", g_origin.x, g_origin.y, g_origin.z );
}

/*==============================================================================================
    Test 3: static local (same as mod_desc_t pattern in engine)
==============================================================================================*/

typedef struct
{
    int          version;
    uint32_t     state_size;
    bool         active;
    const char*  name;
} desc_t;

static desc_t*
get_desc( void )
{
    /* This mirrors the engine pattern: static local with designated initializers. */
    static desc_t d = {
        .version    = 1,
        .state_size = 64,
        .active     = true,
        .name       = "test",
    };
    return &d;
}

static void
test_static_local( void )
{
    desc_t* d = get_desc();
    printf( "test_static_local: version=%d size=%u active=%d name=%s\n",
            d->version, d->state_size, ( int )d->active, d->name );
}

/*==============================================================================================
    Test 4: nested struct designated initializers
==============================================================================================*/

typedef struct
{
    int a;
    int b;
} inner_t;

typedef struct
{
    inner_t  pos;
    inner_t  dim;
    int      flags;
} outer_t;

static void
test_nested( void )
{
    outer_t o = {
        .pos   = { .a = 10, .b = 20 },
        .dim   = { .a = 100, .b = 200 },
        .flags = 0xFF,
    };
    printf( "test_nested: pos=(%d,%d) dim=(%d,%d) flags=0x%X\n",
            o.pos.a, o.pos.b, o.dim.a, o.dim.b, o.flags );
}

/*==============================================================================================
    Test 5: struct with function pointers (mirrors core_api_t / mod_desc_t)
==============================================================================================*/

typedef struct
{
    int   version;
    void  ( *init )  ( void );
    void  ( *exit )  ( void );
    int   count;
} vtable_t;

static void stub_init( void ) { printf( "  stub_init called\n" ); }
static void stub_exit( void ) { printf( "  stub_exit called\n" ); }

static const vtable_t g_vtable = {
    .version = 2,
    .init    = stub_init,
    .exit    = stub_exit,
    .count   = 42,
};

static void
test_vtable( void )
{
    printf( "test_vtable: version=%d count=%d\n", g_vtable.version, g_vtable.count );
    g_vtable.init();
    g_vtable.exit();
}

/*==============================================================================================
    Runner
==============================================================================================*/

void
c11_di_run( void )
{
    printf( "--- c11 designated initializer tests ---\n" );
    test_basic();
    test_static_global();
    test_static_local();
    test_nested();
    test_vtable();
    printf( "--- all tests complete ---\n" );
}
