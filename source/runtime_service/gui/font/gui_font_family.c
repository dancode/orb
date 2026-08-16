/*==============================================================================================

    runtime_service/gui/font/gui_font_family.c -- curated font family identities.

    The family table is the resolver's identity source: per curated family (gui_font_family_t,
    gui.h) it carries the runtime baker request name and the shipped .orb_font filename stem.
    Sizes are requested at resolve time, never enumerated here -- the resolver
    (frame/gui_frame_resolve.c) composes these strings with a pixel size to find or mint a
    bake.

    Compiled into the gui_font.c resource unit; included after font/gui_font_load.c.

==============================================================================================*/
// clang-format off

typedef struct
{
    const char* bake_source;  // runtime baker request name (a file in assets/font_source, or an
                              // OS-installed friendly name); NULL = family has no runtime source
    const char* ship_stem;    // shipped .orb_font filename stem under assets/font/

} font_family_info_t;

static const font_family_info_t s_family[ GUI_FONT_FAMILY_COUNT ] =
{
    [ GUI_FONT_NONE ]          = { NULL,                NULL                      },
    [ GUI_FONT_JETBRAINS ]     = { "JetBrains Mono NL", "JetBrainsMonoNL-Regular" },
    [ GUI_FONT_ROBOTO ]        = { "Roboto-Regular",    "Roboto-Regular"          },
    [ GUI_FONT_CASCADIA_MONO ] = { "Cascadia Mono",     "CascadiaMono"            },
    [ GUI_FONT_CASCADIA_CODE ] = { "Cascadia Code",     "CascadiaCode"            },
};

const char*
font_family_bake_source( gui_font_family_t fam )
{
    if ( fam >= GUI_FONT_FAMILY_COUNT )
        return NULL;
    return s_family[ fam ].bake_source;
}

const char*
font_family_ship_stem( gui_font_family_t fam )
{
    if ( fam >= GUI_FONT_FAMILY_COUNT )
        return NULL;
    return s_family[ fam ].ship_stem;
}

// clang-format on
/*============================================================================================*/
