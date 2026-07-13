/*==============================================================================================

    engine/fs/fs_zip_miniz.c -- implementation TU for the vendored miniz amalgamation.

    Compiled as its own object in the fs target (NOT folded into fs.c's unity build) so miniz's
    ~10k lines and its file-scope statics stay isolated from the engine's own code.  fs.c (inside
    fs's unity TU) calls the mz_zip_* reader API declared in vendor/miniz.h; the definitions live
    here.  fs_zip.h sets the shared miniz configuration for both sides.

    Vendored miniz 3.0.2 (public domain / MIT); see vendor/miniz_LICENSE.txt.

==============================================================================================*/

#include "engine/fs/fs_zip.h"        /* MINIZ_NO_STDIO -- must precede the amalgamation */

/* Vendored third-party code: silence its warnings so the engine's /WX (warnings-as-error) does
   not fail on miniz's own style (const-init, constant conditionals, narrowing).  Our own code
   keeps /WX -- this scope covers only the amalgamation. */
#if defined( _MSC_VER )
    #pragma warning( push, 0 )
    #pragma warning( disable : 4132 4127 4100 4244 4245 4267 4456 )
#endif

#include "vendor/miniz.c"            /* the amalgamation includes vendor/miniz.h itself */

#if defined( _MSC_VER )
    #pragma warning( pop )
#endif

/*============================================================================================*/
