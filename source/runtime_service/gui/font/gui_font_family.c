/*==============================================================================================

    runtime_service/gui/font/gui_font_family.c -- family directory -> typeface for the baker.

    A font's family is the directory its bakes live in under content/font ("cascadiamono").
    The resolver (frame/gui_frame_resolve.c) composes "font/<family>/<size>" from it and reads
    the cooked bake through fs; only when no bake answers does it ask the runtime baker, and
    the baker needs the TYPEFACE -- "Cascadia Mono", the OS-installed face, or a TTF under
    assets/font_source -- not the directory.  This table is that mapping for the curated
    families.  A family not listed passes through as its own face name, which is right for a
    directory named after something dev_font resolves directly ("consolas").

    Compiled into the gui_font.c resource unit; included after font/gui_font_load.c.

==============================================================================================*/
// clang-format off

typedef struct
{
    const char* family;   // directory under content/font
    const char* face;     // runtime baker request: an OS face name or a TTF under assets/font_source

} font_family_face_t;

static const font_family_face_t s_family_face[] =
{
    { "jetbrains",    "JetBrains Mono NL" },
    { "roboto",       "Roboto-Regular"    },
    { "cascadiamono", "Cascadia Mono"     },
    { "cascadiacode", "Cascadia Code"     },
};

const char*
font_family_face( const char* family )
{
    if ( !family )
        return "";
    for ( u32 i = 0; i < ARRAY_COUNT( s_family_face ); ++i )
        if ( strcmp( s_family_face[ i ].family, family ) == 0 )
            return s_family_face[ i ].face;
    return family;
}

// clang-format on
/*============================================================================================*/
