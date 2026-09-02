/*==============================================================================================

    engine/res/res_cook.h - Source-to-cooked file classification shared by the offline tools.

    A logical name is a path under a content root minus the extension; the source extension is
    what says which cooker runs and what the cooked file is called.  Every tool that turns a
    source file into a cooked one -- asset_tool's tree cook today, the packager's ship cook
    later -- reads the mapping from here, so the file a loader asks fs for (name plus the
    extension it accepts) is the file the cooker wrote.

    Header-only, tools only.  Not part of the runtime res library and never included by it.

==============================================================================================*/
#ifndef RES_COOK_H
#define RES_COOK_H

#include <string.h>

#include "orb.h"

// clang-format off
/*==============================================================================================
    Kinds

    RES_KIND_RECIPE is content with no source file of its own (a font bake at a size): a small
    text file whose "kind" line names the kind it cooks to, so a recipe's cooked extension comes
    from res_kind_from_name over that word rather than from the file's own extension.
==============================================================================================*/

typedef enum res_kind_e
{
    RES_KIND_COPY = 0,      // no cooker: ships verbatim under its own extension
    RES_KIND_IMAGE,         // png/jpg/jpeg/bmp/tga/psd/gif/hdr  ->  .tex (RGBA8, asset_tex.h)
    RES_KIND_FONT,          // ttf/otf                            ->  .orb_font (font_tool bake)
    RES_KIND_SHADER,        // hlsl                               ->  .oshd (shader_tool cook)
    RES_KIND_RECIPE,        // recipe                             ->  whatever its "kind" line says

} res_kind_t;

#define RES_RECIPE_EXT    "recipe"                     // the recipe file extension, no dot

/* Case-insensitive compare of an extension (no leading dot) against a lowercase literal. */
static inline bool
res_ext_is( const char* ext, const char* want )
{
    while ( *ext && *want )
    {
        char a = *ext++;
        if ( a >= 'A' && a <= 'Z' ) a = ( char )( a + 32 );
        if ( a != *want++ ) return false;
    }
    return *ext == 0 && *want == 0;
}

/* Kind of a source file from its extension (no leading dot, any case). */
static inline res_kind_t
res_kind_from_ext( const char* ext )
{
    if ( res_ext_is( ext, "ttf" ) || res_ext_is( ext, "otf" ) )
        return RES_KIND_FONT;
    if ( res_ext_is( ext, "png" ) || res_ext_is( ext, "jpg" ) || res_ext_is( ext, "jpeg" ) ||
         res_ext_is( ext, "bmp" ) || res_ext_is( ext, "tga" ) || res_ext_is( ext, "psd" ) ||
         res_ext_is( ext, "gif" ) || res_ext_is( ext, "hdr" ) )
        return RES_KIND_IMAGE;
    if ( res_ext_is( ext, "hlsl" ) )                    /* .hlsli headers copy verbatim */
        return RES_KIND_SHADER;
    if ( res_ext_is( ext, RES_RECIPE_EXT ) )
        return RES_KIND_RECIPE;
    return RES_KIND_COPY;
}

/* Kind named by a recipe's "kind" word.  Only kinds that produce a cooked file are recipe
   targets; anything else (including "copy" and "recipe") is a malformed recipe. */
static inline res_kind_t
res_kind_from_name( const char* word )
{
    if ( strcmp( word, "font"   ) == 0 ) return RES_KIND_FONT;
    if ( strcmp( word, "image"  ) == 0 ) return RES_KIND_IMAGE;
    if ( strcmp( word, "shader" ) == 0 ) return RES_KIND_SHADER;
    return RES_KIND_COPY;
}

/* Extension (no dot) of the cooked file a kind produces; NULL when the source keeps its own
   extension (RES_KIND_COPY) or when the kind cannot answer alone (RES_KIND_RECIPE). */
static inline const char*
res_kind_cooked_ext( res_kind_t kind )
{
    switch ( kind )
    {
        case RES_KIND_IMAGE:  return "tex";
        case RES_KIND_FONT:   return "orb_font";
        case RES_KIND_SHADER: return "oshd";
        default:              return NULL;
    }
}

// clang-format on
/*============================================================================================*/
#endif    // RES_COOK_H
