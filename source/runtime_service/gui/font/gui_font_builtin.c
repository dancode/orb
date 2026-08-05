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

/* Every gui_builtin_font_t preset (gui.h), indexed by the enum.  family groups the bakes of one
   typeface so DPI retargeting (font_builtin_pick) can trade the active preset for the same face
   at a size nearer the wanted scale; size is the baked glyph height in px (the .orb_font header's
   font_size, mirrored here so picking needs no file I/O). */

typedef enum
{
    FONT_FAM_NONE = 0,
    FONT_FAM_JETBRAINS,
    FONT_FAM_ROBOTO,
    FONT_FAM_CASCADIA_MONO,
    FONT_FAM_CASCADIA_CODE,

} font_family_t;

typedef struct builtin_font_info_s
{
    const char* rel;    // asset path relative to the engine root; NULL for GUI_FONT_NONE
    u8          family; // font_family_t -- bakes of one typeface share a value
    u8          size;   // baked glyph height, px (must match the file's header)

} builtin_font_info_t;

static const builtin_font_info_t s_builtin_font[ GUI_FONT_BUILTIN_COUNT ] =
{
    [ GUI_FONT_NONE ]               = { NULL,                                                FONT_FAM_NONE,          0  },
    [ GUI_FONT_JETBRAINS_12 ]       = { "assets/font/JetBrainsMonoNL-Regular_12px.orb_font", FONT_FAM_JETBRAINS,     12 },
    [ GUI_FONT_JETBRAINS_16 ]       = { "assets/font/JetBrainsMonoNL-Regular_16px.orb_font", FONT_FAM_JETBRAINS,     16 },
    [ GUI_FONT_JETBRAINS_20 ]       = { "assets/font/JetBrainsMonoNL-Regular_20px.orb_font", FONT_FAM_JETBRAINS,     20 },
    [ GUI_FONT_JETBRAINS_24 ]       = { "assets/font/JetBrainsMonoNL-Regular_24px.orb_font", FONT_FAM_JETBRAINS,     24 },
    [ GUI_FONT_ROBOTO_12 ]          = { "assets/font/Roboto-Regular_12px.orb_font",          FONT_FAM_ROBOTO,        12 },
    [ GUI_FONT_ROBOTO_16 ]          = { "assets/font/Roboto-Regular_16px.orb_font",          FONT_FAM_ROBOTO,        16 },
    [ GUI_FONT_ROBOTO_20 ]          = { "assets/font/Roboto-Regular_20px.orb_font",          FONT_FAM_ROBOTO,        20 },
    [ GUI_FONT_ROBOTO_24 ]          = { "assets/font/Roboto-Regular_24px.orb_font",          FONT_FAM_ROBOTO,        24 },
    [ GUI_FONT_CASCADIA_MONO_12 ]   = { "assets/font/CascadiaMono_12px.orb_font",            FONT_FAM_CASCADIA_MONO, 12 },
    [ GUI_FONT_CASCADIA_MONO_16 ]   = { "assets/font/CascadiaMono_16px.orb_font",            FONT_FAM_CASCADIA_MONO, 16 },
    [ GUI_FONT_CASCADIA_MONO_20 ]   = { "assets/font/CascadiaMono_20px.orb_font",            FONT_FAM_CASCADIA_MONO, 20 },
    [ GUI_FONT_CASCADIA_MONO_24 ]   = { "assets/font/CascadiaMono_24px.orb_font",            FONT_FAM_CASCADIA_MONO, 24 },
    [ GUI_FONT_CASCADIA_MONO_32 ]   = { "assets/font/CascadiaMono_32px.orb_font",            FONT_FAM_CASCADIA_MONO, 32 },
    [ GUI_FONT_CASCADIA_CODE_16 ]   = { "assets/font/CascadiaCode_16px.orb_font",            FONT_FAM_CASCADIA_CODE, 16 },
};

/* Relative asset path of a built-in preset; NULL for GUI_FONT_NONE / out-of-range. */

const char*
font_builtin_rel_path( gui_builtin_font_t font )
{
    if ( font >= GUI_FONT_BUILTIN_COUNT )
        return NULL;
    return s_builtin_font[ font ].rel;
}

/* Baked glyph height of a preset in px; 0 for GUI_FONT_NONE / out-of-range. */

u32
font_builtin_size( gui_builtin_font_t font )
{
    if ( font >= GUI_FONT_BUILTIN_COUNT )
        return 0;
    return s_builtin_font[ font ].size;
}

/* |a - b| -- the size distance font_builtin_pick minimizes.  Spelled out because this leaf unit
   pulls in no math header for one absolute value. */

static f32
size_dist( f32 a, f32 b ) { return a > b ? a - b : b - a; }

/* The preset in `base`'s family whose baked size lands nearest base_size * scale -- the DPI
   retarget pick.  Returns `base` itself when its size already fits best, when the family ships no
   other bake, or when `base` carries no family (GUI_FONT_NONE / out-of-range).  Ties break toward
   the LARGER bake: text a step too big stays readable, a step too small does not. */

gui_builtin_font_t
font_builtin_pick( gui_builtin_font_t base, f32 scale )
{
    if ( base >= GUI_FONT_BUILTIN_COUNT )
        return base;

    const builtin_font_info_t* b = &s_builtin_font[ base ];
    if ( b->family == FONT_FAM_NONE || b->size == 0 )
        return base;

    f32                want   = (f32)b->size * scale;
    gui_builtin_font_t best   = base;
    f32                best_d = size_dist( want, (f32)b->size );

    for ( u32 i = 1; i < GUI_FONT_BUILTIN_COUNT; ++i )
    {
        const builtin_font_info_t* c = &s_builtin_font[ i ];
        if ( c->family != b->family )
            continue;

        f32 d = size_dist( want, (f32)c->size );
        if ( d < best_d || ( d == best_d && c->size > s_builtin_font[ best ].size ) )
        {
            best   = (gui_builtin_font_t)i;
            best_d = d;
        }
    }
    return best;
}

/* Load a built-in font preset into slot 0 and activate it.  A no-op success for GUI_FONT_NONE.
   Called from gui_init() when the host passes a preset. */

bool
font_load_builtin( gui_builtin_font_t font )
{
    if ( font == GUI_FONT_NONE )
        return true;

    const char* rel = font_builtin_rel_path( font );
    if ( !rel )
        return false;

    /* Built-in presets are engine assets at <root>/assets/font -- resolve against sys_root_dir()
       so hosts work from any working directory. */
    char path[ 576 ];
    fmt_snprintf( path, sizeof( path ), "%s/%s", sys_root_dir(), rel );

    return font_load_into( 0, path );   // slot 0 = the default font
}

// clang-format on
/*============================================================================================*/
