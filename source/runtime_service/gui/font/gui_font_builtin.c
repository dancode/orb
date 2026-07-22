/*==============================================================================================

    runtime_service/gui/font/gui_font_builtin.c -- built-in font presets.

    A convenience over font_load_into: each gui_builtin_font_t (gui.h) names a baked .orb_font asset
    shipped with the engine; this resolves the preset to its root-relative path and loads it into the
    default slot.  Picking a file and loading it is resource work, so it lives with the resource, not
    render-side -- no atlas, no GPU.

    Compiled into the gui_font.c resource unit; included after font/gui_font_load.c.

==============================================================================================*/
// clang-format off

#include "base/fmt.h"                // fmt_snprintf -- build the preset's absolute path
#include "engine/sys/sys_host.h"     // sys_root_dir -- presets resolve root-relative

/* Path of every gui_builtin_font_t preset (gui.h), indexed by the enum; NULL for GUI_FONT_NONE. */

static const char* s_builtin_font_path[] =
{
    [ GUI_FONT_NONE ]               = NULL,
    [ GUI_FONT_JETBRAINS_16 ]       = "assets/font/JetBrainsMonoNL-Regular_16px.orb_font",
    [ GUI_FONT_ROBOTO_16 ]          = "assets/font/Roboto-Regular_16px.orb_font",
    [ GUI_FONT_CASCADIA_MONO_12 ]   = "assets/font/CascadiaMono_12px.orb_font",
    [ GUI_FONT_CASCADIA_MONO_16 ]   = "assets/font/CascadiaMono_16px.orb_font",
    [ GUI_FONT_CASCADIA_MONO_20 ]   = "assets/font/CascadiaMono_20px.orb_font",
    [ GUI_FONT_CASCADIA_CODE_16 ]   = "assets/font/CascadiaCode_16px.orb_font",
};

/* Relative asset path of a built-in preset; NULL for GUI_FONT_NONE / out-of-range. */

const char*
font_builtin_rel_path( gui_builtin_font_t font )
{
    if ( font >= ARRAY_COUNT( s_builtin_font_path ) )
        return NULL;
    return s_builtin_font_path[ font ];
}

/* Load a built-in font preset into slot 0 and activate it.  A no-op success for GUI_FONT_NONE.
   Called from gui_init() when the host passes a preset. */

bool
font_load_builtin( gui_builtin_font_t font )
{
    if ( font == GUI_FONT_NONE )
        return true;

    const char* rel = font_builtin_rel_path( font );
    if ( rel != NULL )
    {
        /* Built-in presets are engine assets at <root>/assets/font -- resolve against sys_root_dir()
           so hosts work from any working directory. */
        char path[ 576 ];
        fmt_snprintf( path, sizeof( path ), "%s/%s", sys_root_dir(), rel );

        return font_load_into( 0, path );   // slot 0 = the default font
    }

    return false;
}

// clang-format on
/*============================================================================================*/
