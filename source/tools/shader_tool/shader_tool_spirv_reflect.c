/*==============================================================================================

    tools/shader_tool/shader_tool_spirv_reflect.c -- implementation TU for vendored SPIRV-Reflect.

    Compiled as its own object in the shader_tool target (not folded into shader_tool.c) so the
    library's ~7k lines and file-scope statics stay isolated from the tool's own code, matching
    the asset_tool_miniz.c precedent.  shader_tool.c calls the spvReflect* API (declared in
    vendor/spirv_reflect.h) to extract descriptor bindings, push constant layouts, and vertex
    inputs from compiled SPIR-V; the definitions live here.

    Vendored from the Vulkan SDK (Source/SPIRV-Reflect, Apache 2.0 -- license header in the
    sources).  vendor/include/spirv/unified1/spirv.h is the library's bundled SPIR-V header,
    resolved relative to spirv_reflect.h; nothing else is required.

==============================================================================================*/

/* Vendored third-party code: silence its warnings so the tool's /WX does not fail on the
   library's own style.  Scope covers only the vendored implementation. */
#if defined( _MSC_VER )
    #pragma warning( push, 0 )
#endif

#include "vendor/spirv_reflect.c"    /* includes vendor/spirv_reflect.h itself */

#if defined( _MSC_VER )
    #pragma warning( pop )
#endif

/*============================================================================================*/
