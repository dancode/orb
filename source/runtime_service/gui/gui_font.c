/*==============================================================================================

    runtime_service/gui/gui_font.c -- GUI_FONT translation unit: the font resource.

    * A low-level resource the GUI depends on, beside rect -- not a GUI feature: it draws nothing
      and holds no widgets.  It parses a baked .orb_font into the two things the GUI needs: the
      TYPE METRICS layout measures against, and the raw R8 GLYPH PIXELS the render atlas uploads.
    * No atlas, no GPU: this unit never touches a render resource.  The render side is the one
      consumer of the pixels -- it reads a slot's pixels and packs them into the shared atlas.
    * Types + reader/loader surface are font/gui_font.h; this unit compiles the registry, the
      metric readers, the .orb_font parser, and the built-in preset loader.

==============================================================================================*/

#include <stdio.h>    /* fopen/fread/printf                          */
#include <stdlib.h>   /* malloc/free -- resident glyph pixels        */
#include <string.h>   /* memset/memcpy                               */

#include "orb.h"
#include "base/utf8.h"   /* codepoint stepping in font_text_w_n's measure loop */

#include "runtime_service/gui/font/gui_font.h"
#include "runtime_service/gui/font/gui_font_core.c"       // registry + metric readers
#include "runtime_service/gui/font/gui_font_load.c"       // .orb_font parser + load API
#include "runtime_service/gui/font/gui_font_builtin.c"    // built-in preset -> load

/*============================================================================================*/
