#ifndef GUI_RES_H
#define GUI_RES_H
/*==============================================================================================

    runtime_service/gui/gui_res.h -- how gui reads content bytes.

    Every file gui loads -- a font bake, an icon or sprite image, its own cooked shaders -- 
    is a resource NAME not a file (engine/res/res.h) plus the extension the loader accepts,
    read through the fs mounts.
    
    gui holds no root of its own: which directory, cooked mirror, or pack 
    answers a name is the host's mount table (run_host mounts content/ and build/content; the gui boot
    path does the same for the sandboxes).  A name that does not resolve reads as ok=false.

    Included by every gui unit that loads a file: the font unit (bakes), the draw unit (icons
    and sprites), and the render unit (the pipeline's .oshd pair).

==============================================================================================*/

#include "engine/res/res.h"
#include "engine/fs/fs_api.h"

/* Read `name` + `ext` (ext carries its own leading dot, "" for none) through the mounts.  A
   name too long for a path, or one no mount serves, comes back ok=false with data=NULL.
   Release with fs()->free (safe on a failed read). */

static inline fs_blob_t
gui_res_read( const char* name, const char* ext )
{
    fs_blob_t none = { NULL, 0, false };
    char      path[ RES_PATH_MAX ];
    if ( !name || !res_path( path, sizeof( path ), name, ext ) )
        return none;
    return fs()->read( path );
}

/* The same read over a list of accepted extensions, first hit wins: an image loader that
   takes PNG first and the other stb formats after.  `exts` is NULL-terminated. */

static inline fs_blob_t
gui_res_read_any( const char* name, const char* const* exts )
{
    fs_blob_t b = { NULL, 0, false };
    for ( ; *exts && !b.ok; ++exts ) b = gui_res_read( name, *exts );
    return b;
}

/*============================================================================================*/
#endif    // GUI_RES_H
