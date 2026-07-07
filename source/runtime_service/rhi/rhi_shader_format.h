#ifndef RHI_SHADER_FORMAT_H
#define RHI_SHADER_FORMAT_H
/*==============================================================================================

    runtime_service/rhi/rhi_shader_format.h -- cooked shader container format (.oshd).

    The .oshd file is the CONTRACT between the offline cooker and the runtime loader
    (asset_tex.h precedent):

        writer  -- shader_tool's `cook` verb compiles HLSL through dxc, reflects the SPIR-V
                   with SPIRV-Reflect, and serializes the reflection tables + the SPIR-V blob
                   into this container.
        reader  -- the RHI's shader loader consumes the SPIR-V for vkCreateShaderModule and
                   the tables to derive/validate vertex input layout, push constant size, and
                   the bindless descriptor contract -- without linking any reflection library.

    One .oshd holds ONE shader stage (per-stage containers; a pipeline references a vs + ps
    pair).  File layout -- a fixed 64-byte header followed by four tightly packed sections:

        [ oshd_header_t ]
        [ oshd_input_t      x input_count     ]   vertex inputs, sorted by location
        [ oshd_pc_member_t  x pc_member_count ]   push constant members, pre-order, with depth
        [ oshd_binding_t    x binding_count   ]   descriptor bindings, as reflected
        [ string table      strtab_size bytes ]   NUL-terminated names; offset 0 = "" (unnamed)
        [ SPIR-V payload    spirv_size bytes  ]

    All fields are little-endian (tool and engine share the architecture).  strtab_size is
    padded to a multiple of 4 so the SPIR-V payload stays 4-aligned when the file is mapped.
    Enum fields deliberately reuse the numeric values SPIRV-Reflect reports:

        vk_format        == VkFormat        (SpvReflectFormat aliases VkFormat values)
        descriptor_type  == VkDescriptorType (SpvReflectDescriptorType aliases it likewise)

    so the runtime consumes them without a translation table.  binding count 0 means an
    unbounded runtime array -- the shape of the RHI's bindless set 0 arrays.

    layout_hash fingerprints everything the CPU side must agree with (stage, entry, inputs,
    push constant layout, bindings -- names included).  Hot reload compares the old and new
    file's hashes: equal = swap SPIR-V freely; different = ABI break, full pipeline rebuild
    (or a hard error, per the caller's policy).  The hash is FNV-1a 64 over the fields in
    file order; only the cooker computes it, readers just compare.

    This header is intentionally dependency-free (just u32/u64 from orb.h) so shader_tool --
    which links base + sys only, no engine runtime -- can include it alongside the RHI.

==============================================================================================*/

#include "orb.h"

/* 'O','S','H','D' in file order (little-endian store). */
#define OSHD_MAGIC \
    ( ( u32 )'O' | ( ( u32 )'S' << 8 ) | ( ( u32 )'H' << 16 ) | ( ( u32 )'D' << 24 ) )

#define OSHD_VERSION 1

/* Shader stage.  Own scale (not VkShaderStageFlagBits) so the file format does not encode a
   bitmask where exactly one value is legal. */
enum
{
    OSHD_STAGE_NONE         = 0,
    OSHD_STAGE_VERTEX       = 1,
    OSHD_STAGE_TESS_CONTROL = 2,
    OSHD_STAGE_TESS_EVAL    = 3,
    OSHD_STAGE_GEOMETRY     = 4,
    OSHD_STAGE_FRAGMENT     = 5,
    OSHD_STAGE_COMPUTE      = 6,
};

typedef struct oshd_header_s
{
    u32 magic;             // OSHD_MAGIC
    u32 version;           // OSHD_VERSION
    u32 stage;             // OSHD_STAGE_*
    u32 entry;             // strtab offset of the entry point name
    u32 input_count;       // oshd_input_t records
    u32 pc_member_count;   // oshd_pc_member_t records (0 = no push constant block)
    u32 binding_count;     // oshd_binding_t records
    u32 pc_name;           // strtab offset of the push constant block type name (0 if none)
    u32 pc_size;           // push constant span of declared members, bytes (0 if none)
    u32 pc_padded_size;    // std430-rounded size dxc actually reserves (0 if none)
    u32 strtab_size;       // string table bytes (multiple of 4; >= 1: offset 0 is "")
    u32 spirv_size;        // SPIR-V payload bytes (nonzero, multiple of 4)
    u32 flags;             // reserved (0)
    u32 pad;               // keeps layout_hash 8-aligned; write as 0
    u64 layout_hash;       // FNV-1a 64 fingerprint of the CPU-visible layout contract

} oshd_header_t;

/* One user vertex input (built-ins like SV_VertexID are not stored). */
typedef struct oshd_input_s
{
    u32 location;    // shader input location
    u32 vk_format;   // VkFormat numeric value (e.g. 103 = VK_FORMAT_R32G32_SFLOAT)
    u32 size;        // attribute byte size (stride derivation)
    u32 name;        // strtab offset

} oshd_input_t;

/* One push constant block member, flattened pre-order.  depth 0 = top-level member; nested
   struct members follow their parent with depth + 1.  Offsets are absolute within the block
   (exactly what the CPU struct must reproduce), so leaf members alone define the data
   contract -- depth exists so `header` generation can rebuild the nesting. */
typedef struct oshd_pc_member_s
{
    u64 name_hash;   // oshd_name_hash() of the member name
    u32 name;        // strtab offset
    u32 offset;      // absolute byte offset within the block
    u32 size;        // member byte size
    u32 depth;       // nesting depth (0 = top level)

} oshd_pc_member_t;

/* One descriptor binding. */
typedef struct oshd_binding_s
{
    u32 set;               // descriptor set index
    u32 binding;           // binding index within the set
    u32 descriptor_type;   // VkDescriptorType numeric value
    u32 count;             // array size; 0 = unbounded runtime array (bindless)
    u32 name;              // strtab offset

} oshd_binding_t;

/* oshd_name_hash -- FNV-1a 64 over a NUL-terminated name.  The same function serves the
   cooker (filling name_hash) and the runtime (looking up a member by name without touching
   the string table). */
static inline u64
oshd_name_hash( const char* s )
{
    u64 h = 0xcbf29ce484222325ull;
    while ( *s )
    {
        h ^= ( u64 )( u8 )*s++;
        h *= 0x100000001b3ull;
    }
    return h;
}

/*============================================================================================*/
#endif    // RHI_SHADER_FORMAT_H
