# ref_ — Reflection System

Lean, allocation-free runtime reflection for C structs, enums, and function signatures.
Types are registered per-module into a shared global registry; unloading a module is a
single table truncation with no GC or tombstones.

## Files

| File              | Role
|                   |
| `ref.h`            | Public API — types, constants, registration, lookup, iteration, walkers, serialization |
| `ref_api.h`        | `ref_api_t` func-pointer struct + module accessor macros (`ref()`, `MOD_USE_REF`) |
| `ref_host.h`       | Host-only: `ref_wire_mod_callbacks()` — wires reflection into the mod lifecycle |
| `ref.c`            | Unity entry point: string pool, registry storage, `g_ref_api_struct`, mod descriptor |
| `ref_registry.c`   | Frame lifecycle, type/enum/function registration, lazy resolution, schema hashing |
| `ref_access.c`     | Lookup, field/enum/attribute queries, iteration |
| `ref_walk.c`       | Reference walker (`ref_walk_refs`) and value walker (`ref_walk`) |
| `ref_serialize.c`  | Binary read/write with schema-hash compatibility gating |
| `ref_print.c`      | `ref_field_describe`, `ref_print_type`, `ref_print_types`, `ref_print_frame` |
| `ref_test.c`       | Standalone test TU (not part of the library unity build; `#include`d by `sb_reflect_unit.c`, built as `sb_reflect`) |


## Design

### Stack-frame registry

The registry holds four flat tables — `types[]`, `fields[]`, `attrs[]`, `enums[]` — plus a
stack of `frames[]`. Each frame records the starting index of each table at the moment it was
pushed. Popping a frame truncates all four tables back to those marks and unlinks the frame's
types from the hash table. No validity flags, no tombstones, no deferred passes.

```
push  ->  register  ->  finalize
pop   ->  truncate tables  (O(n types in frame) for hash unlinking, O(1) otherwise)
```

Frame 0 is the "reflect" frame and holds the built-in primitives. It cannot be popped.

### Lazy type resolution

During registration each `ref_field_t` carries its base type as a `type_hash`
(`ref_hash_str` of the base type name) rather than a live type ID. `ref_register_type` does
a best-effort resolve immediately; `ref_finalize_frame` does a second pass for any fields
whose base type was registered after them. This means cross-type forward references work
regardless of registration order inside a frame, and across frames (a dependent frame can
reference types in a dependency's frame that was registered before it).

### Schema hash

Every registered type gets a deterministic `schema_hash` computed over its fields:
name, offset, size, modifier chain, array count, and base type hash. Any structural change
— adding/removing fields, reordering, changing types — produces a different hash. The
serializer gates compatibility on this value; callers use it to detect hot-reload ABI breaks.

### Packed modifier chain

`ref_field_t.mods` is a `uint16_t` holding one of the `ref_mods_t` enum values. The full
set of supported C-field shapes is a closed, named enumeration — invalid combinations are
unrepresentable. Use the `ref_mods_is_*` predicates or compare directly against the enum:

```
REF_MODS_VALUE        0x0000   T
REF_MODS_PTR          0x0001   T*
REF_MODS_PTR_PTR      0x0101   T**
REF_MODS_CONST_PTR    0x0009   T* const
REF_MODS_ARRAY        0x0002   T[N]        aux = element count
REF_MODS_PTR_ARRAY    0x0201   T*[N]       aux = element count
REF_MODS_ARRAY_PTR    0x0102   T(*)[N]     aux = element count
REF_MODS_FUNCTION     0x0004   T(*)()      aux = signature type_id
REF_MODS_CONST_VALUE  0x0010   const T
REF_MODS_PTR_TO_CONST 0x0011   const T*
```

The bit values preserve the encoding of the former two-slot scheme (low byte = innermost
declarator, high byte = outer), so the `schema_hash` of existing registered types is
unchanged.

Multi-dimensional arrays (`T[A][B]`) are not supported — wrap the inner dimension in a
struct or typedef.

### Flat attributes

Attributes are stored as contiguous `ref_attrib_t` runs in `attrs[]`. Multi-value metadata
(e.g. `@range(0, 100)`) becomes two entries with the same `name_id`. No secondary pools,
no variable-size blobs. Attribute payloads are 4 bytes (split across entries for larger
types). Strings are interned.

The `@transient` attribute causes `ref_write` to zero that field in the saved body; pointer
slots are always zeroed regardless.


## Data model

```c
ref_type_t   // a registered type: name, kind, size, align, schema_hash, field range, frame
ref_field_t  // a struct field: name, type_hash/type_id, offset, size, mods, aux, attrs
ref_enum_t   // one enumerator: name, value (int32_t) — name_id+value pack into one 64-bit word
ref_attrib_t // one attribute entry: name, type, 4-byte payload union
ref_frame_t  // one module's registration scope: table start marks, dll handle
```

Type IDs 0..(REF_PRIM_COUNT-1) are fixed built-in primitives (`REF_PRIM_I32`, etc.).
`REF_TYPE_INVALID` (0xFFFF) is the sentinel for unresolved or absent type references.


## String pool

`ref_` owns a 16 KB flat bump-allocator (`g_ref_str_pool`). `ref_intern(s)` returns an
`ref_name_t` (a `uint32_t` byte offset) — identical strings always return the same value.
`ref_cstr(id)` is a direct pointer into the pool with no copy.

`ref_hash_str` is case-insensitive FNV-1a. It must remain algorithmically identical to
`sid_hash`. Hash values are NOT the same as `ref_name_t` offsets.


## Module integration

### Host setup

Include `engine/ref/ref_host.h` in the host and call `ref_wire_mod_callbacks()` once after
`mod_system_init()`. This installs two mod-system callbacks:

- **pre_init** — called just before each module's `init()`, in dependency order. Pushes a
  frame and calls the module's generated `ref_register` function, then finalizes.
- **post_exit** — called just after each module's `exit()`. Pops the frame.

Because the pre_init hook fires in dependency order, a module's `init()` can already query
its own reflected types and those of all its dependencies.

```c
mod_system_init();
ref_wire_mod_callbacks();           // install hooks — nothing fires yet
mod_static_load( "ref",  ... );   // passive
mod_static_load( "core", ... );   // passive
mod_init_all();                   // pre_init fires in dep order (reflection), then init()
```

`ref_wire_mod_callbacks` is safe to call before the `ref` module's `init` has run. The registry
self-bootstraps on first touch via `ref_ensure_init()` in `ref_registry.c`.

### Reflecting a module

In the module's `mod_desc_t`, set the `ref_register` slot using the generated header:

```c
#include "my_module.generated.h"   // declares my_module_ref_register

static mod_desc_t s_mod_desc = {
    .func_api    = &g_my_module_api,
    .ref_register = MOD_REFLECT_FUNC( my_module ),
    ...
};
```

The generated `my_module_ref_register(const ref_reg_api_t* api)` calls through the
`ref_reg_api_t` vtable rather than calling registration functions directly — this keeps
DLL modules from linking against the host's registration internals.

If `ref_register` is NULL the module is silently skipped; reflection is opt-in.

### Consuming ref at runtime

`ref_` is always statically linked. In dynamic builds, modules access it through the
standard `ref_api_t` gateway:

```c
// file scope — allocates the cached pointer in dynamic builds, no-op in static
MOD_USE_REF;

// in init() / reload()
if ( !MOD_FETCH_REF ) return false;

// call site identical in both build modes
uint16_t tid = ref()->find_type_by_name( "vec3_t" );
```


## Registration API

`ref_reg_api_t` is the vtable passed to generated registrar functions. Direct calls to
`ref_register_type` etc. are only valid from the host process (they access `g_ref` directly).

```c
typedef struct ref_reg_api_s {
    ref_name_t  (*intern)( const char* );
    uint16_t   (*ref_register_type)( const ref_type_t*, const ref_field_t*, uint16_t );
    uint16_t   (*ref_register_enum)( const ref_type_t*, const ref_enum_t*, uint16_t );
    uint16_t   (*ref_register_bitset)( const ref_type_t*, const ref_enum_t*, uint16_t );
    uint16_t   (*ref_register_function)( const ref_type_t*, const ref_field_t*, uint16_t );
    bool       (*ref_type_add_attr)( uint16_t type_id, const ref_attrib_t* );
    bool       (*ref_field_add_attr)( uint16_t field_id, const ref_attrib_t* );
    const ref_type_t* (*ref_get_type)( uint16_t type_id );
} ref_reg_api_t;
```

Attributes must be added contiguously per owner (all of type A's attrs before type B's).
The registration path enforces this with an assert.


## Walkers

### `ref_walk_refs` — pointer discovery

Visits every pointer-bearing slot in an instance. The visitor receives `(void** slot,
pointee_type_id, field, user)` and decides whether to chase, mark, or ignore. Recurses
into nested structs and inline arrays of structs automatically.

Supported shapes: `T`, `T*`, `T[N]`, `T*[N]`, `T(*)[N]`. Function pointers and chains
deeper than 2 modifiers are skipped.

### `ref_walk` — value traversal

Visits every field value. Visitor receives `(void* addr, type_id, field, user)`. Same
recursion rules. Function pointers and deep chains are visited as opaque slots.


## Serialization

`ref_write` serializes to a 20-byte header + raw `sizeof(T)` body. Pointer slots and
`@transient` fields are zeroed in the saved copy. `ref_read` verifies magic, `type_hash`,
`schema_hash`, and `body_size` before `memcpy`-ing the body. Any mismatch returns
`REF_IO_INCOMPAT`. Pointers read back as NULL.

```
Header layout (all u32 LE):
  [0]  magic        = 0x31307372  ('rs01')
  [4]  type_hash    = ref_hash_str(type name)
  [8]  schema_hash  = content hash of field layout
  [12] body_size    = sizeof(T)
  [16] reserved     = 0
```


## Limits

| Constant | Value |
|----------|-------|
| `REF_MAX_FRAMES` | 32 |
| `REF_MAX_TYPES` | 512 |
| `REF_MAX_FIELDS` | 4096 |
| `REF_MAX_ATTRS` | 1024 |
| `REF_MAX_ENUMS` | 1024 |
| String pool | 16 KB |
| Modifier shapes (`ref_mods_t` values) | 10 |

All storage is static global (`g_ref`). No heap allocation at any point.
