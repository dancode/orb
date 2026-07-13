/*==============================================================================================

    tools/asset_tool/asset_tool_miniz.c -- implementation TU for the vendored miniz amalgamation.

    Compiled as its own object in the asset_tool target (not folded into asset_tool.c) so miniz's
    ~10k lines and its file-scope statics stay isolated from the tool's own code.  asset_tool.c
    calls the mz_zip_writer_* API (declared in vendor/miniz.h) to bundle a cooked tree into a .zip
    that core/fs can mount; the definitions live here.

    asset_tool builds its own copy of miniz rather than linking fs so the tool stays base+sys
    only -- no engine runtime.  The fs target compiles the same amalgamation separately for the
    fs ZIP reader (engine/fs/fs_zip_miniz.c); the two objects never meet.

    We keep stdio out of miniz (MINIZ_NO_STDIO): the writer builds the archive in a heap block,
    which asset_tool then writes to disk through the sys layer, matching the engine's approach.

    Vendored miniz 3.0.2 (public domain / MIT); see vendor/miniz_LICENSE.txt.

==============================================================================================*/

#define MINIZ_NO_STDIO   /* must precede the amalgamation */

/* Vendored third-party code: silence its warnings so the tool's /WX does not fail on miniz's own
   style (const-init, constant conditionals, narrowing).  Scope covers only the amalgamation. */
#if defined( _MSC_VER )
    #pragma warning( push, 0 )
    #pragma warning( disable : 4132 4127 4100 4244 4245 4267 4456 )
#endif

#include "vendor/miniz.c"            /* the amalgamation includes vendor/miniz.h itself */

#if defined( _MSC_VER )
    #pragma warning( pop )
#endif

/*============================================================================================*/
