/*==============================================================================================

    runtime_service/gui/gui_font.c -- GUI_FONT translation unit: the font resource.

    Loads fonts and holds what the rest of the GUI needs to use one. A font ships as a
    pre-baked ".orb_font" file -- a font tool has already rasterized every character into a
    grid of pixels offline, so this unit does no rendering of its own; it just parses that
    file into two things: METRICS (how wide each character is, how tall a line is -- what
    layout code measures text against) and the raw GLYPH PIXELS for each character.

    This unit never touches the GPU or a texture. It hands the glyph pixels to the render
    unit, which is the one that actually uploads them into the shared atlas texture the GPU
    draws from. Font types and the loader functions other units call are declared in
    font/gui_font.h; this file compiles the font registry (which fonts are currently loaded),
    the metric readers, the .orb_font file parser, and the loader for the engine's built-in
    default fonts.

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
