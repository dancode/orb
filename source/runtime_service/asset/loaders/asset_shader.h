#ifndef ASSET_SHADER_H
#define ASSET_SHADER_H
/*==============================================================================================

    runtime_service/asset/loaders/asset_shader.h -- built-in shader asset resource.

    The asset service auto-registers a "shader" type (see asset_api.c) for cooked .oshd
    containers (rhi_shader_format.h, produced by `shader_tool cook` or the asset_tool .hlsl
    dispatch).  acquire()ing one parses the container through rhi()->shader_load_oshd_memory
    -- stage and entry come from the file, reflection lands in the RHI shader slot, and the
    bindless contract is enforced at load.  get() returns a pointer to this struct.

    Hot reload (asset()->refresh()) re-runs the loader in place behind the same asset id, so
    the rhi_shader_t handle CHANGES on every reload -- re-get() after refresh() reports work.
    Pipelines built from the old handle keep working but keep the old code; rebuild them from
    the new handle to pick up the change.

    layout_hash is the reload gate: it fingerprints everything the CPU side must agree with
    (inputs, push constant layout, bindings).  Cache it next to the pipeline you build:

        equal after a reload  -- pure code change; rebuild the pipeline with the same desc.
        differs               -- ABI break: hand-written push constant structs / vertex
                                 layouts may be stale.  pipeline_create still validates a
                                 hand-filled desc against the new reflection, so a stale desc
                                 fails loudly there rather than corrupting GPU reads.

==============================================================================================*/

#include "orb.h"
#include "runtime_service/rhi/rhi.h"

/* Extension claimed by the built-in shader type. */
#define ASSET_SHADER_EXTS { ".oshd" }

typedef struct asset_shader_s
{
    rhi_shader_t shader;        // RHI handle; a NEW handle after every hot reload
    u32          stage;         // OSHD_STAGE_* (rhi_shader_format.h)
    u32          pc_size;       // push constant span the shader reads, bytes
    u64          layout_hash;   // ABI fingerprint -- compare across reloads (see banner)

} asset_shader_t;

/*============================================================================================*/
#endif    // ASSET_SHADER_H
