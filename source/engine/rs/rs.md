# rs_ — Reflection System

Lean, allocation-free runtime reflection for C structs, enums, and function signatures.
Types are registered per-module into a shared global registry; unloading a module is a
single table truncation with no GC or tombstones.

## Files

| File              | Role
|                   |
| `rs.h`            | Public API — types, constants, registration, lookup, iteration, walkers, serialization |
| `rs_api.h`        | `rs_api_t` func-pointer struct + module accessor macros (`rs()`, `MOD_USE_RS`) |
| `rs_host.h`       | Host-only: `rs_wire_mod_callbacks()` — wires reflection into the mod lifecycle |
| `rs.c`            | Unity entry point: string pool, registry storage, `g_rs_api_struct`, mod descriptor |
| `rs_registry.c`   | Frame lifecycle, type/enum/function registration, lazy resolution, schema hashing |
| `rs_access.c`     | Lookup, field/enum/attribute queries, iteration |
| `rs_walk.c`       | Reference walker (`rs_walk_refs`) and value walker (`rs_walk`) |
| `rs_serialize.c`  | Binary read/write with schema-hash compatibility gating |
| `rs_print.c`      | `rs_field_describe`, `rs_print_type`, `rs_print_types`, `rs_print_frame` |
| `rs_test.c`       | Standalone test TU (not part of the library unity build; compiled into `sb_engine_reflect`) |


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

During registration each `rs_field_t` carries its base type as a `type_hash`
(`rs_hash_str` of the base type name) rather than a live type ID. `rs_register_type` does
a best-effort resolve immediately; `rs_finalize_frame` does a second pass for any fields
whose base type was registered after them. This means cross-type forward references work
regardless of registration order inside a frame, and across frames (a dependent frame can
reference types in a dependency's frame that was registered before it).

### Schema hash

Every registered type gets a deterministic `schema_hash` computed over its fields:
name, offset, size, modifier chain, array count, and base type hash. Any structural change
— adding/removing fields, reordering, changing types — produces a different hash. The
serializer gates compatibility on this value; callers use it to detect hot-reload ABI breaks.

### Packed modifier chain

`rs_field_t.mods` is a `uint16_t` holding one of the `rs_mods_t` enum values. The full
set of supported C-field shapes is a closed, named enumeration — invalid combinations are
unrepresentable. Use the `rs_mods_is_*` predicates or compare directly against the enum:

```
RS_MODS_VALUE        0x0000   T
RS_MODS_PTR          0x0001   T*
RS_MODS_PTR_PTR      0x0101   T**
RS_MODS_CONST_PTR    0x0009   T* const
RS_MODS_ARRAY        0x0002   T[N]        aux = element count
RS_MODS_PTR_ARRAY    0x0201   T*[N]       aux = element count
RS_MODS_ARRAY_PTR    0x0102   T(*)[N]     aux = element count
RS_MODS_FUNCTION     0x0004   T(*)()      aux = signature type_id
RS_MODS_CONST_VALUE  0x0010   const T
RS_MODS_PTR_TO_CONST 0x0011   const T*
```

The bit values preserve the encoding of the former two-slot scheme (low byte = innermost
declarator, high byte = outer), so the `schema_hash` of existing registered types is
unchanged.

Multi-dimensional arrays (`T[A][B]`) are not supported — wrap the inner dimension in a
struct or typedef.

### Flat attributes

Attributes are stored as contiguous `rs_attrib_t` runs in `attrs[]`. Multi-value metadata
(e.g. `@range(0, 100)`) becomes two entries with the same `name_id`. No secondary pools,
no variable-size blobs. Attribute payloads are 4 bytes (split across entries for larger
types). Strings are interned.

The `@transient` attribute causes `rs_write` to zero that field in the saved body; pointer
slots are always zeroed regardless.


## Data model

```c
rs_type_t   // a registered type: name, kind, size, align, schema_hash, field range, frame
rs_field_t  // a struct field: name, type_hash/type_id, offset, size, mods, aux, attrs
rs_enum_t   // one enumerator: name, value (int64_t)
rs_attrib_t // one attribute entry: name, type, 4-byte payload union
rs_frame_t  // one module's registration scope: table start marks, dll handle
```

Type IDs 0..(RS_PRIM_COUNT-1) are fixed built-in primitives (`RS_PRIM_I32`, etc.).
`RS_TYPE_INVALID` (0xFFFF) is the sentinel for unresolved or absent type references.


## String pool

`rs_` owns a 16 KB flat bump-allocator (`g_rs_str_pool`). `rs_intern(s)` returns an
`rs_name_t` (a `uint32_t` byte offset) — identical strings always return the same value.
`rs_cstr(id)` is a direct pointer into the pool with no copy.

`rs_hash_str` is case-insensitive FNV-1a. It must remain algorithmically identical to
`sid_hash`. Hash values are NOT the same as `rs_name_t` offsets.


## Module integration

### Host setup

Include `engine/rs/rs_host.h` in the host and call `rs_wire_mod_callbacks()` once after
`mod_system_init()`. This installs two mod-system callbacks:

- **pre_init** — called just before each module's `init()`, in dependency order. Pushes a
  frame and calls the module's generated `rs_register` function, then finalizes.
- **post_exit** — called just after each module's `exit()`. Pops the frame.

Because the pre_init hook fires in dependency order, a module's `init()` can already query
its own reflected types and those of all its dependencies.

```c
mod_system_init();
rs_wire_mod_callbacks();           // install hooks — nothing fires yet
mod_static_load( "rs",  ... );    // passive
mod_static_load( "core", ... );   // passive
mod_init_all();                   // pre_init fires in dep order (reflection), then init()
```

`rs_wire_mod_callbacks` is safe to call before `rs.mod_init` has run. The registry
self-bootstraps on first touch via `rs_ensure_init()` in `rs_registry.c`.

### Reflecting a module

In the module's `mod_desc_t`, set the `rs_register` slot using the generated header:

```c
#include "my_module.generated.h"   // declares my_module_rs_register

static mod_desc_t s_mod_desc = {
    .func_api    = &g_my_module_api,
    .rs_register = MOD_REFLECT_FUNC( my_module ),
    ...
};
```

The generated `my_module_rs_register(const rs_reg_api_t* api)` calls through the
`rs_reg_api_t` vtable rather than calling registration functions directly — this keeps
DLL modules from linking against the host's registration internals.

If `rs_register` is NULL the module is silently skipped; reflection is opt-in.

### Consuming rs at runtime

`rs_` is always statically linked. In dynamic builds, modules access it through the
standard `rs_api_t` gateway:

```c
// file scope — allocates the cached pointer in dynamic builds, no-op in static
MOD_USE_RS;

// in init() / reload()
if ( !MOD_FETCH_RS ) return false;

// call site identical in both build modes
uint16_t tid = rs()->find_type_by_name( "vec3_t" );
```


## Registration API

`rs_reg_api_t` is the vtable passed to generated registrar functions. Direct calls to
`rs_register_type` etc. are only valid from the host process (they access `g_rs` directly).

```c
typedef struct rs_reg_api_s {
    rs_name_t  (*intern)( const char* );
    uint16_t   (*rs_register_type)( const rs_type_t*, const rs_field_t*, uint16_t );
    uint16_t   (*rs_register_enum)( const rs_type_t*, const rs_enum_t*, uint16_t );
    uint16_t   (*rs_register_bitset)( const rs_type_t*, const rs_enum_t*, uint16_t );
    bool       (*rs_type_add_attr)( uint16_t type_id, const rs_attrib_t* );
    bool       (*rs_field_add_attr)( uint16_t field_id, const rs_attrib_t* );
    const rs_type_t* (*rs_get_type)( uint16_t type_id );
} rs_reg_api_t;
```

Attributes must be added contiguously per owner (all of type A's attrs before type B's).
The registration path enforces this with an assert.


## Walkers

### `rs_walk_refs` — pointer discovery

Visits every pointer-bearing slot in an instance. The visitor receives `(void** slot,
pointee_type_id, field, user)` and decides whether to chase, mark, or ignore. Recurses
into nested structs and inline arrays of structs automatically.

Supported shapes: `T`, `T*`, `T[N]`, `T*[N]`, `T(*)[N]`. Function pointers and chains
deeper than 2 modifiers are skipped.

### `rs_walk` — value traversal

Visits every field value. Visitor receives `(void* addr, type_id, field, user)`. Same
recursion rules. Function pointers and deep chains are visited as opaque slots.


## Serialization

`rs_write` serializes to a 20-byte header + raw `sizeof(T)` body. Pointer slots and
`@transient` fields are zeroed in the saved copy. `rs_read` verifies magic, `type_hash`,
`schema_hash`, and `body_size` before `memcpy`-ing the body. Any mismatch returns
`RS_IO_INCOMPAT`. Pointers read back as NULL.

```
Header layout (all u32 LE):
  [0]  magic        = 0x31307372  ('rs01')
  [4]  type_hash    = rs_hash_str(type name)
  [8]  schema_hash  = content hash of field layout
  [12] body_size    = sizeof(T)
  [16] reserved     = 0
```


## Limits

| Constant | Value |
|----------|-------|
| `RS_MAX_FRAMES` | 32 |
| `RS_MAX_TYPES` | 512 |
| `RS_MAX_FIELDS` | 4096 |
| `RS_MAX_ATTRS` | 1024 |
| `RS_MAX_ENUMS` | 1024 |
| String pool | 16 KB |
| Modifier shapes (`rs_mods_t` values) | 10 |

All storage is static global (`g_rs`). No heap allocation at any point.
