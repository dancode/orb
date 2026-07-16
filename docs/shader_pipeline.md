# The ORB Shader Pipeline

*A bottom-to-top walkthrough of the shader reflection system: what each piece is, where it
lives, why it exists, and how it all connects when a frame renders.*

This document covers the full shader campaign: `shader_tool`, the `.oshd` container format,
reflection-driven pipeline creation in the RHI, generated C layout headers, asset-pipeline
integration, and the migration of the engine's own gui/draw shaders to the new path (with the
old embedded SPIR-V kept as a permanent fallback).

---

## Part 0: The problem this whole system solves

A shader is a small program that runs on the GPU. The engine's Vulkan backend needs two things
from every shader:

1. **The compiled code** -- SPIR-V, Vulkan's portable bytecode format. GPUs don't run HLSL or
   GLSL text; a compiler turns the text into SPIR-V, and the driver turns SPIR-V into actual
   GPU machine code at pipeline-creation time.

2. **A description of the shader's interface** -- what vertex data it expects, how big its
   push-constant block is, which textures/samplers it binds. Vulkan makes the *CPU* declare
   all of this when creating a pipeline. If the CPU-side declaration and the shader disagree,
   you get anything from garbage rendering to validation errors to crashes.

Before this system, ORB handled (2) entirely by hand: every pipeline hard-coded its vertex
attributes and push-constant size, and a comment next to the embedded SPIR-V array said
"trust me, this matches the shader." That works, but it is fragile in exactly the way
hand-maintained parallel truths always are -- edit the shader, forget the C struct, and
nothing tells you until pixels look wrong.

The core idea of the campaign: **the compiled shader already knows its own interface.**
SPIR-V is a structured format that can be *reflected* -- walked programmatically to extract
inputs, push constants, and bindings. So instead of humans keeping two descriptions in sync,
we extract the shader's interface at *cook time* (offline, when the shader is compiled), store
it alongside the bytecode in one file, and let the runtime *derive or validate* the CPU-side
declarations from it. The single source of truth becomes the shader source file itself.

A note on vocabulary used throughout:

- **Cook** -- ORB's term for offline conversion of a source asset into an engine-ready binary
  (same word the texture path uses). For shaders: HLSL text in, `.oshd` container out.
- **Push constants** -- a tiny (128-byte guaranteed) block of data the CPU shoves directly
  into the command buffer per draw call. ORB uses it for the MVP matrix and per-draw indices;
  it is the fastest way to get small per-draw data to a shader.
- **Bindless** -- ORB's descriptor model. Instead of binding specific textures per draw, the
  RHI maintains one giant global array of textures (set 0, binding 0) and one of samplers
  (set 0, binding 1). Shaders index into these arrays with integers passed in push constants.
  Every pipeline shares one descriptor layout; draws never rebind descriptor sets.

---

## Part 1: shader_tool -- the compiler front end

**Location:** `source/tools/shader_tool/` (standalone exe, links base + sys only)

The bottom layer is a command-line tool that wraps two third-party pieces:

- **dxc** -- Microsoft's DirectX Shader Compiler, shipped inside the Vulkan SDK
  (`%VULKAN_SDK%\Bin\dxc.exe`). It compiles HLSL to SPIR-V. ORB spawns it as a child process
  (via `sys_process_run`, the same pattern `font_tool` uses to spawn external tools) rather
  than linking `dxcompiler.dll` -- spawning keeps the tool dependency-free and the dxc version
  swappable with the SDK.
- **SPIRV-Reflect** -- a small single-file C library that parses SPIR-V and reports its
  interface. Vendored *tool-locally* at `source/tools/shader_tool/vendor/` because only the
  cooker ever reflects; the runtime never links a reflection library (that is the point of
  the container format in Part 2).

### Why HLSL and not GLSL?

The engine's original shaders were GLSL compiled with `glslc`. The campaign standardizes on
HLSL because it is the industry-dominant shading language, dxc is actively developed by
Microsoft with first-class SPIR-V output, and HLSL's `[[vk::...]]` attributes give explicit
control over Vulkan-specific details (binding numbers, input locations, push constants) right
in the source.

### The baked house conventions

`shader_tool compile` does not expose raw dxc flags; it bakes ORB's conventions in, so every
shader in the project is compiled identically:

```
-spirv -fspv-target-env=vulkan1.3 -WX -Zpc
```

- `-spirv` / `-fspv-target-env=vulkan1.3` -- emit SPIR-V for Vulkan 1.3 (the RHI's target).
- `-WX` -- warnings are errors. A shader warning at cook time is cheap; a rendering mystery
  at runtime is not.
- `-Zpc` -- **column-major matrix packing**. This one matters: it makes `mul( pc.mvp, v )` in
  HLSL consume the exact same bytes as `pc.mvp * v` did in the GLSL, so the CPU-side matrix
  code did not have to change when the shaders migrated.
- `-fvk-invert-y` -- added automatically for vertex-stage profiles. HLSL sources are
  conventionally authored in D3D clip space (+y up); Vulkan's clip space has +y down. This
  flag makes dxc emit a y-flip so D3D-authored shaders come out right in Vulkan. (Part 8
  explains the wrinkle this caused for the engine's own shaders.)

### Verbs

`shader_tool` grew one verb per phase; the full set:

| Verb | What it does |
|------|--------------|
| `compile <src.hlsl> -o <out.spv> -T <profile>` | HLSL to raw SPIR-V via dxc |
| `reflect <file.spv \| file.oshd>` | Print the shader's interface (inputs, push constants, bindings) |
| `cook <src.hlsl> -o <out.oshd> -T <profile>` | compile + reflect + serialize into one `.oshd` container |
| `header <file.oshd> -o <out.h>` | Generate a C header describing the push-constant layout |

The `-T` profile (`vs_6_0`, `ps_6_0`, ...) tells dxc which shader stage to compile. `reflect`
is primarily a debugging/inspection tool -- run it on any shader to see exactly what the
container (or raw SPIR-V) says its interface is.

---

## Part 2: The .oshd container -- reflection travels with the bytecode

**Location:** `source/runtime_service/rhi/rhi_shader_format.h` (the format contract, a
dependency-free header shared by tool and runtime)

Raw `.spv` files carry only bytecode. To use reflection at runtime you would have to either
link SPIRV-Reflect into the engine (heavyweight, and reflection is really a cook-time job) or
ship a sidecar metadata file next to every `.spv` (two files that can drift apart or get
separated). ORB chose a third option: **one container file that holds both**, cooked once,
consumed forever.

An `.oshd` ("ORB shader") file holds exactly one shader stage -- a pipeline references a
vertex + pixel pair like `gui.vs.oshd` + `gui.ps.oshd`. The layout is a fixed 64-byte header
followed by four tightly packed sections:

```
[ oshd_header_t ]                              64 bytes, counts + offsets + layout_hash
[ oshd_input_t      x input_count     ]        vertex inputs, sorted by location
[ oshd_pc_member_t  x pc_member_count ]        push constant members, pre-order flattened
[ oshd_binding_t    x binding_count   ]        descriptor bindings
[ string table ]                               all names, NUL-terminated, deduped
[ SPIR-V payload ]                             the actual bytecode
```

Design points worth understanding:

- **No translation tables.** The `vk_format` and `descriptor_type` fields store SPIRV-Reflect's
  numeric values, which deliberately alias `VkFormat` / `VkDescriptorType`. The runtime casts
  them straight into Vulkan structures.
- **String table with offset 0 = ""**. Names are optional debugging aids; the empty string at
  offset 0 means "unnamed" costs nothing. The table is padded to a multiple of 4 so the
  SPIR-V payload stays 4-aligned when the file is memory-mapped (Vulkan requires 4-aligned
  SPIR-V).
- **Push-constant members are flattened pre-order with a depth field.** A nested struct member
  appears after its parent at depth+1, and all offsets are *absolute* within the block. Leaf
  members alone define the byte contract; depth and type exist so the header generator
  (Part 4) can reconstruct real C struct nesting.
- **`count 0` on a binding means unbounded runtime array** -- exactly the shape of the bindless
  set-0 texture/sampler arrays. This lets the loader recognize and enforce the bindless
  contract.

### layout_hash -- the ABI fingerprint

The most important field is `layout_hash`: an FNV-1a 64 hash over everything the *CPU side*
must agree with -- stage, entry point, vertex inputs, push-constant layout, bindings, names
included. Think of it as a checksum of the shader's calling convention.

Its purpose is **hot reload**. When a shader is recooked while the engine runs, the consumer
compares the old and new hashes:

- **Equal** -- only the shader *body* changed (different math, different colors). The SPIR-V
  can be swapped freely; every CPU-side assumption still holds.
- **Different** -- the *interface* changed (new push-constant field, different vertex input).
  This is an ABI break: pipelines must be rebuilt and hand-filled descriptions re-validated,
  or the reload refused, per the caller's policy.

Only the cooker computes the hash; readers just compare u64s. Cooking is deterministic --
recooking unchanged source produces an identical hash.

The format is currently **version 2** (version 1 lacked the per-member type/elem_count fields
that the header generator needs; the bump invalidated all v1 hashes and required a global
recook, which is exactly the kind of break a version field exists to make loud instead of
silent).

---

## Part 3: The RHI loader -- reflection consumed at runtime

**Location:** `source/runtime_service/rhi/vk_shader_load.c` (loader),
`vk_pipeline_graphics.c` (derivation/validation), new vtable entries in `rhi_api.h`

The RHI (render hardware interface -- ORB's Vulkan wrapper) gained two loader entry points:

```c
rhi()->shader_load_oshd( path, debug_name );                 // from a file on disk
rhi()->shader_load_oshd_memory( blob, size, debug_name );    // from bytes (e.g. a zip bundle)
```

The memory variant is the real parser; the file variant is a thin wrapper. This split exists
for the asset system (Part 5), where shader bytes may arrive from inside a `.zip` bundle and
never touch the filesystem as a standalone file.

Loading an `.oshd` does several things a raw-SPIR-V load cannot:

1. **Stage and entry point come from the container** -- the caller no longer declares them
   (one less thing to get wrong).
2. **Structural validation** -- section math is checked in u64 (no overflow tricks), counts
   are bounded, version is checked.
3. **The bindless contract is enforced at load time.** Every descriptor binding in the shader
   must be set 0 / binding 0 (sampled images) or set 0 / binding 1 (samplers), unbounded.
   A shader that declares a UBO or an SSBO fails the load with a clear error -- *before*
   pipeline creation, at the moment the mistake is cheapest to diagnose. ORB's whole renderer
   runs on one shared descriptor layout; a shader that wants something else is simply not a
   valid ORB shader.
4. **The reflection tables are cached** on the shader slot (`vk_shader_reflect_t`: input
   locations/formats/sizes, push-constant size, layout hash). Shaders loaded from raw SPIR-V
   leave this empty and keep the old trust-the-caller behavior -- which is exactly how the
   embedded fallback shaders still work.

### What pipeline creation does with the cached reflection

`rhi()->pipeline_create` takes a description that includes vertex attributes and a
push-constant size. With reflection available, two new behaviors kick in:

**Derivation** -- if the caller passes `attrib_count == 0`, the pipeline derives the vertex
layout entirely from the vertex shader's reflection: attributes in location order, tightly
interleaved, stride = sum of sizes. Likewise `push_const_size == 0` derives the size from
reflection. A sandbox can now create a working pipeline from an almost-empty description --
the shader describes itself.

**Validation** -- if the caller *does* hand-fill attributes (as gui and draw do, because their
vertex buffers have specific strides and offsets), every input the shader reflects must be fed
by a compatible attribute. Extra attributes are legal (draw's solid pipeline binds only 2 of
the 3 attributes in its vertex buffer -- the shader doesn't read uv, so no one has to feed
it). A hand-declared push-constant size smaller than what the shader needs is a hard error.

"Compatible" deserves a word. The first implementation required *exact* `VkFormat` equality,
and that turned out to be too strict in a way that reflects real GPU behavior: gui's color
attribute is `UNORM4` (4 bytes, hardware-normalized to 0..1 floats during vertex fetch) while
the shader input reflects as `float4`. That pairing is completely legal -- format conversion
is what vertex fetch *does*. So validation was relaxed to `vk_vertex_input_compatible`:
the attribute and input must share a numeric class (float / signed int / unsigned int), and
the attribute must supply at least as many components as the shader reads. `UNORM4` feeding
`float4` passes; `float2` feeding `float4`, or an integer feeding a float, still fails.

---

## Part 4: Generated C headers -- the layout contract as compile errors

**Location:** the `header` verb in shader_tool

`shader_tool header foo.ps.oshd -o foo_layout.h` generates a C header from the reflection
tables:

- a `LAYOUT_HASH` define (compare against the runtime asset's hash),
- vertex input location defines (vertex stages only),
- descriptor SET/BINDING defines,
- and the centerpiece: a **`<base>_pc_t` struct** reconstructing the push-constant block, with
  typed fields (`float mvp[4][4]`, `u32 tex_idx`, ...) where the reflected type maps cleanly,
  raw `u8` byte blobs where it does not (std430 padding oddities like `float3` arrays degrade
  safely rather than lying), and explicit pad fields for gaps.

The critical feature is what follows the struct: a `_Static_assert( sizeof(...) == pc_size )`
plus one `_Static_assert( offsetof(...) == N )` **per field**. This means a generator
misjudgment -- or a compiler that packs differently than expected -- becomes a *compile
error*, never silent byte drift. The generated header is not "hopefully right"; it is
machine-checked against the same numbers the runtime validates.

The rationale: push constants are filled by `memcpy`-ing a C struct. If the C struct and the
shader block disagree on even one offset, the shader reads garbage with no error anywhere.
Generated + statically-asserted headers close that hole. (Proof case: the generated header
for a gui-shaped shader reproduced the hand-maintained `gui_shader.h` offsets exactly --
84-byte block, mvp at 0, indices at 64/68/72/76/80.)

---

## Part 5: Asset pipeline integration -- shaders as first-class assets

**Location:** asset_tool dispatch, `source/runtime_service/asset/loaders/asset_shader.{h,c}`

Three pieces connect the shader system to ORB's existing asset pipeline (the incremental
cooker + runtime asset service with id/refcount/hot-reload, from the earlier asset campaign):

**1. asset_tool understands `.hlsl`.** The stage is encoded in the *filename*:
`foo.vs.hlsl` cooks with profile `vs_6_0`, `foo.ps.hlsl` with `ps_6_0`, and so on for
cs/gs/hs/ds. An untagged `.hlsl` is a loud error (better than guessing), and `.hlsli` include
files copy verbatim. asset_tool spawns `shader_tool cook` per file and maps the output name
`foo.vs.hlsl -> foo.vs.oshd`, keeping the stage tag. One gotcha: the staleness cache is
per-file, so editing a shared `.hlsli` needs a forced recook (`-f`).

**2. The RHI memory-loader seam** (Part 3) exists precisely so cooked shaders can live inside
zip bundles like any other asset.

**3. A built-in "shader" asset type.** The asset service loads `.oshd` into an
`asset_shader_t { rhi_shader_t shader; u32 stage, pc_size; u64 layout_hash }`. The loader
peeks the header for the public fields; all real validation stays in the one RHI parser.

The hot-reload contract ties it together: when a shader asset refreshes (file changed on
disk), the service mints a **new** `rhi_shader_t` behind the same asset id. Consumers re-get
the asset and compare their cached `layout_hash` against the new one -- equal means rebuild
the pipeline from the new handle and carry on (a "safe swap"); different means ABI break,
handle it deliberately. The `sb_asset_shader` sandbox proves both branches live: edit a shader
body mid-run and watch the triangle recolor; grow its push-constant block and watch the
"layout hash CHANGED (ABI break)" path fire.

---

## Part 6: Migrating the engine's own shaders -- HLSL twins with a frozen fallback

**Location:** `source/runtime_service/gui/shaders/gui.{vs,ps}.hlsl`,
`source/runtime_service/draw/shaders/draw_{solid,tex}.{vs,ps}.hlsl`

Everything above was infrastructure proven on sandbox shaders. The final phase pointed it at
the engine's real shaders: the gui renderer (one vs/ps pair) and the draw service (solid and
textured pairs). These previously existed only as GLSL sources compiled offline by `glslc`
into C arrays embedded in `gui_shader.h` / `draw_shader.h`.

The design constraint, set explicitly at the start of the phase: **the cooked path is
additive, never a dependency.** The engine must compile and run with zero cook steps, and the
new pipeline must be switch-off-able. This shaped everything that follows.

### The HLSL twins

Each GLSL shader got an HLSL twin living next to it, a faithful port down to the comments:
same push-constant blocks (gui: 84 bytes -- mvp + five u32s for bindless indices and debug
modes; draw_solid: 64 bytes -- mvp only; draw_tex: 72 bytes -- mvp + texture/sampler
indices), same vertex inputs pinned with `[[vk::location(n)]]`, same bindless declarations:

```hlsl
[[vk::binding( 0, 0 )]] Texture2D    u_textures[] : register( t0, space0 );
[[vk::binding( 1, 0 )]] SamplerState u_samplers[] : register( s0, space0 );
```

The pixel shaders port the interesting logic intact: gui's three sampling modes (R8 coverage
for font/shape atlases, full RGBA, packed-RGBA8 debug tint/flat views) and the manual
`srgb_to_linear` conversion, expressed branchlessly with `lerp`/`step` in HLSL.

The GLSL sources and the embedded arrays were **deliberately not touched**. The two headers'
banners now read "FROZEN FALLBACK": new shader work happens in the HLSL twins; the GLSL exists
to document exactly what the frozen arrays contain, and the glslc recipe remains only as the
fallback regeneration procedure.

### The file-presence toggle

How do you make a feature opt-in without adding config plumbing? The smallest mechanism that
works: **file presence.** At init, `gui_render_init` and `draw_material_init` probe for
`<exe_dir>/shaders/<base>.{vs,ps}.oshd`:

```c
// probe with fopen so a missing pair stays silent (the normal fallback case)
FILE* fv = fopen( vs_path, "rb" );
...
if ( !fv || !fp ) return false;    // -> caller uses the embedded arrays
```

Rules baked into the probe:

- **Silent when absent.** A missing cooked pair is the normal state of a fresh build, not an
  error. (Going through `shader_load_oshd` directly would LOG_ERROR on a missing file --
  hence the fopen probe first.)
- **All-or-nothing per pair.** Both stages load from the same generation or neither does.
  Mixing an embedded GLSL vertex stage with a cooked HLSL pixel stage could pair mismatched
  interpolant locations -- never allowed to happen.
- **Any load failure falls back cleanly**, destroying the half-loaded stage.

The off switch is therefore: delete `bin\shaders`. No build flag, no cvar, no host change.
(gui and draw are static libs always linked into hosts, so they can call `sys_exe_dir`
directly to locate the exe -- rhi.c set that precedent.)

`scriptsok_shaders.bat` cooks all six shaders through asset_tool's stage-tag
dispatch into `bin\shaders`. Run it after editing an HLSL twin; skip it entirely and the
engine runs on the frozen arrays.

### On why this shape

This is the standing "smallest mechanism" principle applied: the alternative designs -- a
build-time define, a cvar, a host-passed config -- all add plumbing through layers that
otherwise do not care, and all create a mode that can be misconfigured. File presence is
self-describing, testable by renaming a folder, and costs two `fopen` calls at init.

---

## Part 7: The invert-y story -- the one genuinely tricky bit

This is the subtlest thing in the whole campaign and worth understanding properly.

**Background.** Direct3D and Vulkan disagree about which way y points in *clip space* (the
coordinate space a vertex shader outputs into): D3D has +y up, Vulkan +y down. HLSL shaders
are conventionally authored assuming D3D. So shader_tool bakes `-fvk-invert-y` into every
vertex-stage cook -- dxc appends a y-negation to the shader, and D3D-authored HLSL comes out
correct under Vulkan. That is the right house convention for shaders written from scratch.

**The wrinkle.** ORB's gui and draw shaders were *not* authored for D3D. Their CPU-side
matrices (`render_ortho` in gui, the mvp in draw) are built directly in **Vulkan** clip
space -- the GLSL twins compile with no y-flip anywhere and render correctly. Cook those same
shaders as HLSL and the baked `-fvk-invert-y` flips them: the entire UI renders upside down.

**The options were:**
1. Add a no-invert-y flag to shader_tool and thread it through asset_tool's dispatch -- but
   now the toolchain has per-file modes, tree cooks need per-file configuration, and the house
   convention has an exception encoded in tooling.
2. Rebuild the CPU matrices D3D-style -- touches working, proven math shared with the
   fallback path.
3. **Cancel the flip in the shader itself** -- one line after the multiply:

```hlsl
o.sv_pos   = mul( pc.mvp, float4( v.pos, 0.0, 1.0 ) );
o.sv_pos.y = -o.sv_pos.y;    // cancel the cook's -fvk-invert-y: mvp is already Vulkan-style
```

Option 3 won: the toolchain stays uniform (every vs cook gets the same flags, asset-tree
cooks stay correct), the CPU side stays untouched and byte-identical with the fallback path,
and the exception lives exactly where the reason for it lives -- in the shader source, with a
comment telling the full story (see `gui.vs.hlsl`). dxc's negation and this one compose to a
net no-op.

The general lesson: `-fvk-invert-y` is a *convention adapter*, and these four vertex shaders
sit on the other side of the convention. Any future HLSL shader whose matrices are authored
Vulkan-style needs the same one-line cancel; any shader authored D3D-style (the expected
default going forward) must NOT have it.

---

## Part 8: How it all fits together at runtime

The end-to-end flow when `sb_gui_editor` (or any host using gui/draw) starts with a populated
`bin\shaders`:

```
cook time (offline, once per shader edit):
    gui.vs.hlsl --dxc--> SPIR-V --SPIRV-Reflect--> tables --serialize--> gui.vs.oshd
    (cook_shaders.bat, or asset_tool over a project tree)

init time:
    gui_render_init
      -> probe <exe>/shaders/gui.{vs,ps}.oshd .......... both present
      -> rhi()->shader_load_oshd x2 .................... parse, validate bindless
                                                          contract, cache reflection
      -> "[gui] using cooked shaders"
      -> pipeline_create with hand-filled attribs ...... reflection VALIDATES them:
           FLOAT2 pos   feeds float2 loc 0             exact match
           FLOAT2 uv    feeds float2 loc 1             exact match
           UNORM4 color feeds float4 loc 2             compatible (class+components)
         push_const_size 84 == reflected 84            validated
      -> shader modules destroyed (only pipelines live on)

    draw_material_init: same dance for draw_solid + draw_tex pairs (64B / 72B blocks)

frame time:
    identical to before the campaign -- pipelines are pipelines; the cooked path
    changes how they are *created and checked*, not how they render
```

And with `bin\shaders` absent: the probes return false silently, the original
`shader_create`-from-embedded-array code runs unchanged, and reflection stays empty on those
shaders -- the pre-campaign trust-the-caller behavior, byte-for-byte the same pixels.

Verified proof points, for the record: cooked runs log matching push-constant sizes
(84 / 64 / 72) for all three pairs with zero validation errors; a fallback run (shaders
folder renamed away) renders cleanly with no oshd activity; recooking unchanged sources
yields identical layout hashes; and `sb_asset_shader` demonstrates both hot-reload branches
(safe swap and ABI break).

---

## Part 9: Quick reference

**Cook the engine shaders:** `scriptsok_shaders.bat` (needs `bin\asset_tool.exe`,
`bin\shader_tool.exe`, and dxc from `%VULKAN_SDK%`)

**Turn the cooked path off:** delete (or rename) `bin\shaders`

**Inspect any shader's interface:** `bin\shader_tool.exe reflect <file.oshd|file.spv>`

**Generate a C layout header:** `bin\shader_tool.exe header <file.oshd> -o <out.h>`

**Edit an engine shader:** edit the `.hlsl` twin, run `cook_shaders.bat`. Only regenerate the
embedded arrays (glslc recipe in `gui_shader.h` / `draw_shader.h`) if the *interface* changed,
and then keep GLSL + HLSL byte-identical in push constants and vertex inputs.

**Key files:**

| File | Role |
|------|------|
| `source/tools/shader_tool/` | compile / reflect / cook / header CLI |
| `source/runtime_service/rhi/rhi_shader_format.h` | `.oshd` format contract (tool + runtime) |
| `source/runtime_service/rhi/vk_shader_load.c` | `.oshd` parser + bindless contract enforcement |
| `source/runtime_service/rhi/vk_pipeline_graphics.c` | attrib/pc derivation + compatibility validation |
| `source/runtime_service/asset/loaders/asset_shader.c` | "shader" asset type + hot-reload |
| `source/runtime_service/gui/shaders/*.hlsl` | gui HLSL twins (live source) |
| `source/runtime_service/draw/shaders/*.hlsl` | draw HLSL twins (live source) |
| `gui_shader.h` / `draw_shader.h` | frozen fallback SPIR-V arrays |
| `cook_shaders.bat` | cooks all six engine shaders into `bin\shaders` |

**Invariants to remember:**

- One `.oshd` = one stage; pipelines pair a vs + ps cooked from the same generation.
- `layout_hash` equal = swap SPIR-V freely; different = ABI break.
- All shaders must speak the bindless contract (set 0 / b0 textures, b1 samplers, unbounded);
  the loader rejects anything else.
- Vulkan-style CPU matrices + `-fvk-invert-y` cook = the shader needs the one-line y cancel.
- The cooked path is additive: the engine must always build and run with `bin\shaders` empty.
