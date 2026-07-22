/*==============================================================================================

    runtime_service/gui/gui_text.c -- GUI_TEXT translation unit: the leaf font-metrics library.

    * A foundational leaf of the gui stack, beside rect: text measurement is sizes-and-math, not
      drawing, so the loaded-font metrics live here where BOTH servers and every layer can read them.
    * No draw, no atlas, no GPU -- this unit never touches a render resource.  The glyph DRAWING
      half (atlas pixels, UV dispatch, the .orb_font loader) stays render-side.
    * The types + reader surface are text/gui_text.h; this unit compiles the registry + readers.

==============================================================================================*/

#include <stdio.h>    /* printf  -- font_print_active                 */
#include <string.h>   /* memset  -- font_registry_reset               */

#include "orb.h"

#include "runtime_service/gui/text/gui_text.h"
#include "runtime_service/gui/text/gui_text_core.c"    // registry + metric readers

/*============================================================================================*/
