/*==============================================================================================

    runtime_service/gui/font/gui_font_family.c -- the typeface behind a family directory.

    A font's family is the directory its bakes live in under content/font ("cascadiamono").
    The resolver (frame/gui_frame_resolve.c) composes "font/<family>/<size>" from it and reads
    the cooked bake through fs; only when no bake answers does it ask the runtime baker, and
    the baker needs the TYPEFACE -- "Cascadia Mono", the OS-installed face, or a TTF under
    source_content/font -- not the directory.  That spelling is content, the family's
    descriptor:

        content/font/<family>/family.txt        face <font_tool input>

    the same file the family's recipes take their face from when the build cooks them, so a
    runtime bake and a cooked bake of one family come from one spelling.  It is read through
    the mounts like every other content byte (gui_res.h), never composed through RID(): a
    shipped build carries no baker and never asks.  A family with no descriptor passes
    through as its own face name, which is right for a directory named after something
    dev_font resolves directly ("consolas").

    Compiled into the gui_font.c resource unit; included after font/gui_font_load.c.

==============================================================================================*/
// clang-format off

/* Copy the "face" value out of a family.txt blob: "<key> <value>" lines, '#' comments.  False
   when no face line is present. */
static bool
family_face_scan( const char* data, u32 size, char* out, u32 cap )
{
    u32 i = 0;
    while ( i < size )
    {
        /* one line */
        u32 s = i;
        while ( i < size && data[ i ] != '\n' )
            ++i;
        u32 e = i;
        if ( i < size )
            ++i;    /* past the '\n' */

        for ( u32 k = s; k < e; ++k )
            if ( data[ k ] == '#' ) { e = k; break; }
        while ( s < e && ( data[ s ] == ' ' || data[ s ] == '\t' || data[ s ] == '\r' ) )
            ++s;
        while ( e > s && ( data[ e - 1 ] == ' ' || data[ e - 1 ] == '\t' || data[ e - 1 ] == '\r' ) )
            --e;
        if ( e - s < 5 || memcmp( data + s, "face", 4 ) != 0 || ( data[ s + 4 ] != ' ' && data[ s + 4 ] != '\t' ) )
            continue;

        u32 v = s + 4;
        while ( v < e && ( data[ v ] == ' ' || data[ v ] == '\t' ) )
            ++v;
        if ( v >= e )
            continue;
        u32 n = e - v;
        if ( n >= cap )
            n = cap - 1;
        memcpy( out, data + v, n );
        out[ n ] = 0;
        return true;
    }
    return false;
}

void
font_family_face( const char* family, char* out, u32 cap )
{
    if ( cap == 0 )
        return;
    snprintf( out, (int)cap, "%s", family ? family : "" );
    if ( !family || !*family )
        return;

    char name[ RES_NAME_MAX + 1 ];
    int  n = snprintf( name, (int)sizeof( name ), "font/%s/family", family );
    if ( n <= 0 || n >= (int)sizeof( name ) )
        return;

    fs_blob_t b = gui_res_read( name, ".txt" );
    if ( b.ok && family_face_scan( (const char*)b.data, b.size, out, cap ) )
        gui_log( GUI_LOG_INFO, "font family '%s' bakes from '%s' (%s.txt)", family, out, name );
    fs()->free( &b );    /* no descriptor or no face line: the family name stays as the request */
}

// clang-format on
/*============================================================================================*/
