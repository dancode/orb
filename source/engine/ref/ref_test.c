/*==============================================================================================

    engine/ref/ref_test.c - Exercise the ref_ reflection system.

    Compiled as a standalone TU in the test sandbox (sb_engine_core_reflect).
    Depends only on engine_rs.  ref_init() uses the internal string pool so no
    external interner is needed.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "orb.h"
#include "engine/ref/ref_host.h"
#include "engine/ref/ref_import.h"

#define test_intern ref_intern
#define test_cstr   ref_cstr

typedef struct ref_test_vec3_s
{
    float x, y, z;

} ref_test_vec3_t;

typedef struct ref_test_transform_s
{
    ref_test_vec3_t position;
    ref_test_vec3_t rotation;
    ref_test_vec3_t scale;

} ref_test_transform_t;

typedef struct ref_test_entity_s
{
    int32_t             id;
    char                name[ 64 ];
    ref_test_transform_t transform;
    float               health;
    ref_test_vec3_t*     velocity;   /* pointer field    */
    const char*         label;      /* const char*      */
    ref_test_vec3_t*     slots[ 8 ]; /* array-of-pointer */

} ref_test_entity_t;

/*==============================================================================================
    Prim-field test type

    One field per non-void, non-invalid primitive.  After finalize each field->type_id must
    equal the corresponding REF_PRIM_* enum value and field->kind must be REF_KIND_PRIM.

    REF_PRIM_VOID is intentionally absent -- it is only valid as the return-descriptor slot
    (fields[0]) inside a REF_KIND_FUNCTION type; see test_function_sigs.
==============================================================================================*/

typedef struct ref_test_prim_fields_s
{
    bool     b;
    int8_t   i8;
    uint8_t  u8;
    int16_t  i16;
    uint16_t u16;
    int32_t  i32;
    uint32_t u32;
    int64_t  i64;
    uint64_t u64;
    float    f32;
    double   f64;
    char     c;
    char*    str;   /* base type "string" -- opaque pointer-sized string handle */

} ref_test_prim_fields_t;

/*==============================================================================================
    Modifier-shape test type

    One C field per ref_mods_t value.  const-qualified variants (CONST_VALUE, PTR_TO_CONST,
    CONST_PTR) share the same underlying C layout as their non-const counterparts -- the mods
    tag is stored independently of the C qualifier.  ARRAY_PTR uses a plain pointer slot
    because T(*)[N] is always pointer-sized; the reflect_tool parser cannot emit this
    declarator but the runtime stores and prints it correctly.

    REF_MODS_FUNCTION is exercised separately in test_function_sigs.
==============================================================================================*/

typedef struct ref_test_mods_s
{
    int32_t   plain;      /* REF_MODS_VALUE        T              */
    int32_t   cv;         /* REF_MODS_CONST_VALUE  const T        */
    int32_t*  ptr;        /* REF_MODS_PTR          T*             */
    int32_t*  ptc;        /* REF_MODS_PTR_TO_CONST const T*       */
    int32_t*  cptr;       /* REF_MODS_CONST_PTR    T* const       */
    int32_t** pptr;       /* REF_MODS_PTR_PTR      T**            */
    int32_t   arr[ 4 ];   /* REF_MODS_ARRAY        T[N]  aux=4    */
    int32_t*  parr[ 4 ];  /* REF_MODS_PTR_ARRAY    T*[N] aux=4   */
    int32_t*  aptr;       /* REF_MODS_ARRAY_PTR    T(*)[N]        */

} ref_test_mods_t;

/*==============================================================================================
    Registration helpers

    These functions demonstrate the manual registration API that generated code (from the
    ref_import.h macros) produces automatically. Each helper interns names, hashes type names,
    fills ref_type_t / ref_field_t descriptors, and calls ref_register_type / ref_register_enum.

    In production, a code-gen tool emits these calls from annotated C headers. Here they are
    hand-written so the tests have no dependency on the generator and can serve as a reference
    for what the generated output should look like.
==============================================================================================*/

static void
ref_test_register_vec3( void )
{
    const uint32_t h_vec3  = ref_hash_str( "vec3_t" );
    const uint32_t h_float = ref_hash_str( "float" );

    ref_type_t      type    = { 0 };
    type.name_id           = test_intern( "vec3_t" );
    type.name_hash        = h_vec3;
    type.size              = REF_SIZEOF( ref_test_vec3_t );
    type.align             = REF_ALIGNOF( ref_test_vec3_t );
    type.kind              = REF_KIND_STRUCT;

    ref_field_t fields[ 3 ] = { 0 };

    fields[ 0 ].name_id    = test_intern( "x" );
    fields[ 0 ].type_hash  = h_float;
    fields[ 0 ].offset     = REF_OFFSETOF( ref_test_vec3_t, x );
    fields[ 0 ].size       = REF_FIELD_SIZE( ref_test_vec3_t, x );
    fields[ 0 ].mods       = REF_NO_MODS;

    fields[ 1 ].name_id    = test_intern( "y" );
    fields[ 1 ].type_hash  = h_float;
    fields[ 1 ].offset     = REF_OFFSETOF( ref_test_vec3_t, y );
    fields[ 1 ].size       = REF_FIELD_SIZE( ref_test_vec3_t, y );

    fields[ 2 ].name_id    = test_intern( "z" );
    fields[ 2 ].type_hash  = h_float;
    fields[ 2 ].offset     = REF_OFFSETOF( ref_test_vec3_t, z );
    fields[ 2 ].size       = REF_FIELD_SIZE( ref_test_vec3_t, z );

    uint16_t    tid        = ref_register_type( &type, fields, 3 );

    ref_attrib_t a          = { 0 };
    a.name_id              = test_intern( "serializable" );
    a.type                 = REF_ATTR_BOOL;
    a.value.u32            = 1;
    ref_type_add_attr( tid, &a );
}

static void
ref_test_register_transform( void )
{
    const uint32_t h_xform = ref_hash_str( "transform_t" );
    const uint32_t h_vec3  = ref_hash_str( "vec3_t" );

    ref_type_t      type    = { 0 };
    type.name_id           = test_intern( "transform_t" );
    type.name_hash         = h_xform;
    type.size              = REF_SIZEOF( ref_test_transform_t );
    type.align             = REF_ALIGNOF( ref_test_transform_t );
    type.kind              = REF_KIND_STRUCT;

    ref_field_t fields[ 3 ] = { 0 };

    fields[ 0 ].name_id    = test_intern( "position" );
    fields[ 0 ].type_hash  = h_vec3;
    fields[ 0 ].offset     = REF_OFFSETOF( ref_test_transform_t, position );
    fields[ 0 ].size       = REF_FIELD_SIZE( ref_test_transform_t, position );

    fields[ 1 ].name_id    = test_intern( "rotation" );
    fields[ 1 ].type_hash  = h_vec3;
    fields[ 1 ].offset     = REF_OFFSETOF( ref_test_transform_t, rotation );
    fields[ 1 ].size       = REF_FIELD_SIZE( ref_test_transform_t, rotation );

    fields[ 2 ].name_id    = test_intern( "scale" );
    fields[ 2 ].type_hash  = h_vec3;
    fields[ 2 ].offset     = REF_OFFSETOF( ref_test_transform_t, scale );
    fields[ 2 ].size       = REF_FIELD_SIZE( ref_test_transform_t, scale );

    ref_register_type( &type, fields, 3 );
}

static void
ref_test_register_entity( void )
{
    const uint32_t h_entity = ref_hash_str( "entity_t" );
    const uint32_t h_int32  = ref_hash_str( "int32_t" );
    const uint32_t h_char   = ref_hash_str( "char" );
    const uint32_t h_xform  = ref_hash_str( "transform_t" );
    const uint32_t h_float  = ref_hash_str( "float" );
    const uint32_t h_vec3   = ref_hash_str( "vec3_t" );

    ref_type_t      type     = { 0 };
    type.name_id            = test_intern( "entity_t" );
    type.name_hash          = h_entity;
    type.size               = REF_SIZEOF( ref_test_entity_t );
    type.align              = REF_ALIGNOF( ref_test_entity_t );
    type.kind               = REF_KIND_STRUCT;

    ref_field_t fields[ 7 ]  = { 0 };

    /* id : int32_t                                  */
    fields[ 0 ].name_id   = test_intern( "id" );
    fields[ 0 ].type_hash = h_int32;
    fields[ 0 ].offset    = REF_OFFSETOF( ref_test_entity_t, id );
    fields[ 0 ].size      = REF_FIELD_SIZE( ref_test_entity_t, id );

    /* name : char[64]    base=char, mods=[ARRAY], aux=64 */
    fields[ 1 ].name_id   = test_intern( "name" );
    fields[ 1 ].type_hash = h_char;
    fields[ 1 ].offset    = REF_OFFSETOF( ref_test_entity_t, name );
    fields[ 1 ].size      = REF_FIELD_SIZE( ref_test_entity_t, name );
    fields[ 1 ].mods      = REF_MODS_ARRAY;
    fields[ 1 ].aux       = 64;

    /* transform : transform_t                       */
    fields[ 2 ].name_id   = test_intern( "transform" );
    fields[ 2 ].type_hash = h_xform;
    fields[ 2 ].offset    = REF_OFFSETOF( ref_test_entity_t, transform );
    fields[ 2 ].size      = REF_FIELD_SIZE( ref_test_entity_t, transform );

    /* health : float                                */
    fields[ 3 ].name_id   = test_intern( "health" );
    fields[ 3 ].type_hash = h_float;
    fields[ 3 ].offset    = REF_OFFSETOF( ref_test_entity_t, health );
    fields[ 3 ].size      = REF_FIELD_SIZE( ref_test_entity_t, health );

    /* velocity : vec3_t*  base=vec3, mods=[PTR]     */
    fields[ 4 ].name_id   = test_intern( "velocity" );
    fields[ 4 ].type_hash = h_vec3;
    fields[ 4 ].offset    = REF_OFFSETOF( ref_test_entity_t, velocity );
    fields[ 4 ].size      = REF_FIELD_SIZE( ref_test_entity_t, velocity );
    fields[ 4 ].mods      = REF_MODS_PTR;

    /* label : const char*  mods=PTR_TO_CONST */
    fields[ 5 ].name_id    = test_intern( "label" );
    fields[ 5 ].type_hash  = h_char;
    fields[ 5 ].offset     = REF_OFFSETOF( ref_test_entity_t, label );
    fields[ 5 ].size       = REF_FIELD_SIZE( ref_test_entity_t, label );
    fields[ 5 ].mods       = REF_MODS_PTR_TO_CONST;

    /* slots : vec3_t*[8]   mods=[PTR, ARRAY], aux=8 */
    fields[ 6 ].name_id   = test_intern( "slots" );
    fields[ 6 ].type_hash = h_vec3;
    fields[ 6 ].offset    = REF_OFFSETOF( ref_test_entity_t, slots );
    fields[ 6 ].size      = REF_FIELD_SIZE( ref_test_entity_t, slots );
    fields[ 6 ].mods      = REF_MODS_PTR_ARRAY;
    fields[ 6 ].aux       = 8;

    uint16_t tid          = ref_register_type( &type, fields, 7 );

    /* Attach two attributes to the 'health' field to exercise the repeated-entry path. */
    const ref_field_t* health = ref_find_field( tid, "health" );
    if ( health )
    {
        uint16_t    fid = ( uint16_t )( health - ref_get_field( 0 ) );
        ref_attrib_t a   = { 0 };
        a.name_id       = test_intern( REF_ANAME_RANGE );
        a.type          = REF_ATTR_FLOAT;
        a.flags         = REF_AF_CLAMP;
        a.value.f32     = 0.0f;
        ref_field_add_attr( fid, &a );
        a.value.f32     = 100.0f;
        ref_field_add_attr( fid, &a );
    }
}

/*==============================================================================================
    Test cases

    Each test is self-contained: it calls ref_init() at the top and ref_exit() at the bottom
    so the registry is always in a clean state. Tests must be run sequentially (they share
    the single global g_ref), but can be toggled individually by the if(0) block in ref_run_tests.
==============================================================================================*/

/*----------------------------------------------------------------------------------------------
    test_primitives: all 15 built-in types installed by ref_init()

    Verifies: name, type_id == ref_prim_t enum value, size, align, kind, and name-based lookup.
    The enum-value == type_id invariant is load-bearing: callers use REF_PRIM_F32 etc. directly
    as type IDs without going through the hash table.
----------------------------------------------------------------------------------------------*/

static void
test_primitives( void )
{
    printf( "\n=== ref: primitive type table ===\n" );

    ref_init();

    static const struct
    {
        ref_prim_t  id; 
        const char* name;
        uint16_t    size;
        uint8_t     align;
    } EXPECTED[ REF_PRIM_COUNT ] = {
        { REF_PRIM_INVALID, "invalid",  0,               0               },
        { REF_PRIM_VOID,    "void",     0,               0               },
        { REF_PRIM_BOOL,    "bool",     1,               1               },
        { REF_PRIM_CHAR,    "char",     1,               1               },
        { REF_PRIM_I8,      "int8_t",   1,               1               },
        { REF_PRIM_U8,      "uint8_t",  1,               1               },
        { REF_PRIM_I16,     "int16_t",  2,               2               },
        { REF_PRIM_U16,     "uint16_t", 2,               2               },
        { REF_PRIM_I32,     "int32_t",  4,               4               },
        { REF_PRIM_U32,     "uint32_t", 4,               4               },
        { REF_PRIM_I64,     "int64_t",  8,               8               },
        { REF_PRIM_U64,     "uint64_t", 8,               8               },
        { REF_PRIM_F32,     "float",    4,               4               },
        { REF_PRIM_F64,     "double",   8,               8               },
        { REF_PRIM_STRING,  "string",   sizeof( char* ), sizeof( char* ) },
    };

    bool all_ok = true;
    for ( int i = 0; i < REF_PRIM_COUNT; i++ )
    {
        const ref_type_t* t  = ref_get_type( (uint16_t)i );
        bool              ok = true;

        if ( !t )
        {
            printf( "  [FAIL] prim %d: ref_get_type returned NULL\n", i );
            all_ok = false;
            continue;
        }

        if ( (int)EXPECTED[ i ].id != i )                                    ok = false;
        if ( strcmp( test_cstr( t->name_id ), EXPECTED[ i ].name ) != 0 )    ok = false;
        if ( t->size  != EXPECTED[ i ].size  )                               ok = false;
        if ( t->align != EXPECTED[ i ].align )                               ok = false;
        if ( t->kind  != REF_KIND_PRIM )                                     ok = false;

        /* name-based lookup must round-trip to the same slot */
        uint16_t found = ref_find_type_by_name( EXPECTED[ i ].name );
        if ( found != (uint16_t)i )                                          ok = false;

        printf( "  [%s] %-10s  id=%-2d  size=%-2u  align=%u\n",
                ok ? "ok  " : "FAIL", EXPECTED[ i ].name, i, t->size, t->align );

        if ( !ok ) all_ok = false;
    }

    assert( all_ok );
    ref_exit();
}

/*----------------------------------------------------------------------------------------------
    test_prim_fields: lazy resolution maps every non-void primitive to its REF_PRIM_* type_id

    Registers a flat struct with one field per non-void, non-invalid primitive.  After
    ref_finalize_frame each field->type_id must equal the corresponding REF_PRIM_* enum value
    and field->kind must be REF_KIND_PRIM.
----------------------------------------------------------------------------------------------*/

static void
test_prim_fields( void )
{
    printf( "\n=== ref: primitive field resolution ===\n" );

    ref_init();
    uint16_t our_frame = ref_push_frame( "test_prim_fields" );

    /* create a struct of all our primitive fields */
    ref_type_t type = { 0 };
    type.name_id   = test_intern( "prim_fields_t" );
    type.name_hash = ref_hash_str( "prim_fields_t" );
    type.size      = REF_SIZEOF( ref_test_prim_fields_t );
    type.align     = REF_ALIGNOF( ref_test_prim_fields_t );
    type.kind      = REF_KIND_STRUCT;

    ref_field_t fields[ 13 ] = { 0 };
    int         fi           = 0;

    /* bool */
    fields[ fi ].name_id   = test_intern( "b" );
    fields[ fi ].type_hash = ref_hash_str( "bool" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, b );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, b );
    fi++;

    /* int8_t */
    fields[ fi ].name_id   = test_intern( "i8" );
    fields[ fi ].type_hash = ref_hash_str( "int8_t" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, i8 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, i8 );
    fi++;

    /* uint8_t */
    fields[ fi ].name_id   = test_intern( "u8" );
    fields[ fi ].type_hash = ref_hash_str( "uint8_t" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, u8 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, u8 );
    fi++;

    /* int16_t */
    fields[ fi ].name_id   = test_intern( "i16" );
    fields[ fi ].type_hash = ref_hash_str( "int16_t" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, i16 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, i16 );
    fi++;

    /* uint16_t */
    fields[ fi ].name_id   = test_intern( "u16" );
    fields[ fi ].type_hash = ref_hash_str( "uint16_t" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, u16 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, u16 );
    fi++;

    /* int32_t */
    fields[ fi ].name_id   = test_intern( "i32" );
    fields[ fi ].type_hash = ref_hash_str( "int32_t" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, i32 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, i32 );
    fi++;

    /* uint32_t */
    fields[ fi ].name_id   = test_intern( "u32" );
    fields[ fi ].type_hash = ref_hash_str( "uint32_t" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, u32 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, u32 );
    fi++;

    /* int64_t */
    fields[ fi ].name_id   = test_intern( "i64" );
    fields[ fi ].type_hash = ref_hash_str( "int64_t" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, i64 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, i64 );
    fi++;

    /* uint64_t */
    fields[ fi ].name_id   = test_intern( "u64" );
    fields[ fi ].type_hash = ref_hash_str( "uint64_t" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, u64 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, u64 );
    fi++;

    /* float */
    fields[ fi ].name_id   = test_intern( "f32" );
    fields[ fi ].type_hash = ref_hash_str( "float" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, f32 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, f32 );
    fi++;

    /* double */
    fields[ fi ].name_id   = test_intern( "f64" );
    fields[ fi ].type_hash = ref_hash_str( "double" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, f64 );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, f64 );
    fi++;

    /* char */
    fields[ fi ].name_id   = test_intern( "c" );
    fields[ fi ].type_hash = ref_hash_str( "char" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, c );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, c );
    fi++;

    /* string: opaque pointer-sized handle; C field is char* but base type name is "string" */
    fields[ fi ].name_id   = test_intern( "str" );
    fields[ fi ].type_hash = ref_hash_str( "string" );
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_prim_fields_t, str );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_prim_fields_t, str );
    fi++;

    /* register the struct + field array + count */
    uint16_t tid = ref_register_type( &type, fields, (uint16_t)fi );
    ref_finalize_frame( our_frame );

    static const struct { const char* name; ref_prim_t expected_id; }
    EXPECTED[] = {
        { "b",   REF_PRIM_BOOL   },
        { "i8",  REF_PRIM_I8     },
        { "u8",  REF_PRIM_U8     },
        { "i16", REF_PRIM_I16    },
        { "u16", REF_PRIM_U16    },
        { "i32", REF_PRIM_I32    },
        { "u32", REF_PRIM_U32    },
        { "i64", REF_PRIM_I64    },
        { "u64", REF_PRIM_U64    },
        { "f32", REF_PRIM_F32    },
        { "f64", REF_PRIM_F64    },
        { "c",   REF_PRIM_CHAR   },
        { "str", REF_PRIM_STRING },
    };

    bool all_ok = true;
    for ( int i = 0; i < fi; i++ )
    {
        const ref_field_t* f       = ref_find_field( tid, EXPECTED[ i ].name );
        bool               id_ok   = f && ( f->type_id == (uint16_t)EXPECTED[ i ].expected_id );
        bool               kind_ok = f && ( f->kind    == REF_KIND_PRIM );
        bool               ok      = id_ok && kind_ok;

        printf( "  [%s] %-10s -> type_id=%-2u (expect %-2u)  kind=%s\n",
                ok ? "ok  " : "FAIL", EXPECTED[ i ].name,
                f ? f->type_id : 0xFFFFu, (uint16_t)EXPECTED[ i ].expected_id,
                ( f && f->kind == REF_KIND_PRIM ) ? "PRIM" : "WRONG" );

        if ( !ok ) all_ok = false;
    }

    assert( all_ok );

    ref_pop_frame( our_frame );
    ref_exit();
}

/*----------------------------------------------------------------------------------------------
    test_mods: all REF_MODS_* values stored and retrieved correctly

    Registers one field per modifier shape.  After finalize each field->mods must equal the
    value that was set at registration time.  ARRAY_PTR (0x0102, T(*)[N]) is included here
    even though the reflect_tool parser cannot emit it -- the runtime stores it correctly and
    ref_print_type renders it.  FUNCTION is covered by test_function_sigs.
----------------------------------------------------------------------------------------------*/

static void
test_mods( void )
{
    printf( "\n=== ref: modifier shapes ===\n" );

    ref_init();
    uint16_t our_frame = ref_push_frame( "test_mods" );

    ref_type_t type = { 0 };
    type.name_id   = test_intern( "mods_t" );
    type.name_hash = ref_hash_str( "mods_t" );
    type.size      = REF_SIZEOF( ref_test_mods_t );
    type.align     = REF_ALIGNOF( ref_test_mods_t );
    type.kind      = REF_KIND_STRUCT;

    const uint32_t h_i32 = ref_hash_str( "int32_t" );

    ref_field_t fields[ 9 ] = { 0 };
    int         fi           = 0;

    /* VALUE: T */
    fields[ fi ].name_id   = test_intern( "plain" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, plain );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, plain );
    fields[ fi ].mods      = REF_MODS_VALUE;
    fi++;

    /* CONST_VALUE: const T */
    fields[ fi ].name_id   = test_intern( "cv" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, cv );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, cv );
    fields[ fi ].mods      = REF_MODS_CONST_VALUE;
    fi++;

    /* PTR: T* */
    fields[ fi ].name_id   = test_intern( "ptr" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, ptr );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, ptr );
    fields[ fi ].mods      = REF_MODS_PTR;
    fi++;

    /* PTR_TO_CONST: const T* */
    fields[ fi ].name_id   = test_intern( "ptc" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, ptc );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, ptc );
    fields[ fi ].mods      = REF_MODS_PTR_TO_CONST;
    fi++;

    /* CONST_PTR: T* const */
    fields[ fi ].name_id   = test_intern( "cptr" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, cptr );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, cptr );
    fields[ fi ].mods      = REF_MODS_CONST_PTR;
    fi++;

    /* PTR_PTR: T** */
    fields[ fi ].name_id   = test_intern( "pptr" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, pptr );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, pptr );
    fields[ fi ].mods      = REF_MODS_PTR_PTR;
    fi++;

    /* ARRAY: T[N], aux = element count */
    fields[ fi ].name_id   = test_intern( "arr" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, arr );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, arr );
    fields[ fi ].mods      = REF_MODS_ARRAY;
    fields[ fi ].aux       = 4;
    fi++;

    /* PTR_ARRAY: T*[N], aux = element count */
    fields[ fi ].name_id   = test_intern( "parr" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, parr );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, parr );
    fields[ fi ].mods      = REF_MODS_PTR_ARRAY;
    fields[ fi ].aux       = 4;
    fi++;

    /* ARRAY_PTR: T(*)[N] -- pointer to a fixed-size array.  Same pointer-sized layout as T*.
       The reflect_tool parser cannot emit this declarator; registered manually here. */
    fields[ fi ].name_id   = test_intern( "aptr" );
    fields[ fi ].type_hash = h_i32;
    fields[ fi ].offset    = REF_OFFSETOF( ref_test_mods_t, aptr );
    fields[ fi ].size      = REF_FIELD_SIZE( ref_test_mods_t, aptr );
    fields[ fi ].mods      = REF_MODS_ARRAY_PTR;
    fields[ fi ].aux       = 4;
    fi++;

    uint16_t tid = ref_register_type( &type, fields, (uint16_t)fi );
    ref_finalize_frame( our_frame );

    ref_print_type( tid );

    static const struct { const char* name; uint16_t mods; }
    EXPECTED[] = {
        { "plain", REF_MODS_VALUE        },
        { "cv",    REF_MODS_CONST_VALUE  },
        { "ptr",   REF_MODS_PTR          },
        { "ptc",   REF_MODS_PTR_TO_CONST },
        { "cptr",  REF_MODS_CONST_PTR    },
        { "pptr",  REF_MODS_PTR_PTR      },
        { "arr",   REF_MODS_ARRAY        },
        { "parr",  REF_MODS_PTR_ARRAY    },
        { "aptr",  REF_MODS_ARRAY_PTR    },
    };

    bool all_ok = true;
    for ( int i = 0; i < fi; i++ )
    {
        const ref_field_t* f  = ref_find_field( tid, EXPECTED[ i ].name );
        bool               ok = f && ( f->mods == EXPECTED[ i ].mods );
        printf( "  [%s] %-6s  mods=0x%04x (expect 0x%04x)\n",
                ok ? "ok  " : "FAIL", EXPECTED[ i ].name,
                f ? (unsigned)f->mods : 0xFFFFu, (unsigned)EXPECTED[ i ].mods );
        if ( !ok ) all_ok = false;
    }

    assert( all_ok );

    ref_pop_frame( our_frame );
    ref_exit();
}

static void
test_basic( void )
{
    /* Smoke test: register three types with cross-references, finalize, print, and pop.
       Verifies the push/register/finalize/pop lifecycle without any special cases. */
    printf( "\n=== rs: basic registration ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    ref_test_register_vec3();
    ref_test_register_transform();
    ref_test_register_entity();

    bool ok = ref_finalize_frame( game );
    printf( "finalize: %s\n", ok ? "OK" : "FAIL" );

    ref_print_frame( game );

    ref_pop_frame( game );
    ref_exit();
}

static void
test_hot_reload_rs( void )
{
    /* Simulates a DLL hot-reload: pop the "game" frame, re-register identical types, and
       verify that schema_hash is stable. An unchanged schema_hash means hot-loaded save
       data can be reused without a reload warning. */
    printf( "\n=== rs: hot reload via pop + repush ===\n" );

    ref_init();

    uint16_t game = ref_push_frame( "game" );
    ref_test_register_vec3();
    ref_test_register_transform();
    ref_test_register_entity();
    ref_finalize_frame( game );

    uint16_t t_count, f_count, fr_count;
    ref_get_stats( &t_count, &f_count, &fr_count );
    printf( "  before reload: types=%u fields=%u frames=%u\n", t_count, f_count, fr_count );

    /* Drop and reload the 'game' frame. */
    ref_pop_frame( game );
    ref_get_stats( &t_count, &f_count, &fr_count );
    printf( "  after pop    : types=%u fields=%u frames=%u\n", t_count, f_count, fr_count );

    game = ref_push_frame( "game" );
    ref_test_register_vec3();
    ref_test_register_transform();
    ref_test_register_entity();
    ref_finalize_frame( game );

    ref_get_stats( &t_count, &f_count, &fr_count );
    printf( "  after reload : types=%u fields=%u frames=%u\n", t_count, f_count, fr_count );

    /* Schema hash should be identical because the layout did not change. */
    uint16_t v1 = ref_find_type_by_name( "vec3_t" );
    if ( v1 != REF_TYPE_INVALID )
        printf( "  vec3_t schema_hash = 0x%08x\n", ref_get_type( v1 )->schema_hash );

    ref_pop_frame( game );
    ref_exit();
}

static void
test_mod_decode( void )
{
    /* Exercises ref_field_describe() by registering entity_t which contains fields with
       every interesting modifier (PTR, ARRAY, PTR_ARRAY, PTR_TO_CONST) and printing them. */
    printf( "\n=== rs: mod chain pretty-print ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    ref_test_register_vec3();
    ref_test_register_transform();
    ref_test_register_entity();
    ref_finalize_frame( game );

    uint16_t eid = ref_find_type_by_name( "entity_t" );
    ref_print_type( eid );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Enum test
==============================================================================================*/

typedef enum ref_test_color_e
{
    REF_TEST_COLOR_RED   = 0,
    REF_TEST_COLOR_GREEN = 1,
    REF_TEST_COLOR_BLUE  = 2,
    REF_TEST_COLOR_ALPHA = 0xFF,
} ref_test_color_t;

static void
test_enums( void )
{
    printf( "\n=== rs: enum registration ===\n" );

    ref_init();
    uint16_t  game = ref_push_frame( "game" );

    ref_type_t type = { 0 };
    type.name_id   = test_intern( "color_t" );
    type.name_hash = ref_hash_str( "color_t" );
    type.size      = ( uint16_t )sizeof( ref_test_color_t );
    type.align     = ( uint8_t )_Alignof( ref_test_color_t );
    /* kind is forced to REF_KIND_ENUM by ref_register_enum */

    ref_enum_t entries[ 4 ] = {
        {.name_id = test_intern( "RED" ),   .value = REF_TEST_COLOR_RED  },
        {.name_id = test_intern( "GREEN" ), .value = REF_TEST_COLOR_GREEN},
        {.name_id = test_intern( "BLUE" ),  .value = REF_TEST_COLOR_BLUE },
        {.name_id = test_intern( "ALPHA" ), .value = REF_TEST_COLOR_ALPHA},
    };

    uint16_t tid = ref_register_enum( &type, entries, 4 );
    ref_finalize_frame( game );

    ref_print_type( tid );

    /* Lookup by name */
    const ref_enum_t* by_name = ref_enum_find_by_name( tid, "BLUE" );
    printf( "  find_by_name(\"BLUE\")  -> value=%lld\n", by_name ? ( long long )by_name->value : -1 );

    /* Lookup by value */
    const ref_enum_t* by_value = ref_enum_find_by_value( tid, 0xFF );
    printf( "  find_by_value(0xFF)   -> name=%s\n", by_value ? test_cstr( by_value->name_id ) : "<none>" );

    /* Negative cases */
    if ( ref_enum_find_by_name( tid, "PURPLE" ) == NULL )
        printf( "  find_by_name(\"PURPLE\") correctly returned NULL\n" );

    /* Hot-reload survival: pop, repush, expect same schema_hash. */
    uint32_t schema_before = ref_get_type( tid )->schema_hash;
    ref_pop_frame( game );

    game = ref_push_frame( "game" );
    tid  = ref_register_enum( &type, entries, 4 );
    ref_finalize_frame( game );

    uint32_t schema_after = ref_get_type( tid )->schema_hash;
    printf( "  schema_hash before=0x%08x after=0x%08x %s\n", schema_before, schema_after,
            ( schema_before == schema_after ) ? "(stable)" : "(MISMATCH)" );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Function-signature test

      Reflect:
          typedef void (*on_die_fn)( int32_t reason, vec3_t* loc );
          struct npc_t { float health; on_die_fn on_die; };
==============================================================================================*/

typedef struct ref_test_npc_s
{
    float health;
    void ( *on_die )( int32_t reason, ref_test_vec3_t* loc );
} ref_test_npc_t;

static void
test_function_sigs( void )
{
    /* Registers a function signature type "on_die_fn" and a struct that embeds it as a
       callback field (REF_MODS_FUNCTION, aux=sig_id). Verifies that ref_function_get_return
       and ref_function_get_param correctly navigate the field[0]=return / field[1..]=params
       layout, and that the inspector can print the struct with the callback field. */
    printf( "\n=== rs: function signatures ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    /* vec3_t is needed by the signature's second parameter. */
    ref_test_register_vec3();

    /* --- Register the signature type "on_die_fn". --- */
    const uint32_t h_sig    = ref_hash_str( "on_die_fn" );
    const uint32_t h_void   = ref_hash_str( "void" );
    const uint32_t h_int32  = ref_hash_str( "int32_t" );
    const uint32_t h_vec3   = ref_hash_str( "vec3_t" );

    ref_type_t      sig_type = { 0 };
    sig_type.name_id        = test_intern( "on_die_fn" );
    sig_type.name_hash      = h_sig;
    sig_type.size           = ( uint16_t )sizeof( void* ); /* a pointer holds the callable */
    sig_type.align          = ( uint8_t )_Alignof( void* );

    /* index 0 = return, indices 1..N = params */
    ref_field_t sig_fields[ 3 ] = { 0 };

    sig_fields[ 0 ].name_id    = test_intern( "return" );
    sig_fields[ 0 ].type_hash  = h_void;
    /* return: void  -> no mods, no aux */

    sig_fields[ 1 ].name_id   = test_intern( "reason" );
    sig_fields[ 1 ].type_hash = h_int32;
    sig_fields[ 1 ].size      = ( uint16_t )sizeof( int32_t );
    /* int32_t value */

    sig_fields[ 2 ].name_id   = test_intern( "loc" );
    sig_fields[ 2 ].type_hash = h_vec3;
    sig_fields[ 2 ].size      = ( uint16_t )sizeof( ref_test_vec3_t* );
    sig_fields[ 2 ].mods      = REF_MODS_PTR;

    uint16_t sig_id           = ref_register_function( &sig_type, sig_fields, 3 );

    /* --- Register a struct that contains an on_die callback field. --- */
    const uint32_t h_npc       = ref_hash_str( "npc_t" );

    ref_type_t      npc_type    = { 0 };
    npc_type.name_id           = test_intern( "npc_t" );
    npc_type.name_hash         = h_npc;
    npc_type.size              = REF_SIZEOF( ref_test_npc_t );
    npc_type.align             = REF_ALIGNOF( ref_test_npc_t );
    npc_type.kind              = REF_KIND_STRUCT;

    ref_field_t npc_fields[ 2 ] = { 0 };

    npc_fields[ 0 ].name_id    = test_intern( "health" );
    npc_fields[ 0 ].type_hash  = ref_hash_str( "float" );
    npc_fields[ 0 ].offset     = REF_OFFSETOF( ref_test_npc_t, health );
    npc_fields[ 0 ].size       = REF_FIELD_SIZE( ref_test_npc_t, health );

    /* on_die: void(*)(int32_t, vec3_t*)
         base = void, mods=[FUNCTION], aux=sig_id  */
    npc_fields[ 1 ].name_id   = test_intern( "on_die" );
    npc_fields[ 1 ].type_hash = h_void;
    npc_fields[ 1 ].offset    = REF_OFFSETOF( ref_test_npc_t, on_die );
    npc_fields[ 1 ].size      = REF_FIELD_SIZE( ref_test_npc_t, on_die );
    npc_fields[ 1 ].mods      = REF_MODS_FUNCTION;
    npc_fields[ 1 ].aux       = sig_id;

    uint16_t npc_id           = ref_register_type( &npc_type, npc_fields, 2 );

    ref_finalize_frame( game );

    printf( "\n-- signature --\n" );
    ref_print_type( sig_id );

    printf( "\n-- struct with callback --\n" );
    ref_print_type( npc_id );

    /* Sanity checks via the convenience accessors. */
    const ref_field_t* ret = ref_function_get_return( sig_id );
    printf( "  return resolves to type_id=%u  (expect void = %u)\n", ret ? (unsigned)ret->type_id : 0xFFFFu, (unsigned)REF_PRIM_VOID );
    printf( "  param_count = %u\n", ref_function_param_count( sig_id ) );
    const ref_field_t* p1 = ref_function_get_param( sig_id, 1 );
    printf( "  param[1] '%s' base_type_id=%u (expect vec3_t)\n", p1 ? test_cstr( p1->name_id ) : "?",
            p1 ? (unsigned)p1->type_id : 0xFFFFu );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Serialization round-trip test

      Reflect a struct that exercises all the redaction rules:
        - primitives, nested struct, inline char array        -> preserved
        - pointer field                                       -> zeroed on save
        - field marked @transient                             -> zeroed on save
      Then write, scribble the destination, read back, and verify byte-for-byte equality
      with the expected post-redaction state.
==============================================================================================*/

typedef struct ref_test_save_s
{
    int32_t         id;
    float           health;
    ref_test_vec3_t  position;    /* nested struct        */
    char            name[ 16 ];  /* inline array         */
    ref_test_vec3_t* cache_ptr;   /* pointer - redacted   */
    uint32_t        cached_hash; /* @transient - redacted */
} ref_test_save_t;

static void
test_serialize( void )
{
    /* Full round-trip test for ref_write / ref_read. The save_t struct has:
       - primitives and nested struct -> must survive unchanged
       - a pointer field (cache_ptr)   -> must be NULL in the output (pointer redaction)
       - a @transient field             -> must be 0 in the output (transient redaction)
       Also verifies schema-hash mismatch rejection and truncated-buffer rejection. */
    printf( "\n=== rs: serialization round-trip ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    ref_test_register_vec3();

    /* Register save_t */
    const uint32_t h_save  = ref_hash_str( "save_t" );
    const uint32_t h_int32 = ref_hash_str( "int32_t" );
    const uint32_t h_u32   = ref_hash_str( "uint32_t" );
    const uint32_t h_float = ref_hash_str( "float" );
    const uint32_t h_vec3  = ref_hash_str( "vec3_t" );
    const uint32_t h_char  = ref_hash_str( "char" );

    ref_type_t      type    = { 0 };
    type.name_id           = test_intern( "save_t" );
    type.name_hash         = h_save;
    type.size              = REF_SIZEOF( ref_test_save_t );
    type.align             = REF_ALIGNOF( ref_test_save_t );
    type.kind              = REF_KIND_STRUCT;

    ref_field_t fields[ 6 ] = { 0 };

    fields[ 0 ].name_id    = test_intern( "id" );
    fields[ 0 ].type_hash  = h_int32;
    fields[ 0 ].offset     = REF_OFFSETOF( ref_test_save_t, id );
    fields[ 0 ].size       = REF_FIELD_SIZE( ref_test_save_t, id );

    fields[ 1 ].name_id    = test_intern( "health" );
    fields[ 1 ].type_hash  = h_float;
    fields[ 1 ].offset     = REF_OFFSETOF( ref_test_save_t, health );
    fields[ 1 ].size       = REF_FIELD_SIZE( ref_test_save_t, health );

    fields[ 2 ].name_id    = test_intern( "position" );
    fields[ 2 ].type_hash  = h_vec3;
    fields[ 2 ].offset     = REF_OFFSETOF( ref_test_save_t, position );
    fields[ 2 ].size       = REF_FIELD_SIZE( ref_test_save_t, position );

    fields[ 3 ].name_id    = test_intern( "name" );
    fields[ 3 ].type_hash  = h_char;
    fields[ 3 ].offset     = REF_OFFSETOF( ref_test_save_t, name );
    fields[ 3 ].size       = REF_FIELD_SIZE( ref_test_save_t, name );
    fields[ 3 ].mods       = REF_MODS_ARRAY;
    fields[ 3 ].aux        = 16;

    fields[ 4 ].name_id    = test_intern( "cache_ptr" );
    fields[ 4 ].type_hash  = h_vec3;
    fields[ 4 ].offset     = REF_OFFSETOF( ref_test_save_t, cache_ptr );
    fields[ 4 ].size       = REF_FIELD_SIZE( ref_test_save_t, cache_ptr );
    fields[ 4 ].mods       = REF_MODS_PTR;

    /* cached_hash is runtime-only computed state: flag it REF_FF_TRANSIENT so the serializer
       zeroes it on save. The flag is the single source of truth (no @transient attribute). */
    fields[ 5 ].name_id    = test_intern( "cached_hash" );
    fields[ 5 ].type_hash  = h_u32;
    fields[ 5 ].offset     = REF_OFFSETOF( ref_test_save_t, cached_hash );
    fields[ 5 ].size       = REF_FIELD_SIZE( ref_test_save_t, cached_hash );
    fields[ 5 ].flags      = REF_FF_TRANSIENT;

    uint16_t tid           = ref_register_type( &type, fields, 6 );

    ref_finalize_frame( game );

    /* --- Source instance with distinctive values --- */
    ref_test_vec3_t dummy = { 9, 9, 9 };
    ref_test_save_t src   = { 0 };
    src.id               = 42;
    src.health           = 75.5f;
    src.position         = ( ref_test_vec3_t ){ 1.0f, 2.0f, 3.0f };
    memcpy( src.name, "hello", 6 );
    src.cache_ptr   = &dummy;     /* should be zeroed on save */
    src.cached_hash = 0xCAFEBABE; /* @transient - should be zeroed on save */

    /* --- Write --- */
    uint8_t buf[ 256 ];
    size_t  written = ref_write( &src, tid, buf, sizeof( buf ) );
    printf( "  wrote %zu bytes (header=%d + body=%u)\n", written, REF_SAVE_HEADER_SIZE,
            ( unsigned )sizeof( ref_test_save_t ) );
    assert( written == ( size_t )REF_SAVE_HEADER_SIZE + sizeof( ref_test_save_t ) );

    /* peek */
    uint32_t peeked = ref_peek_type_hash( buf, written );
    printf( "  peek_type_hash    = 0x%08x  (expect 0x%08x)\n", peeked, h_save );
    assert( peeked == h_save );

    /* --- Read back into a deliberately-trashed destination --- */
    ref_test_save_t dst;
    memset( &dst, 0xAA, sizeof( dst ) );

    size_t         consumed = 0;
    ref_io_status_t st       = ref_read( &dst, tid, buf, written, &consumed );
    printf( "  read status=%d consumed=%zu\n", ( int )st, consumed );
    assert( st == REF_IO_OK );
    assert( consumed == written );

    /* --- Verify --- */
    assert( dst.id == 42 );
    assert( dst.health == 75.5f );
    assert( dst.position.x == 1.0f && dst.position.y == 2.0f && dst.position.z == 3.0f );
    assert( memcmp( dst.name, "hello", 6 ) == 0 );
    assert( dst.cache_ptr == NULL ); /* pointer redacted */
    assert( dst.cached_hash == 0 );  /* transient redacted */
    printf( "  round-trip OK; pointer and @transient cleared as expected\n" );

    /* --- Schema-hash mismatch: corrupt the header and expect refusal --- */
    uint8_t corrupt[ 256 ];
    memcpy( corrupt, buf, written );
    corrupt[ 8 ] ^= 0x01; /* flip a bit in schema_hash */
    st = ref_read( &dst, tid, corrupt, written, NULL );
    printf( "  corrupt schema -> status=%d (expect %d)\n", ( int )st, REF_IO_INCOMPAT );
    assert( st == REF_IO_INCOMPAT );

    /* --- Truncated buffer --- */
    st = ref_read( &dst, tid, buf, 10, NULL );
    assert( st == REF_IO_TRUNCATED );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Bitset enum test
==============================================================================================*/

typedef enum ref_test_perm_e
{
    REF_TEST_PERM_NONE  = 0,
    REF_TEST_PERM_READ  = 1 << 0,
    REF_TEST_PERM_WRITE = 1 << 1,
    REF_TEST_PERM_EXEC  = 1 << 2,
    REF_TEST_PERM_ALL   = REF_TEST_PERM_READ | REF_TEST_PERM_WRITE | REF_TEST_PERM_EXEC,
} ref_test_perm_t;

static void
test_bitset( void )
{
    /* Tests the greedy bit-claim decoding. The "ALL" entry is registered before its
       constituent bits (READ, WRITE, EXEC) so that ref_bitset_describe(ALL) returns "ALL"
       rather than "READ | WRITE | EXEC". Swapping registration order would change the output.
       Also verifies zero-value display, unknown-bits hex tail, and ref_bitset_find_flag. */
    printf( "\n=== rs: bitset enum ===\n" );

    ref_init();
    uint16_t  game = ref_push_frame( "game" );

    ref_type_t type = { 0 };
    type.name_id   = test_intern( "perm_t" );
    type.name_hash = ref_hash_str( "perm_t" );
    type.size      = ( uint16_t )sizeof( ref_test_perm_t );
    type.align     = ( uint8_t )_Alignof( ref_test_perm_t );

    /* Order matters when decoding: place multi-bit masks (ALL) BEFORE the single-bit
       components so a value of 0b111 prints as "ALL" instead of "READ | WRITE | EXEC".
       Try swapping the order to see the alternative formatting. */
    ref_enum_t entries[ 5 ] = {
        {.name_id = test_intern( "NONE" ),  .value = REF_TEST_PERM_NONE },
        {.name_id = test_intern( "ALL" ),   .value = REF_TEST_PERM_ALL  },
        {.name_id = test_intern( "READ" ),  .value = REF_TEST_PERM_READ },
        {.name_id = test_intern( "WRITE" ), .value = REF_TEST_PERM_WRITE},
        {.name_id = test_intern( "EXEC" ),  .value = REF_TEST_PERM_EXEC },
    };

    uint16_t tid = ref_register_bitset( &type, entries, 5 );
    ref_finalize_frame( game );

    printf( "  is_bitset(perm_t) = %s\n", ref_enum_is_bitset( tid ) ? "true" : "false" );
    assert( ref_enum_is_bitset( tid ) );

    char buf[ 64 ];

    /* 0 -> "NONE" via the zero-valued enumerator. */
    ref_bitset_describe( tid, 0, buf, sizeof( buf ) );
    printf( "  describe(0)               = \"%s\"\n", buf );

    /* READ|WRITE -> "READ | WRITE" */
    ref_bitset_describe( tid, REF_TEST_PERM_READ | REF_TEST_PERM_WRITE, buf, sizeof( buf ) );
    printf( "  describe(READ|WRITE)      = \"%s\"\n", buf );

    /* All bits -> "ALL" because the multi-bit mask comes first in registration. */
    ref_bitset_describe( tid, REF_TEST_PERM_ALL, buf, sizeof( buf ) );
    printf( "  describe(ALL)             = \"%s\"\n", buf );

    /* Unknown bits -> tail hex */
    ref_bitset_describe( tid, REF_TEST_PERM_READ | 0x100, buf, sizeof( buf ) );
    printf( "  describe(READ|0x100)      = \"%s\"\n", buf );

    /* find_flag(WRITE) */
    const ref_enum_t* w = ref_bitset_find_flag( tid, REF_TEST_PERM_WRITE );
    printf( "  find_flag(WRITE)          = %s\n", w ? test_cstr( w->name_id ) : "?" );
    assert( w && w->value == REF_TEST_PERM_WRITE );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Reference walker test

      Reflect:
          struct inner_t { vec3_t* p; };
          struct walk_t  { int32_t id; vec3_t* single; vec3_t* slots[3]; inner_t nested; };

      Expected visits: 1 (single) + 3 (slots) + 1 (nested.p) = 5
==============================================================================================*/

typedef struct ref_test_inner_s
{
    ref_test_vec3_t* p;
} ref_test_inner_t;

typedef struct ref_test_walk_s
{
    int32_t         id;
    ref_test_vec3_t* single;
    ref_test_vec3_t* slots[ 3 ];
    ref_test_inner_t nested;
} ref_test_walk_t;

typedef struct
{
    int      count;
    uint16_t expected_pointee_id;
} ref_test_walk_ctx_t;

static void
ref_test_walk_visitor( void** slot, uint16_t pointee_id, const ref_field_t* f, void* user )
{
    ref_test_walk_ctx_t* ctx = ( ref_test_walk_ctx_t* )user;
    ctx->count++;

    const ref_type_t* pointee = ref_get_type( pointee_id );
    printf( "  visit field '%-12s' slot=%p -> %p  pointee=%s\n", test_cstr( f->name_id ), ( void* )slot,
            *slot, pointee ? test_cstr( pointee->name_id ) : "?" );

    /* Loose sanity check: every visit in this test should point at a vec3_t. */
    assert( pointee_id == ctx->expected_pointee_id );
}

static void
test_walker( void )
{
    /* Tests ref_walk_refs() traversal across a struct hierarchy:
       walk_t { id, single*, slots*[3], nested{p*} }
       Expected visits: 1 (single) + 3 (slots[0..2]) + 1 (nested.p) = 5.
       slots[2] is NULL, but the walker still visits the slot -- visiting a null pointer
       slot is correct behaviour since the callback decides what to do with it. */
    printf( "\n=== rs: reference walker ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    ref_test_register_vec3();

    /* Register ref_test_inner_t { vec3_t* p; } */
    {
        const uint32_t h_inner = ref_hash_str( "inner_t" );
        const uint32_t h_vec3  = ref_hash_str( "vec3_t" );

        ref_type_t      type    = { 0 };
        type.name_id           = test_intern( "inner_t" );
        type.name_hash         = h_inner;
        type.size              = REF_SIZEOF( ref_test_inner_t );
        type.align             = REF_ALIGNOF( ref_test_inner_t );
        type.kind              = REF_KIND_STRUCT;

        ref_field_t fields[ 1 ] = { 0 };

        fields[ 0 ].name_id    = test_intern( "p" );
        fields[ 0 ].type_hash  = h_vec3;
        fields[ 0 ].offset     = REF_OFFSETOF( ref_test_inner_t, p );
        fields[ 0 ].size       = REF_FIELD_SIZE( ref_test_inner_t, p );
        fields[ 0 ].mods       = REF_MODS_PTR;

        ref_register_type( &type, fields, 1 );
    }

    /* Register ref_test_walk_t */
    uint16_t walk_tid;
    {
        const uint32_t h_walk  = ref_hash_str( "walk_t" );
        const uint32_t h_int32 = ref_hash_str( "int32_t" );
        const uint32_t h_vec3  = ref_hash_str( "vec3_t" );
        const uint32_t h_inner = ref_hash_str( "inner_t" );

        ref_type_t      type    = { 0 };
        type.name_id           = test_intern( "walk_t" );
        type.name_hash         = h_walk;
        type.size              = REF_SIZEOF( ref_test_walk_t );
        type.align             = REF_ALIGNOF( ref_test_walk_t );
        type.kind              = REF_KIND_STRUCT;

        ref_field_t fields[ 4 ] = { 0 };

        fields[ 0 ].name_id    = test_intern( "id" );
        fields[ 0 ].type_hash  = h_int32;
        fields[ 0 ].offset     = REF_OFFSETOF( ref_test_walk_t, id );
        fields[ 0 ].size       = REF_FIELD_SIZE( ref_test_walk_t, id );

        fields[ 1 ].name_id    = test_intern( "single" );
        fields[ 1 ].type_hash  = h_vec3;
        fields[ 1 ].offset     = REF_OFFSETOF( ref_test_walk_t, single );
        fields[ 1 ].size       = REF_FIELD_SIZE( ref_test_walk_t, single );
        fields[ 1 ].mods       = REF_MODS_PTR;

        fields[ 2 ].name_id    = test_intern( "slots" );
        fields[ 2 ].type_hash  = h_vec3;
        fields[ 2 ].offset     = REF_OFFSETOF( ref_test_walk_t, slots );
        fields[ 2 ].size       = REF_FIELD_SIZE( ref_test_walk_t, slots );
        fields[ 2 ].mods       = REF_MODS_PTR_ARRAY;
        fields[ 2 ].aux        = 3;

        fields[ 3 ].name_id    = test_intern( "nested" );
        fields[ 3 ].type_hash  = h_inner;
        fields[ 3 ].offset     = REF_OFFSETOF( ref_test_walk_t, nested );
        fields[ 3 ].size       = REF_FIELD_SIZE( ref_test_walk_t, nested );

        walk_tid               = ref_register_type( &type, fields, 4 );
    }

    ref_finalize_frame( game );

    /* Build an instance with distinctive dummy pointers so the visitor output is readable. */
    ref_test_vec3_t v1   = { 1.0f, 0.0f, 0.0f };
    ref_test_vec3_t v2   = { 0.0f, 2.0f, 0.0f };
    ref_test_vec3_t v3   = { 0.0f, 0.0f, 3.0f };
    ref_test_vec3_t v4   = { 9.0f, 9.0f, 9.0f };
    ref_test_vec3_t v5   = { 7.0f, 7.0f, 7.0f };

    ref_test_walk_t inst = { 0 };
    inst.id             = 42;
    inst.single         = &v1;
    inst.slots[ 0 ]     = &v2;
    inst.slots[ 1 ]     = &v3;
    inst.slots[ 2 ]     = NULL; /* walker still visits null slots */
    inst.nested.p       = &v4;
    ( void )v5;

    ref_test_walk_ctx_t ctx = { 0, ref_find_type_by_name( "vec3_t" ) };
    ref_walk_refs( &inst, walk_tid, ref_test_walk_visitor, &ctx );

    printf( "  total visits = %d (expected 5)\n", ctx.count );
    assert( ctx.count == 5 );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Usage Example: Generic Editor Inspector

    Scenario: an editor panel receives a (type_name, instance_ptr) pair and must enumerate
    all inspectable fields, print their live values, respect visibility/mutability flags,
    and surface any editor metadata (range clamps, display hints).

    This is the realistic "day one" use case for the reflection library. Read the friction
    comments to see where the API makes the user reach below the abstraction boundary.

    Types registered here:
        stance_t  -- enum (IDLE / RUNNING / CROUCHED)
        player_t  -- struct with mixed field flags and @range attrs on float fields

    The inspector callback ref_usage_inspect_field demonstrates the canonical dispatch
    pattern: check f->flags for REF_FF_HIDDEN early, then switch on f->kind for the
    value type, using f->type_id as the prim or enum id when kind is known. f->kind is
    cached at finalize time so this dispatch needs no secondary ref_get_type() call.
==============================================================================================*/

typedef enum ref_test_stance_e
{
    REF_TEST_STANCE_IDLE     = 0,
    REF_TEST_STANCE_RUNNING  = 1,
    REF_TEST_STANCE_CROUCHED = 2,
} ref_test_stance_t;

typedef struct ref_test_player_s
{
    int32_t          id;
    char             name[ 32 ];
    float            health;
    float            speed;
    ref_test_stance_t stance;
    uint32_t         internal_cookie;  /* REF_FF_HIDDEN -- runtime only, never inspected */
    int32_t          kill_count;       /* REF_FF_READONLY -- visible but not editable */
} ref_test_player_t;

typedef struct
{
    const void* instance;
} ref_usage_ctx_t;

static void
ref_usage_inspect_field( uint16_t field_id, const ref_field_t* f, void* user )
{
    const ref_usage_ctx_t* ctx  = (const ref_usage_ctx_t*)user;
    const void*           addr = (const char*)ctx->instance + f->offset;

    /* Hidden fields are runtime-only; the inspector never shows them. */
    if ( f->flags & REF_FF_HIDDEN ) return;

    const char* label    = ref_cstr( f->name_id );
    bool        readonly = ( f->flags & REF_FF_READONLY ) != 0;

    printf( "  %s %-16s : ", readonly ? "[RO]" : "    ", label );

    /* f->kind is the cached kind of the resolved base type -- no extra lookup needed. */
    if ( f->mods == REF_MODS_ARRAY && f->kind == REF_KIND_PRIM && f->type_id == REF_PRIM_CHAR )
    {
        /* char[N] inline string */
        printf( "\"%.*s\"", (int)f->aux, (const char*)addr );
    }
    else if ( f->mods == REF_MODS_VALUE )
    {
        switch ( (ref_kind_t)f->kind )
        {
            case REF_KIND_PRIM:
            {
                /* FRICTION: no typed read helpers; offset cast is the only path. */
                switch ( (ref_prim_t)f->type_id )
                {
                    case REF_PRIM_I32: printf( "%d", *(const int32_t*)addr );  break;
                    case REF_PRIM_U32: printf( "%u", *(const uint32_t*)addr ); break;
                    case REF_PRIM_F32:
                    {
                        float v = *(const float*)addr;
                        printf( "%.2f", v );

                        /* Read @range as a multi-value group. ref_field_get_attr_values
                           returns how many consecutive entries share the name and points at
                           the first, so min/max are range[0]/range[1] -- no pointer math. */
                        const ref_attrib_t* range;
                        if ( ref_field_get_attr_values( field_id, REF_ANAME_RANGE, &range ) >= 2 )
                            printf( "  [range %.0f..%.0f]", range[ 0 ].value.f32, range[ 1 ].value.f32 );
                        break;
                    }
                    default: printf( "<prim id=%u>", f->type_id ); break;
                }
                break;
            }
            case REF_KIND_ENUM:
            {
                /* Print the enumerator name instead of the raw integer. */
                int32_t           v     = *(const int32_t*)addr;
                const ref_enum_t*  entry = ref_enum_find_by_value( f->type_id, v );
                if ( entry ) printf( "%s",              ref_cstr( entry->name_id ) );
                else         printf( "<unknown %d>", v );
                break;
            }
            default:
            {
                /* Nested struct or unknown -- print type name as placeholder. */
                const ref_type_t* bt = ref_get_type( f->type_id );
                printf( "{%s}", bt ? ref_cstr( bt->name_id ) : "?" );
                break;
            }
        }
    }
    else if ( f->mods & REF_MODS_PTR )
    {
        const void* ptr = *(const void* const*)addr;
        printf( "%p", ptr );
    }
    else
    {
        printf( "<mods=0x%04x>", f->mods );
    }

    printf( "\n" );
}

/*==============================================================================================
    - Find a type by name (simulating a generic system that only has a string)
    - Read REF_TF_* type flags for fast-path editor/serialization checks
    - Iterate all fields via ref_each_field with a callback
    - Offset-cast to read live field values (*(float*)((char*)instance + f->offset))
    - Use f->kind for dispatch without a secondary ref_get_type lookup
    - Render an enum field as its name string via ref_enum_find_by_value
    - Print @range min/max for float fields
    - Respect REF_FF_HIDDEN, REF_FF_READONLY flags

    1. Multi-value attribute access � ref_field_get_attr returns only the first matching entry. 
       Getting @range max requires rmin + 1 pointer arithmetic; there is no ref_each_field_attr iterator.

    2. field_id from field pointer � ref_find_field returns const ref_field_t*, but 
       ref_field_add_attr takes a uint16_t field_id. Bridging them requires 
       (field - ref_get_field(0)) pointer subtraction. There's no 
       ref_get_field_id(const ref_field_t*) helper.

==============================================================================================*/

static void
test_usage_example( void )
{
    printf( "\n=== rs: usage example -- generic editor inspector ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    /* Register stance_t enum. */
    const uint32_t h_stance    = ref_hash_str( "stance_t" );
    ref_type_t     stance_type = { 0 };
    stance_type.name_id        = test_intern( "stance_t" );
    stance_type.name_hash      = h_stance;
    stance_type.size           = REF_SIZEOF( ref_test_stance_t );
    stance_type.align          = REF_ALIGNOF( ref_test_stance_t );

    ref_enum_t stances[ 3 ] = {
        {.name_id = test_intern( "IDLE" ),     .value = REF_TEST_STANCE_IDLE     },
        {.name_id = test_intern( "RUNNING" ),  .value = REF_TEST_STANCE_RUNNING  },
        {.name_id = test_intern( "CROUCHED" ), .value = REF_TEST_STANCE_CROUCHED },
    };
    ref_register_enum( &stance_type, stances, 3 );

    /* Register player_t struct with per-field flags. */
    const uint32_t h_player = ref_hash_str( "player_t" );
    const uint32_t h_int32  = ref_hash_str( "int32_t" );
    const uint32_t h_char   = ref_hash_str( "char" );
    const uint32_t h_float  = ref_hash_str( "float" );
    const uint32_t h_u32    = ref_hash_str( "uint32_t" );

    ref_type_t player_type = { 0 };
    player_type.name_id   = test_intern( "player_t" );
    player_type.name_hash = h_player;
    player_type.size      = REF_SIZEOF( ref_test_player_t );
    player_type.align     = REF_ALIGNOF( ref_test_player_t );
    player_type.kind      = REF_KIND_STRUCT;
    player_type.flags     = REF_TF_EDITOR | REF_TF_SERIALIZE;

    ref_field_t fields[ 7 ] = { 0 };

    fields[ 0 ].name_id   = test_intern( "id" );
    fields[ 0 ].type_hash = h_int32;
    fields[ 0 ].offset    = REF_OFFSETOF( ref_test_player_t, id );
    fields[ 0 ].size      = REF_FIELD_SIZE( ref_test_player_t, id );
    fields[ 0 ].flags     = REF_FF_READONLY;   /* assigned once; never changed via editor */

    fields[ 1 ].name_id   = test_intern( "name" );
    fields[ 1 ].type_hash = h_char;
    fields[ 1 ].offset    = REF_OFFSETOF( ref_test_player_t, name );
    fields[ 1 ].size      = REF_FIELD_SIZE( ref_test_player_t, name );
    fields[ 1 ].mods      = REF_MODS_ARRAY;
    fields[ 1 ].aux       = 32;

    fields[ 2 ].name_id   = test_intern( "health" );
    fields[ 2 ].type_hash = h_float;
    fields[ 2 ].offset    = REF_OFFSETOF( ref_test_player_t, health );
    fields[ 2 ].size      = REF_FIELD_SIZE( ref_test_player_t, health );

    fields[ 3 ].name_id   = test_intern( "speed" );
    fields[ 3 ].type_hash = h_float;
    fields[ 3 ].offset    = REF_OFFSETOF( ref_test_player_t, speed );
    fields[ 3 ].size      = REF_FIELD_SIZE( ref_test_player_t, speed );

    fields[ 4 ].name_id   = test_intern( "stance" );
    fields[ 4 ].type_hash = h_stance;
    fields[ 4 ].offset    = REF_OFFSETOF( ref_test_player_t, stance );
    fields[ 4 ].size      = REF_FIELD_SIZE( ref_test_player_t, stance );

    fields[ 5 ].name_id   = test_intern( "internal_cookie" );
    fields[ 5 ].type_hash = h_u32;
    fields[ 5 ].offset    = REF_OFFSETOF( ref_test_player_t, internal_cookie );
    fields[ 5 ].size      = REF_FIELD_SIZE( ref_test_player_t, internal_cookie );
    fields[ 5 ].flags     = REF_FF_HIDDEN | REF_FF_TRANSIENT;   /* runtime-only; never serialized */

    fields[ 6 ].name_id   = test_intern( "kill_count" );
    fields[ 6 ].type_hash = h_int32;
    fields[ 6 ].offset    = REF_OFFSETOF( ref_test_player_t, kill_count );
    fields[ 6 ].size      = REF_FIELD_SIZE( ref_test_player_t, kill_count );
    fields[ 6 ].flags     = REF_FF_READONLY;   /* stat: displayed, not editable */

    uint16_t player_tid = ref_register_type( &player_type, fields, 7 );

    /* Attach @range(0, 100) to health and @range(0, 20) to speed.
       FRICTION: ref_find_field returns a const ref_field_t*; ref_field_add_attr requires a
       field_id (uint16_t).  Bridging the two requires pointer subtraction from the base
       of the field table -- there is no ref_get_field_id(const ref_field_t*) helper. */
    {
        const ref_field_t* hf  = ref_find_field( player_tid, "health" );
        uint16_t          hid = (uint16_t)( hf - ref_get_field( 0 ) );

        ref_attrib_t a = { 0 };
        a.name_id     = test_intern( REF_ANAME_RANGE );
        a.type        = REF_ATTR_FLOAT;
        a.flags       = REF_AF_CLAMP;
        a.value.f32   = 0.0f;
        ref_field_add_attr( hid, &a );
        a.value.f32   = 100.0f;
        ref_field_add_attr( hid, &a );
    }
    {
        const ref_field_t* sf  = ref_find_field( player_tid, "speed" );
        uint16_t          sid = (uint16_t)( sf - ref_get_field( 0 ) );

        ref_attrib_t a = { 0 };
        a.name_id     = test_intern( REF_ANAME_RANGE );
        a.type        = REF_ATTR_FLOAT;
        a.flags       = REF_AF_CLAMP;
        a.value.f32   = 0.0f;
        ref_field_add_attr( sid, &a );
        a.value.f32   = 20.0f;
        ref_field_add_attr( sid, &a );
    }

    ref_finalize_frame( game );

    /* Build a live instance with distinctive values. */
    ref_test_player_t player = { 0 };
    player.id               = 7;
    memcpy( player.name, "Aragorn", 8 );
    player.health           = 82.5f;
    player.speed            = 6.3f;
    player.stance           = REF_TEST_STANCE_RUNNING;
    player.internal_cookie  = 0xDEADBEEF;   /* hidden -- inspector must not show this */
    player.kill_count       = 42;

    /* Simulate a generic system that only knows the type name, not the C type. */
    uint16_t         tid  = ref_find_type_by_name( "player_t" );
    const ref_type_t* t   = ref_get_type( tid );
    assert( tid != REF_TYPE_INVALID && t );
    
    /* Type-level flags are a fast-path alternative to attribute lookup for common semantics. */
    printf( "Inspecting '%s'  size=%u  flags:%s%s\n", ref_cstr( t->name_id ), t->size,
            ( t->flags & REF_TF_EDITOR    ) ? " EDITOR"    : "",
            ( t->flags & REF_TF_SERIALIZE ) ? " SERIALIZE" : "" );

    ref_usage_ctx_t ctx = { .instance = &player };
    ref_each_field( tid, ref_usage_inspect_field, &ctx );

    /* Verify internal_cookie was silently skipped (its sentinel 0xDEADBEEF never printed). */
    printf( "  (internal_cookie[0xDEADBEEF] hidden -- not shown above)\n" );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Attribute test

      Register a struct with one field per attribute type and flag combination:
        volume      -- @range(0,100)        REF_ATTR_INT,    REF_AF_CLAMP,        2-value group
        brightness  -- @clamp_ui(0.0,1.0)  REF_ATTR_FLOAT,  REF_AF_CLAMP_UI,     2-value group
        fullscreen  -- @display_name(...)   REF_ATTR_STRING, REF_AF_DISPLAY_NAME, single value
        username    -- @tooltip(...)        REF_ATTR_STRING, REF_AF_TOOLTIP,       single value

      Also attaches a type-level @version INT attribute to exercise ref_type_add_attr and
      ref_type_get_attr.  Missing-attr lookup is verified to return NULL.

      Multi-value groups are consecutive same-name entries; ref_field_get_attr_values returns
      the group size and points at the first, so the values are read as out[0], out[1], ...
==============================================================================================*/

typedef struct ref_test_cfg_s
{
    int32_t  volume;
    float    brightness;
    bool     fullscreen;
    char     username[ 32 ];

} ref_test_cfg_t;

static void
test_attrs_full( void )
{
    printf( "\n=== ref: attributes full ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    /* --- Register cfg_t --- */
    ref_type_t type = { 0 };
    type.name_id   = test_intern( "cfg_t" );
    type.name_hash = ref_hash_str( "cfg_t" );
    type.size      = REF_SIZEOF( ref_test_cfg_t );
    type.align     = REF_ALIGNOF( ref_test_cfg_t );
    type.kind      = REF_KIND_STRUCT;

    ref_field_t fields[ 4 ] = { 0 };

    fields[ 0 ].name_id   = test_intern( "volume" );
    fields[ 0 ].type_hash = ref_hash_str( "int32_t" );
    fields[ 0 ].offset    = REF_OFFSETOF( ref_test_cfg_t, volume );
    fields[ 0 ].size      = REF_FIELD_SIZE( ref_test_cfg_t, volume );

    fields[ 1 ].name_id   = test_intern( "brightness" );
    fields[ 1 ].type_hash = ref_hash_str( "float" );
    fields[ 1 ].offset    = REF_OFFSETOF( ref_test_cfg_t, brightness );
    fields[ 1 ].size      = REF_FIELD_SIZE( ref_test_cfg_t, brightness );

    fields[ 2 ].name_id   = test_intern( "fullscreen" );
    fields[ 2 ].type_hash = ref_hash_str( "bool" );
    fields[ 2 ].offset    = REF_OFFSETOF( ref_test_cfg_t, fullscreen );
    fields[ 2 ].size      = REF_FIELD_SIZE( ref_test_cfg_t, fullscreen );

    fields[ 3 ].name_id   = test_intern( "username" );
    fields[ 3 ].type_hash = ref_hash_str( "char" );
    fields[ 3 ].offset    = REF_OFFSETOF( ref_test_cfg_t, username );
    fields[ 3 ].size      = REF_FIELD_SIZE( ref_test_cfg_t, username );
    fields[ 3 ].mods      = REF_MODS_ARRAY;
    fields[ 3 ].aux       = 32;

    uint16_t tid = ref_register_type( &type, fields, 4 );

    /* --- Type-level: @version = 3 (REF_ATTR_INT, single) ---
       Must be added before any field attrs to satisfy the contiguous-pool ordering rule. */
    {
        ref_attrib_t a = { 0 };
        a.name_id      = test_intern( "version" );
        a.type         = REF_ATTR_INT;
        a.flags        = 0;
        a.value.i32    = 3;
        ref_type_add_attr( tid, &a );
    }

    /* --- Field attrs: all entries for each field before moving to the next --- */

    /* volume: @range(0, 100)  REF_ATTR_INT, REF_AF_CLAMP, 2-entry CI group */
    {
        const ref_field_t* f   = ref_find_field( tid, "volume" );
        uint16_t           fid = (uint16_t)( f - ref_get_field( 0 ) );

        ref_attrib_t a = { 0 };
        a.name_id      = test_intern( REF_ANAME_RANGE );
        a.type         = REF_ATTR_INT;
        a.flags        = REF_AF_CLAMP;
        a.value.i32    = 0;
        ref_field_add_attr( fid, &a );

        a.value.i32    = 100;
        ref_field_add_attr( fid, &a );
    }

    /* brightness: @clamp_ui(0.0, 1.0)  REF_ATTR_FLOAT, REF_AF_CLAMP_UI, 2-entry CI group */
    {
        const ref_field_t* f   = ref_find_field( tid, "brightness" );
        uint16_t           fid = (uint16_t)( f - ref_get_field( 0 ) );

        ref_attrib_t a = { 0 };
        a.name_id      = test_intern( REF_ANAME_CLAMP_UI );
        a.type         = REF_ATTR_FLOAT;
        a.flags        = REF_AF_CLAMP_UI;
        a.value.f32    = 0.0f;
        ref_field_add_attr( fid, &a );

        a.value.f32    = 1.0f;
        ref_field_add_attr( fid, &a );
    }

    /* fullscreen: @display_name("Full Screen")  REF_ATTR_STRING, REF_AF_DISPLAY_NAME, single */
    {
        const ref_field_t* f   = ref_find_field( tid, "fullscreen" );
        uint16_t           fid = (uint16_t)( f - ref_get_field( 0 ) );

        ref_attrib_t a = { 0 };
        a.name_id      = test_intern( REF_ANAME_DISPLAY_NAME );
        a.type         = REF_ATTR_STRING;
        a.flags        = REF_AF_DISPLAY_NAME;
        a.value.str    = test_intern( "Full Screen" );
        ref_field_add_attr( fid, &a );
    }

    /* username: @tooltip("Your display name")  REF_ATTR_STRING, REF_AF_TOOLTIP, single */
    {
        const ref_field_t* f   = ref_find_field( tid, "username" );
        uint16_t           fid = (uint16_t)( f - ref_get_field( 0 ) );

        ref_attrib_t a = { 0 };
        a.name_id      = test_intern( REF_ANAME_TOOLTIP );
        a.type         = REF_ATTR_STRING;
        a.flags        = REF_AF_TOOLTIP;
        a.value.str    = test_intern( "Your display name" );
        ref_field_add_attr( fid, &a );
    }

    ref_finalize_frame( game );
    ref_print_type( tid );

    bool all_ok = true;

    /* --- Verify type-level @version --- */
    {
        const ref_attrib_t* a;
        uint16_t            n  = ref_type_get_attr_values( tid, "version", &a );
        bool                ok = a && n == 1 && a->type == REF_ATTR_INT && a->value.i32 == 3;
        printf( "  [%s] type @version: type=%d val=%d count=%u\n",
                ok ? "ok  " : "FAIL",
                a ? a->type : -1, a ? a->value.i32 : -1, ( unsigned )n );
        if ( !ok ) all_ok = false;
    }

    /* --- Verify volume @range(0,100) INT 2-entry CI group --- */
    {
        const ref_field_t*  f    = ref_find_field( tid, "volume" );
        uint16_t            fid  = (uint16_t)( f - ref_get_field( 0 ) );
        const ref_attrib_t* r;
        uint16_t            n    = ref_field_get_attr_values( fid, REF_ANAME_RANGE, &r );

        bool ok = n == 2
               && r[ 0 ].type == REF_ATTR_INT && r[ 0 ].flags == REF_AF_CLAMP && r[ 0 ].value.i32 == 0
               && r[ 1 ].type == REF_ATTR_INT && r[ 1 ].flags == REF_AF_CLAMP && r[ 1 ].value.i32 == 100;
        printf( "  [%s] volume @range: count=%u min=%d max=%d\n",
                ok ? "ok  " : "FAIL", ( unsigned )n,
                n >= 1 ? r[ 0 ].value.i32 : -1, n >= 2 ? r[ 1 ].value.i32 : -1 );
        if ( !ok ) all_ok = false;
    }

    /* --- Verify brightness @clamp_ui(0.0,1.0) FLOAT 2-entry CI group --- */
    {
        const ref_field_t*  f    = ref_find_field( tid, "brightness" );
        uint16_t            fid  = (uint16_t)( f - ref_get_field( 0 ) );
        const ref_attrib_t* c;
        uint16_t            n    = ref_field_get_attr_values( fid, REF_ANAME_CLAMP_UI, &c );

        bool ok = n == 2
               && c[ 0 ].type == REF_ATTR_FLOAT && c[ 0 ].flags == REF_AF_CLAMP_UI && c[ 0 ].value.f32 == 0.0f
               && c[ 1 ].type == REF_ATTR_FLOAT && c[ 1 ].flags == REF_AF_CLAMP_UI && c[ 1 ].value.f32 == 1.0f;
        printf( "  [%s] brightness @clamp_ui: min=%.1f max=%.1f\n",
                ok ? "ok  " : "FAIL",
                n >= 1 ? c[ 0 ].value.f32 : -1.0f, n >= 2 ? c[ 1 ].value.f32 : -1.0f );
        if ( !ok ) all_ok = false;
    }

    /* --- Verify fullscreen @display_name STRING --- */
    {
        const ref_field_t*  f   = ref_find_field( tid, "fullscreen" );
        uint16_t            fid = (uint16_t)( f - ref_get_field( 0 ) );
        const ref_attrib_t* a;
        uint16_t            n   = ref_field_get_attr_values( fid, REF_ANAME_DISPLAY_NAME, &a );

        bool ok = a && n == 1 && a->type == REF_ATTR_STRING && a->flags == REF_AF_DISPLAY_NAME
               && strcmp( test_cstr( a->value.str ), "Full Screen" ) == 0;
        printf( "  [%s] fullscreen @display_name: \"%s\"\n",
                ok ? "ok  " : "FAIL",
                ( a && a->type == REF_ATTR_STRING ) ? test_cstr( a->value.str ) : "<none>" );
        if ( !ok ) all_ok = false;
    }

    /* --- Verify username @tooltip STRING --- */
    {
        const ref_field_t*  f   = ref_find_field( tid, "username" );
        uint16_t            fid = (uint16_t)( f - ref_get_field( 0 ) );
        const ref_attrib_t* a;
        uint16_t            n   = ref_field_get_attr_values( fid, REF_ANAME_TOOLTIP, &a );

        bool ok = a && n == 1 && a->type == REF_ATTR_STRING && a->flags == REF_AF_TOOLTIP
               && strcmp( test_cstr( a->value.str ), "Your display name" ) == 0;
        printf( "  [%s] username @tooltip: \"%s\"\n",
                ok ? "ok  " : "FAIL",
                ( a && a->type == REF_ATTR_STRING ) ? test_cstr( a->value.str ) : "<none>" );
        if ( !ok ) all_ok = false;
    }

    /* --- Missing attr lookup must return NULL --- */
    {
        const ref_field_t*  f   = ref_find_field( tid, "volume" );
        uint16_t            fid = (uint16_t)( f - ref_get_field( 0 ) );
        const ref_attrib_t* a   = ref_field_get_attr( fid, "nonexistent" );
        bool                ok  = ( a == NULL );
        printf( "  [%s] field_get_attr(\"nonexistent\") == NULL\n", ok ? "ok  " : "FAIL" );
        if ( !ok ) all_ok = false;
    }
    {
        const ref_attrib_t* a  = ref_type_get_attr( tid, "nonexistent" );
        bool                ok = ( a == NULL );
        printf( "  [%s] type_get_attr(\"nonexistent\")  == NULL\n", ok ? "ok  " : "FAIL" );
        if ( !ok ) all_ok = false;
    }

    assert( all_ok );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Field-flags test

      One field per REF_FF_* bit, plus a combo field (HIDDEN|READONLY) and a zero-flags field.
      After finalize every field->flags must equal the value set at registration time.
==============================================================================================*/

typedef struct ref_test_ff_s
{
    int32_t a_transient;
    int32_t a_readonly;
    int32_t a_hidden;
    int32_t a_network;
    int32_t a_combo;   /* REF_FF_HIDDEN | REF_FF_READONLY */
    int32_t a_none;

} ref_test_ff_t;

static void
test_field_flags_full( void )
{
    printf( "\n=== ref: field flags full ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    ref_type_t type = { 0 };
    type.name_id   = test_intern( "ff_t" );
    type.name_hash = ref_hash_str( "ff_t" );
    type.size      = REF_SIZEOF( ref_test_ff_t );
    type.align     = REF_ALIGNOF( ref_test_ff_t );
    type.kind      = REF_KIND_STRUCT;

    const uint32_t h_i32 = ref_hash_str( "int32_t" );

    static const struct { const char* name; size_t offset; uint32_t flags; } SPEC[] = {
        { "a_transient",  REF_OFFSETOF( ref_test_ff_t, a_transient  ), REF_FF_TRANSIENT                },
        { "a_readonly",   REF_OFFSETOF( ref_test_ff_t, a_readonly   ), REF_FF_READONLY                 },
        { "a_hidden",     REF_OFFSETOF( ref_test_ff_t, a_hidden     ), REF_FF_HIDDEN                   },
        { "a_network",    REF_OFFSETOF( ref_test_ff_t, a_network    ), REF_FF_NETWORK                  },
        { "a_combo",      REF_OFFSETOF( ref_test_ff_t, a_combo      ), REF_FF_HIDDEN | REF_FF_READONLY },
        { "a_none",       REF_OFFSETOF( ref_test_ff_t, a_none       ), 0                              },
    };
    enum { NFIELDS = 6 };

    ref_field_t fields[ NFIELDS ];
    memset( fields, 0, sizeof( fields ) );
    for ( int i = 0; i < NFIELDS; i++ )
    {
        fields[ i ].name_id   = test_intern( SPEC[ i ].name );
        fields[ i ].type_hash = h_i32;
        fields[ i ].offset    = (uint16_t)SPEC[ i ].offset;
        fields[ i ].size      = sizeof( int32_t );
        fields[ i ].flags     = SPEC[ i ].flags;
    }

    uint16_t tid = ref_register_type( &type, fields, NFIELDS );
    ref_finalize_frame( game );

    bool all_ok = true;
    for ( int i = 0; i < NFIELDS; i++ )
    {
        const ref_field_t* f  = ref_find_field( tid, SPEC[ i ].name );
        bool               ok = f && f->flags == SPEC[ i ].flags;
        printf( "  [%s] %-14s flags=0x%08x (expect 0x%08x)\n",
                ok ? "ok  " : "FAIL", SPEC[ i ].name,
                f ? f->flags : 0xFFFFFFFFu, SPEC[ i ].flags );
        if ( !ok ) all_ok = false;
    }

    assert( all_ok );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Type-flags test

      One type per REF_TF_* bit, plus a type with both bits set and a zero-flags type.
      After finalize every ref_type_t.flags must equal the value set at registration time.
      All types are minimal stubs (one int32_t field) -- the content doesn't matter here.
==============================================================================================*/

typedef struct ref_test_tf_stub_s { int32_t x; } ref_test_tf_stub_t;

static void
test_type_flags_full( void )
{
    printf( "\n=== ref: type flags full ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    static const struct { const char* name; uint8_t flags; } SPEC[] = {
        { "tf_serialize",  REF_TF_SERIALIZE                 },
        { "tf_editor",     REF_TF_EDITOR                    },
        { "tf_all",        REF_TF_SERIALIZE | REF_TF_EDITOR },
        { "tf_none",       0                                },
    };
    enum { NTYPES = 4 };

    const uint32_t h_i32 = ref_hash_str( "int32_t" );

    uint16_t tids[ NTYPES ];
    for ( int i = 0; i < NTYPES; i++ )
    {
        ref_field_t stub = { 0 };
        stub.name_id     = test_intern( "x" );
        stub.type_hash   = h_i32;
        stub.offset      = 0;
        stub.size        = sizeof( int32_t );

        ref_type_t type  = { 0 };
        type.name_id     = test_intern( SPEC[ i ].name );
        type.name_hash   = ref_hash_str( SPEC[ i ].name );
        type.size        = REF_SIZEOF( ref_test_tf_stub_t );
        type.align       = REF_ALIGNOF( ref_test_tf_stub_t );
        type.kind        = REF_KIND_STRUCT;
        type.flags       = SPEC[ i ].flags;

        tids[ i ] = ref_register_type( &type, &stub, 1 );
    }

    ref_finalize_frame( game );

    bool all_ok = true;
    for ( int i = 0; i < NTYPES; i++ )
    {
        const ref_type_t* t  = ref_get_type( tids[ i ] );
        bool              ok = t && t->flags == SPEC[ i ].flags;
        printf( "  [%s] %-14s flags=0x%02x (expect 0x%02x)\n",
                ok ? "ok  " : "FAIL", SPEC[ i ].name,
                t ? t->flags : 0xFFu, SPEC[ i ].flags );
        if ( !ok ) all_ok = false;
    }

    assert( all_ok );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Union test

      Reflect:
          union value_u { int32_t i; float f; uint8_t bytes[4]; };

      All three fields share offset 0.  After finalize:
        - kind == REF_KIND_UNION
        - every field->offset == 0
        - size == sizeof( union ) (4 bytes on all targets)
        - field lookup by name round-trips correctly
        - live value written as int32_t is readable as float/bytes via offset cast
==============================================================================================*/

typedef union ref_test_value_u
{
    int32_t  i;
    float    f;
    uint8_t  bytes[ 4 ];

} ref_test_value_t;

static void
test_union( void )
{
    printf( "\n=== ref: union registration ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    const uint32_t h_value = ref_hash_str( "value_u" );
    const uint32_t h_i32   = ref_hash_str( "int32_t" );
    const uint32_t h_float = ref_hash_str( "float" );
    const uint32_t h_u8    = ref_hash_str( "uint8_t" );

    ref_type_t type = { 0 };
    type.name_id   = test_intern( "value_u" );
    type.name_hash = h_value;
    type.size      = REF_SIZEOF( ref_test_value_t );
    type.align     = REF_ALIGNOF( ref_test_value_t );
    type.kind      = REF_KIND_UNION;

    /* All fields in a union share offset 0.  size is the widest member. */
    ref_field_t fields[ 3 ] = { 0 };

    fields[ 0 ].name_id   = test_intern( "i" );
    fields[ 0 ].type_hash = h_i32;
    fields[ 0 ].offset    = REF_OFFSETOF( ref_test_value_t, i );
    fields[ 0 ].size      = REF_FIELD_SIZE( ref_test_value_t, i );

    fields[ 1 ].name_id   = test_intern( "f" );
    fields[ 1 ].type_hash = h_float;
    fields[ 1 ].offset    = REF_OFFSETOF( ref_test_value_t, f );
    fields[ 1 ].size      = REF_FIELD_SIZE( ref_test_value_t, f );

    fields[ 2 ].name_id   = test_intern( "bytes" );
    fields[ 2 ].type_hash = h_u8;
    fields[ 2 ].offset    = REF_OFFSETOF( ref_test_value_t, bytes );
    fields[ 2 ].size      = REF_FIELD_SIZE( ref_test_value_t, bytes );
    fields[ 2 ].mods      = REF_MODS_ARRAY;
    fields[ 2 ].aux       = 4;

    uint16_t tid = ref_register_type( &type, fields, 3 );
    ref_finalize_frame( game );

    ref_print_type( tid );

    const ref_type_t* t = ref_get_type( tid );

    bool all_ok = true;

    /* kind must be UNION */
    bool kind_ok = t && t->kind == REF_KIND_UNION;
    printf( "  [%s] kind == REF_KIND_UNION\n", kind_ok ? "ok  " : "FAIL" );
    if ( !kind_ok ) all_ok = false;

    /* size must match the C union */
    bool size_ok = t && t->size == REF_SIZEOF( ref_test_value_t );
    printf( "  [%s] size == %u (expect %u)\n", size_ok ? "ok  " : "FAIL",
            (unsigned)( t ? t->size : 0 ), (unsigned)REF_SIZEOF( ref_test_value_t ) );
    if ( !size_ok ) all_ok = false;

    /* every field must have offset == 0 */
    static const char* NAMES[] = { "i", "f", "bytes" };
    for ( int i = 0; i < 3; i++ )
    {
        const ref_field_t* f      = ref_find_field( tid, NAMES[ i ] );
        bool               off_ok = f && f->offset == 0;
        printf( "  [%s] field '%s' offset=%u (expect 0)\n", off_ok ? "ok  " : "FAIL",
                NAMES[ i ], f ? f->offset : 0xFFFFu );
        if ( !off_ok ) all_ok = false;
    }

    /* live value access: write i=0x3F800000 (bit pattern for 1.0f) and read back as float */
    ref_test_value_t v;
    v.i = 0x3F800000;
    const ref_field_t* ff = ref_find_field( tid, "f" );
    float              fv = *(const float*)( (const char*)&v + ff->offset );
    bool               fv_ok = fv == 1.0f;
    printf( "  [%s] i=0x3F800000 read as float -> %.6f (expect 1.000000)\n",
            fv_ok ? "ok  " : "FAIL", fv );
    if ( !fv_ok ) all_ok = false;

    assert( all_ok );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Tagged-union discriminant test

      Reflect a tagged union:
          struct shape_t {
              int32_t kind;                 // discriminant: 0 = circle, 1 = seg
              union { circle_t circle; seg_t seg; } data;   // @union_tag("kind")
          };
      circle_t has no pointers; seg_t holds two vec3_t* pointers.

      The union 'data' field carries @union_tag("kind"); each member carries @case(N).
      ref_walk_refs must visit ONLY the active member's pointer slots:
        - kind = seg (1):    2 visits (seg.a, seg.b)
        - kind = circle (0): 0 visits (seg's overlapping pointers must NOT be touched)
==============================================================================================*/

typedef struct ref_test_circle_s
{
    float radius;
} ref_test_circle_t;

typedef struct ref_test_seg_s
{
    ref_test_vec3_t* a;
    ref_test_vec3_t* b;
} ref_test_seg_t;

typedef union ref_test_shape_data_u
{
    ref_test_circle_t circle;
    ref_test_seg_t    seg;
} ref_test_shape_data_t;

typedef struct ref_test_shape_s
{
    int32_t               kind;
    ref_test_shape_data_t data;
} ref_test_shape_t;

enum { REF_TEST_SHAPE_CIRCLE = 0, REF_TEST_SHAPE_SEG = 1 };

typedef struct { int count; } ref_test_count_ctx_t;

static void
ref_test_count_visitor( void** slot, uint16_t pointee_id, const ref_field_t* f, void* user )
{
    (void)slot; (void)pointee_id; (void)f;
    ( (ref_test_count_ctx_t*)user )->count++;
}

static uint16_t
ref_test_field_id( uint16_t type_id, const char* name )
{
    /* Bridge a field pointer back to its id for the *_add_attr calls. */
    const ref_field_t* f = ref_find_field( type_id, name );
    assert( f );
    return (uint16_t)( f - ref_get_field( 0 ) );
}

static void
test_union_tagged( void )
{
    printf( "\n=== ref: tagged union discriminant ===\n" );

    ref_init();
    uint16_t game = ref_push_frame( "game" );

    ref_test_register_vec3();

    const uint32_t h_vec3 = ref_hash_str( "vec3_t" );
    const uint32_t h_f32  = ref_hash_str( "float" );
    const uint32_t h_i32  = ref_hash_str( "int32_t" );

    /* circle_t { float radius; } */
    {
        ref_type_t  type   = { 0 };
        type.name_id       = test_intern( "circle_t" );
        type.name_hash     = ref_hash_str( "circle_t" );
        type.size          = REF_SIZEOF( ref_test_circle_t );
        type.align         = REF_ALIGNOF( ref_test_circle_t );
        type.kind          = REF_KIND_STRUCT;

        ref_field_t f      = { 0 };
        f.name_id          = test_intern( "radius" );
        f.type_hash        = h_f32;
        f.offset           = REF_OFFSETOF( ref_test_circle_t, radius );
        f.size             = REF_FIELD_SIZE( ref_test_circle_t, radius );
        ref_register_type( &type, &f, 1 );
    }

    /* seg_t { vec3_t* a; vec3_t* b; } */
    {
        ref_type_t  type    = { 0 };
        type.name_id        = test_intern( "seg_t" );
        type.name_hash      = ref_hash_str( "seg_t" );
        type.size           = REF_SIZEOF( ref_test_seg_t );
        type.align          = REF_ALIGNOF( ref_test_seg_t );
        type.kind           = REF_KIND_STRUCT;

        ref_field_t fs[ 2 ] = { 0 };
        fs[ 0 ].name_id     = test_intern( "a" );
        fs[ 0 ].type_hash   = h_vec3;
        fs[ 0 ].offset      = REF_OFFSETOF( ref_test_seg_t, a );
        fs[ 0 ].size        = REF_FIELD_SIZE( ref_test_seg_t, a );
        fs[ 0 ].mods        = REF_MODS_PTR;
        fs[ 1 ].name_id     = test_intern( "b" );
        fs[ 1 ].type_hash   = h_vec3;
        fs[ 1 ].offset      = REF_OFFSETOF( ref_test_seg_t, b );
        fs[ 1 ].size        = REF_FIELD_SIZE( ref_test_seg_t, b );
        fs[ 1 ].mods        = REF_MODS_PTR;
        ref_register_type( &type, fs, 2 );
    }

    /* union shape_data { circle_t circle; seg_t seg; } -- members carry @case(N). */
    uint16_t union_tid;
    {
        ref_type_t  type    = { 0 };
        type.name_id        = test_intern( "shape_data_u" );
        type.name_hash      = ref_hash_str( "shape_data_u" );
        type.size           = REF_SIZEOF( ref_test_shape_data_t );
        type.align          = REF_ALIGNOF( ref_test_shape_data_t );
        type.kind           = REF_KIND_UNION;

        ref_field_t fs[ 2 ] = { 0 };
        fs[ 0 ].name_id     = test_intern( "circle" );
        fs[ 0 ].type_hash   = ref_hash_str( "circle_t" );
        fs[ 0 ].offset      = REF_OFFSETOF( ref_test_shape_data_t, circle );
        fs[ 0 ].size        = REF_FIELD_SIZE( ref_test_shape_data_t, circle );
        fs[ 1 ].name_id     = test_intern( "seg" );
        fs[ 1 ].type_hash   = ref_hash_str( "seg_t" );
        fs[ 1 ].offset      = REF_OFFSETOF( ref_test_shape_data_t, seg );
        fs[ 1 ].size        = REF_FIELD_SIZE( ref_test_shape_data_t, seg );
        union_tid           = ref_register_type( &type, fs, 2 );

        /* @case(0) on circle, @case(1) on seg -- one attr per field, kept contiguous. */
        ref_attrib_t c   = { 0 };
        c.name_id        = test_intern( REF_ANAME_CASE );
        c.type           = REF_ATTR_INT;
        c.flags          = REF_AF_CASE;
        c.value.i32      = REF_TEST_SHAPE_CIRCLE;
        ref_field_add_attr( ref_test_field_id( union_tid, "circle" ), &c );
        c.value.i32      = REF_TEST_SHAPE_SEG;
        ref_field_add_attr( ref_test_field_id( union_tid, "seg" ), &c );
    }

    /* shape_t { int32_t kind; shape_data_u data; } -- data carries @union_tag("kind"). */
    uint16_t shape_tid;
    {
        ref_type_t  type    = { 0 };
        type.name_id        = test_intern( "shape_t" );
        type.name_hash      = ref_hash_str( "shape_t" );
        type.size           = REF_SIZEOF( ref_test_shape_t );
        type.align          = REF_ALIGNOF( ref_test_shape_t );
        type.kind           = REF_KIND_STRUCT;

        ref_field_t fs[ 2 ] = { 0 };
        fs[ 0 ].name_id     = test_intern( "kind" );
        fs[ 0 ].type_hash   = h_i32;
        fs[ 0 ].offset      = REF_OFFSETOF( ref_test_shape_t, kind );
        fs[ 0 ].size        = REF_FIELD_SIZE( ref_test_shape_t, kind );
        fs[ 1 ].name_id     = test_intern( "data" );
        fs[ 1 ].type_hash   = ref_hash_str( "shape_data_u" );
        fs[ 1 ].offset      = REF_OFFSETOF( ref_test_shape_t, data );
        fs[ 1 ].size        = REF_FIELD_SIZE( ref_test_shape_t, data );
        shape_tid           = ref_register_type( &type, fs, 2 );

        ref_attrib_t u   = { 0 };
        u.name_id        = test_intern( REF_ANAME_UNION_TAG );
        u.type           = REF_ATTR_STRING;
        u.flags          = REF_AF_UNION_TAG;
        u.value.str      = test_intern( "kind" );
        ref_field_add_attr( ref_test_field_id( shape_tid, "data" ), &u );
    }

    ref_finalize_frame( game );

    ref_test_vec3_t va = { 1, 2, 3 };
    ref_test_vec3_t vb = { 4, 5, 6 };

    bool all_ok = true;

    /* Active member = seg: both seg pointers must be visited. */
    {
        ref_test_shape_t s = { 0 };
        s.kind            = REF_TEST_SHAPE_SEG;
        s.data.seg.a      = &va;
        s.data.seg.b      = &vb;

        ref_test_count_ctx_t ctx = { 0 };
        ref_walk_refs( &s, shape_tid, ref_test_count_visitor, &ctx );
        bool ok = ( ctx.count == 2 );
        printf( "  [%s] kind=seg    -> %d pointer visits (expect 2)\n", ok ? "ok  " : "FAIL", ctx.count );
        if ( !ok ) all_ok = false;
    }

    /* Active member = circle: no pointers; seg's overlapping slots must NOT be visited. */
    {
        ref_test_shape_t s = { 0 };
        s.kind            = REF_TEST_SHAPE_CIRCLE;
        s.data.circle.radius = 2.5f;

        ref_test_count_ctx_t ctx = { 0 };
        ref_walk_refs( &s, shape_tid, ref_test_count_visitor, &ctx );
        bool ok = ( ctx.count == 0 );
        printf( "  [%s] kind=circle -> %d pointer visits (expect 0)\n", ok ? "ok  " : "FAIL", ctx.count );
        if ( !ok ) all_ok = false;
    }

    /* Direct case lookup. */
    {
        const ref_field_t* m = ref_union_case_field( union_tid, REF_TEST_SHAPE_SEG );
        bool ok = m && strcmp( ref_cstr( m->name_id ), "seg" ) == 0;
        printf( "  [%s] union_case_field(1) -> %s (expect seg)\n", ok ? "ok  " : "FAIL",
                m ? ref_cstr( m->name_id ) : "<none>" );
        if ( !ok ) all_ok = false;
    }

    assert( all_ok );

    ref_pop_frame( game );
    ref_exit();
}

/*==============================================================================================
    Entry
==============================================================================================*/

void
ref_run_tests( void )
{
    printf( "\n" );
    printf( "========================================\n" );
    printf( "ref: Reflection System Tests\n" );
    printf( "  sizeof(ref_type_t)   = %zu\n", sizeof( ref_type_t ) );
    printf( "  sizeof(ref_field_t)  = %zu\n", sizeof( ref_field_t ) );
    printf( "  sizeof(ref_enum_t)   = %zu\n", sizeof( ref_enum_t ) );
    printf( "  sizeof(ref_attrib_t) = %zu\n", sizeof( ref_attrib_t ) );
    printf( "  sizeof(ref_frame_t)  = %zu\n", sizeof( ref_frame_t ) );
    printf( "========================================\n" );
    
    /* -- layer 1: primitives, field resolution, modifier shapes ---------------------------- */

    if ( 1 )
    {
        test_primitives();
        test_prim_fields();
        test_mods();
        test_union();
        test_union_tagged();
        test_attrs_full();
        test_field_flags_full();
        test_type_flags_full();
    }

    /* -- layer 2: struct/enum lifecycle, hot-reload, serialization ------------------------- */

    if ( 1 )
    {
        test_basic();
        test_mod_decode();
        test_hot_reload_rs();
        test_enums();
        test_bitset();
        test_function_sigs();
        test_walker();
        test_serialize();
    }

    /* -- layer 3: editor integration, usage patterns ----------------------------------------- */

    test_usage_example();

    printf( "\nrs: all tests complete.\n" );
}

/*============================================================================================*/
