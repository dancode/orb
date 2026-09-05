# Shaders -- shader_tool, .oshd, reflection-driven pipelines

A shader's compiled code already knows its own interface.  ORB extracts that interface at
cook time, stores it beside the SPIR-V in one file, and lets the runtime derive or validate
the CPU-side pipeline description from it.  The shader source is the single source of
truth; a mismatch is a load error or a compile error, never silent byte drift.

---

## 1. shader_tool

`source/tools/shader_tool/`, links base + sys only.  It wraps two third-party pieces:
**dxc** (spawned from `%VULKAN_SDK%\Bin\dxc.exe`, never linked) compiles HLSL to SPIR-V;
**SPIRV-Reflect** (vendored tool-local) reads the SPIR-V back.  The runtime links neither.

    compile <src.hlsl> -o <out.spv>  -T <profile>    HLSL -> raw SPIR-V
    reflect <file.spv | file.oshd>                   print inputs, push constants, bindings
    cook    <src.hlsl> -o <out.oshd> -T <profile>    compile + reflect + serialize
    header  <file.oshd> -o <out.h>                   generate a C push-constant header

House flags are baked in so every shader compiles identically: `-spirv
-fspv-target-env=vulkan1.3 -WX -Zpc`, plus `-fvk-invert-y` on vertex-ish stages
(section 5).  `-Zpc` is column-major packing, so `mul( pc.mvp, v )` consumes the same bytes
the CPU writes.

The stage is encoded in the filename: `foo.vs.hlsl` cooks with `vs_6_0`, `foo.ps.hlsl` with
`ps_6_0`, and so on for cs/gs/hs/ds.  An untagged `.hlsl` is an error; `.hlsli` files are
includes and cook nothing.

---

## 2. The .oshd container

`source/runtime_service/rhi/rhi_shader_format.h` is the contract, dependency-free so tool
and runtime share it.  One file holds one stage; a pipeline references a vs + ps pair.

    [ oshd_header_t   ]   opens with res_ref_head_t (CONTENT.md section 5)
    [ reference section ] empty today: a shader names no other resource
    [ vertex inputs   ]   sorted by location
    [ pc members      ]   pre-order flattened with depth; absolute offsets
    [ bindings        ]
    [ string table    ]   offset 0 = "", padded to 4 so the payload stays aligned
    [ SPIR-V payload  ]

`vk_format` and `descriptor_type` store SPIRV-Reflect's numbers, which alias `VkFormat` /
`VkDescriptorType`, so the runtime casts them straight through.  A binding with count 0 is
an unbounded runtime array, the shape of the bindless set 0.

`layout_hash` is FNV-1a 64 over everything the CPU side must agree with: stage, entry,
inputs, push-constant layout, bindings, names included.  Only the cooker computes it;
readers compare.  On hot reload, equal means the body changed and the SPIR-V swaps
freely; different means the interface changed and pipelines must be rebuilt or the reload
refused, per the caller.

---

## 3. The RHI consumes it

    rhi()->shader_load_oshd( path, debug_name )
    rhi()->shader_load_oshd_memory( blob, size, debug_name )    // the real parser

Loading validates section math, bounds every count, checks the version, and enforces the
bindless contract: every descriptor binding must be set 0 / binding 0 (sampled images) or
set 0 / binding 1 (samplers), unbounded.  A shader declaring a UBO or SSBO fails to load
with a clear message.  The reflection tables are cached on the shader slot; shaders loaded
from raw SPIR-V leave them empty and keep trust-the-caller behavior.

`rhi()->pipeline_create` then does one of two things with a vertex shader that carries
reflection:

- **Derive.**  `attrib_count == 0` builds the vertex layout from reflection (location
  order, tightly interleaved); `push_const_size == 0` takes the size from reflection.
- **Validate.**  Hand-filled attributes must feed every reflected input with a compatible
  format: same numeric class (float / sint / uint) and at least as many components, so
  `UNORM4` feeding `float4` passes and `float2` feeding `float4` fails.  Extra attributes
  are legal.  A hand-declared push-constant size smaller than the shader's is an error.

`shader_tool header` writes a `<base>_pc_t` struct with typed fields where the reflected
type maps cleanly, byte blobs where it does not, explicit pads, and a `_Static_assert` on
`sizeof` and on every field offset.  Push constants are filled by memcpy of that struct, so
a wrong offset is a compile error.

---

## 4. Where the engine's shaders live

**gui**: `content/shader/gui_quad.{vs,ps}.hlsl` plus `gui_common.hlsli` / `gui_fx.hlsli`,
named in code as `RID( "shader/gui_quad.vs" )` / `.ps`.  The build's content phase cooks
them into `build/content/shader/` from the resource manifests (CONTENT.md section 4); gui
reads the pair through fs and `render_init` fails loudly with the path when one is
missing.  There is no second copy.

**draw**: `source/runtime_service/draw/shaders/draw_{solid,tex}.{vs,ps}.hlsl` are the live
source; `draw_shader.h` holds frozen embedded SPIR-V as the zero-cook fallback.
`draw_material_init` probes `<exe_dir>/shaders/<base>.{vs,ps}.oshd` and uses the cooked
pair when both stages are present, else the arrays.  `scripts/cook_shaders.bat` cooks the
four into `bin\shaders`; deleting that directory is the off switch.  Regenerate the arrays
only when the interface changes, and keep GLSL and HLSL identical in push constants and
vertex inputs.

**as an asset**: the asset service's built-in "shader" type loads `.oshd` into
`asset_shader_t { shader, stage, pc_size, layout_hash }`.  On refresh it mints a new
`rhi_shader_t` behind the same id; consumers compare `layout_hash` and either rebuild the
pipeline or treat it as an ABI break.  `sb_asset_shader` exercises both branches.

---

## 5. The invert-y rule

D3D clip space has +y up, Vulkan +y down.  HLSL is conventionally authored for D3D, so
shader_tool bakes `-fvk-invert-y` into every vertex-stage cook and D3D-style shaders come
out right.  The engine's own gui and draw shaders build their matrices Vulkan-style on the
CPU, so they cancel the flip with one line after the multiply:

    o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style

A new shader whose matrices are Vulkan-style needs the same line.  A shader authored
D3D-style must not have it.  The toolchain stays uniform; the exception lives in the source
that needs it.

---

## 6. Quick reference

    bin\shader_tool.exe reflect <file.oshd|file.spv>     inspect an interface
    bin\shader_tool.exe header  <file.oshd> -o <out.h>   generate a layout header
    bin\build_tool.exe -config Debug                     recooks a stale gui shader
    scripts\cook_shaders.bat                             cooks the draw pair into bin\shaders

Invariants: one `.oshd` = one stage, and a pipeline pairs stages cooked from the same
generation; `layout_hash` equal = swap freely, different = ABI break; every shader speaks
the bindless contract or the loader rejects it; Vulkan-style matrices + the baked flip =
the one-line cancel.
