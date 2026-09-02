/*==============================================================================================

    sandbox/gui/sb_gui_style/st_font.c -- Font Tool window: find / bake / preview a face.

    The FACE half of a look (the Style Editor next door owns the other half).  Unity-included by
    sb_gui_style.c; see st.h for the window contract.

    The bake here is the quick stb one -- dev_font_get(): rasterized at runtime with stb_truetype
    into assets/font_cache/.  Instant, disposable, "I want to see this face at this size".  A
    shippable bake is not made from this window: it is a recipe (content/font/<family>/<size>
    .recipe) the build cooks with font_tool when some image names it with RID().

    Search scope is a checkbox: local only (assets/font/) or Windows too (the OS font registry,
    resolving "Cascadia Mono" -> C:\Windows\Fonts\CascadiaMono.ttf without copying it in).

==============================================================================================*/

// clang-format off

/*============================================================================================*/
/* Font tool state                                                                             */
/*============================================================================================*/

#define FT_LIST_MAX 512
#define FT_NAME_MAX 128

typedef struct
{
    /* Picker listing: assets/font/ files first (indices 0..local_count), then installed faces
       (indices local_count..count, sorted) when allow_windows is on. */
    char names[ FT_LIST_MAX ][ FT_NAME_MAX ];
    int  count;
    int  local_count;        /* number of leading entries that are project (assets/font) fonts */
    int  sel;
    bool scanned;
    bool list_has_windows;   /* scope the current listing was built with (detects toggle) */
    bool list_has_local;     /* ditto, for the assets/font toggle */

    /* Request the user is working on. */
    char request[ FT_NAME_MAX ];   /* font name or filename to resolve */
    bool allow_windows;            /* false = local only; true = also the OS font registry */
    bool allow_local;              /* false = skip assets/font/ in the picker listing */
    i32  size_px;

    /* Live stb preview. */
    u32  preview_id;
    bool preview_ready;
    char preview_name[ FT_NAME_MAX ];
    i32  preview_size;
    char sample_text[ 256 ];

    /* Status line for the bake. */
    char bake_status  [ 256 ];
    bool bake_ok;

    /* Atlas preview toggle. */
    bool show_atlas;
    bool atlas_2x;

} font_tool_t;

static font_tool_t s_ft;

/*============================================================================================*/
/* Resolution honoring the local/Windows scope checkbox.                                       */
/*                                                                                             */
/* dev_font_resolve() always walks assets/font -> system dir -> OS registry.  When the user     */
/* asks for local only we must NOT fall through to Windows, so we probe assets/font/ ourselves */
/* and stop there.  With the box ticked we defer to the full resolver.                         */
/*============================================================================================*/

static bool
ft_resolve( const char* request, bool allow_windows, char* out, int out_size )
{
    if ( allow_windows )
        return dev_font_resolve( request, out, out_size );

    /* Local only: exact name in assets/font/, then the common face extensions. */
    char src[ 512 ];
    if ( !dev_font_source_dir( src, sizeof( src ) ) )
        return false;

    static const char* ext[] = { "", ".ttf", ".otf", ".ttc" };
    for ( int i = 0; i < 4; ++i )
    {
        snprintf( out, (size_t)out_size, "%s" PATH_SEP "%s%s", src, request, ext[ i ] );
        if ( sys_file_time( out ) > 0 )
            return true;
    }
    return false;
}

/*============================================================================================*/
/* Scan assets/font/ for the local picker.                                                     */
/*============================================================================================*/

/* Append one name to the list (both scans share this).  Returns false once the list is full so
   the enumerations stop early. */
static bool
ft_add( const char* name )
{
    if ( s_ft.count >= FT_LIST_MAX )
        return false;
    snprintf( s_ft.names[ s_ft.count++ ], FT_NAME_MAX, "%s", name );
    return s_ft.count < FT_LIST_MAX;
}

/* Case-insensitive name compare for sorting the Windows portion of the list. */

static int
ft_name_cmp( const void* a, const void* b )
{
    const char* x = (const char*)a;
    const char* y = (const char*)b;
    for ( ; *x && *y; ++x, ++y )
    {
        char cx = ( *x >= 'A' && *x <= 'Z' ) ? (char)( *x - 'A' + 'a' ) : *x;
        char cy = ( *y >= 'A' && *y <= 'Z' ) ? (char)( *y - 'A' + 'a' ) : *y;
        if ( cx != cy ) return (unsigned char)cx - (unsigned char)cy;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

/* True if `name` ends in one of the face extensions assets/font/ is scanned for (case-
   insensitive, matching sys_file_glob's Windows FindFirstFile semantics). */

static bool
ft_has_font_ext( const char* name )
{
    static const char* ext[] = { ".ttf", ".otf", ".ttc" };
    size_t n = strlen( name );
    for ( int i = 0; i < 3; ++i )
    {
        size_t el = strlen( ext[ i ] );
        if ( n >= el && ft_name_cmp( name + n - el, ext[ i ] ) == 0 )
            return true;
    }
    return false;
}

/* sys_dir_walk() callback for assets/font/: recurses into subdirectories, since a family's faces
   live in their own folder (assets/font/geist/, mirroring content/font/geist/).  The listed/
   request name is the path relative to assets/font/ (e.g. "geist/Geist-Bold.ttf"), which both
   keeps same-stem files in different folders distinct and is what ft_resolve() and
   dev_font_resolve()'s assets/font-relative fallback expect. */
static bool
ft_local_cb( const char* filename, const char* full_path, void* ud )
{
    const char* root = (const char*)ud;

    if ( !ft_has_font_ext( filename ) )
        return true;   /* keep walking; not a font file */

    const char* rel = full_path;
    size_t      root_len = strlen( root );
    if ( strncmp( full_path, root, root_len ) == 0 )
    {
        rel = full_path + root_len;
        while ( *rel == '\\' || *rel == '/' ) ++rel;
    }
    return ft_add( rel );
}

static bool
ft_windows_cb( const char* name, const char* full_path, void* ud )
{
    UNUSED( full_path );
    UNUSED( ud );
    return ft_add( name );   /* friendly name, e.g. "Cascadia Mono Regular" */
}

static void
ft_scan( void )
{
    s_ft.count = 0;

    /* Local .ttf/.otf/.ttc under assets/font/, recursing into the family folders (listed first,
       sorted), when the scope box is ticked. */
    if ( s_ft.allow_local )
    {
        char src[ 512 ];
        if ( dev_font_source_dir( src, sizeof( src ) ) )
        {
            sys_dir_walk( src, ft_local_cb, src );
            if ( s_ft.count > 0 )
                qsort( s_ft.names[ 0 ], (size_t)s_ft.count, FT_NAME_MAX, ft_name_cmp );
        }
    }
    s_ft.local_count = s_ft.count;   /* everything before here is a project font */

    /* Installed fonts by friendly name, only when the scope box is ticked.  These arrive as two
       per-hive alphabetical runs; sort the whole appended block into one A-Z list so a face is
       easy to find (and does not appear to be missing just because it sits in another run). */
    if ( s_ft.allow_windows )
    {
        sys_font_enumerate( ft_windows_cb, NULL );
        if ( s_ft.count > s_ft.local_count )
            qsort( s_ft.names[ s_ft.local_count ], (size_t)( s_ft.count - s_ft.local_count ),
                   FT_NAME_MAX, ft_name_cmp );
    }

    s_ft.list_has_windows = s_ft.allow_windows;
    s_ft.list_has_local   = s_ft.allow_local;
    s_ft.scanned          = true;
    if ( s_ft.sel >= s_ft.count )
        s_ft.sel = 0;
}

/*============================================================================================*/
/* Action: quick stb bake + live preview (dev_font_get).                                       */
/*============================================================================================*/

static void
ft_bake_preview( void )
{
    if ( !s_ft.request[ 0 ] )
    {
        snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ), "Enter a font name or file." );
        s_ft.bake_ok = false;
        return;
    }

    /* Honor the scope checkbox before handing off to the baker: resolve to an absolute path so
       dev_font_get uses it verbatim (a path with a separator bypasses its internal search). */
    char abs[ 512 ];
    if ( !ft_resolve( s_ft.request, s_ft.allow_windows, abs, sizeof( abs ) ) )
    {
        snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ),
                  s_ft.allow_windows ? "Not found: %s"
                                     : "Not in assets/font/: %s  (tick Windows to search installed fonts)",
                  s_ft.request );
        s_ft.bake_ok = false;
        return;
    }

    /* The bake lives in assets/font_cache, outside the content mounts, so it is handed to gui
       as bytes (font_load_mem) rather than by name. */
    char path[ 512 ];
    if ( !dev_font_get( abs, s_ft.size_px, path, sizeof( path ) ) )
    {
        snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ), "Bake error: %s", dev_font_last_error() );
        s_ft.bake_ok = false;
        return;
    }

    sys_file_data_t bake = sys_file_read_entire( path );
    if ( !bake.ok )
    {
        snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ), "cannot read %s", path );
        s_ft.bake_ok = false;
        return;
    }

    if ( !s_ft.preview_ready )
    {
        u32 id = gui()->font_load_mem( bake.data, bake.size, s_ft.request );
        if ( !id )
        {
            sys_file_free( &bake );
            snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ), "font_load failed for %s", path );
            s_ft.bake_ok = false;
            return;
        }
        s_ft.preview_id    = id;
        s_ft.preview_ready = true;
    }
    else
    {
        gui()->font_load_into_mem( s_ft.preview_id, bake.data, bake.size, s_ft.request );
    }
    sys_file_free( &bake );

    snprintf( s_ft.preview_name, sizeof( s_ft.preview_name ), "%s", s_ft.request );
    s_ft.preview_size = s_ft.size_px;
    snprintf( s_ft.bake_status, sizeof( s_ft.bake_status ),
              "stb baked: %s at %d px -> %s", s_ft.request, s_ft.size_px, path );
    s_ft.bake_ok = true;
}

/*============================================================================================*/
/* Window                                                                                      */
/*============================================================================================*/

static void
st_font_window( void )
{
    /* Lazy init on first show. */
    if ( !s_ft.scanned )
    {
        s_ft.size_px       = 16;
        s_ft.allow_windows = true;
        s_ft.allow_local   = true;
        snprintf( s_ft.sample_text, sizeof( s_ft.sample_text ),
                  "The quick brown fox jumps over the lazy dog." );
        ft_scan();
        if ( s_ft.count > 0 )
            snprintf( s_ft.request, sizeof( s_ft.request ), "%s", s_ft.names[ 0 ] );
    }

    if ( !st_begin( "Font Tool", 560.0f, 560.0f ) )
    {
        gui()->window_end();
        return;
    }
    
    gui()->stack();
    
    /* --- Source --------------------------------------------------------------- */
    gui()->separator_text( "Source" );
    
    gui()->text( "Font name or file" );
    gui()->input_text( "##request", s_ft.request, sizeof( s_ft.request ) );
    
    gui()->checkbox( "Include internal fonts", &s_ft.allow_local );
    gui()->same_line( -1 );
    gui()->help_marker( "On:  list assets/font/ (the raw faces behind content/font/) in the picker.\n"
                        "Off: skip them -- handy for browsing/testing the Windows registry alone." );

    gui()->checkbox( "Include Windows fonts", &s_ft.allow_windows );
    gui()->same_line( -1 );
    gui()->help_marker( "Off: list/search assets/font/ only.\n"
                        "On:  also list installed fonts by friendly name from the OS font registry\n"
                        "     (e.g. \"Cascadia Mono\" -> C:\\Windows\\Fonts\\CascadiaMono.ttf)." );
    
    /* Either checkbox governs the picker's scope -- rebuild the list when either flips. */
    if ( s_ft.list_has_windows != s_ft.allow_windows || s_ft.list_has_local != s_ft.allow_local )
        ft_scan();
    
    /* Local picker: choosing a file copies its name into the request field above. */
    const char* combo_label = ( s_ft.count > 0 ) ? s_ft.names[ s_ft.sel ] : "(no local fonts)";
    
    if ( gui()->combo_begin( "##local", combo_label, GUI_COMBO_NONE ) )
    {
        gui()->scale_push( GUI_SCALE_DENSE );
        for ( int i = 0; i < s_ft.count; i++ )
        {
            /* Project (assets/font) fonts lead the list in a distinct tint; installed Windows
               faces follow in default color. */
            bool project = ( i < s_ft.local_count );
            if ( project )
                gui()->push_style_color( GUI_ROLE_TEXT_PRIMARY, GUI_PHASE_ALL, GUI_COLOR( 0x7C, 0xD9, 0x92, 0xFF ) );
    
            bool sel = ( i == s_ft.sel );
            if ( gui()->selectable( s_ft.names[ i ], &sel ) )
            {
                s_ft.sel = i;
                snprintf( s_ft.request, sizeof( s_ft.request ), "%s", s_ft.names[ i ] );
            }
    
            if ( project )
                gui()->pop_style_color( 1 );
        }
        gui()->scale_pop();
        gui()->combo_end();
    }
        
    // gui()->same_line( -1 );
    if ( gui()->small_button( "Refresh" ) )
        ft_scan();
    
    gui()->slider_int( "Size (px)", &s_ft.size_px, 6, 72, NULL );
    
    /* --- Actions -------------------------------------------------------------- */
    gui()->separator_text( "Bake" );

    /* button_fill is NOT used here -- it sizes its HEIGHT to the view bottom, which feeds back
       into the scroll extent when content follows it (status + preview below), growing the
       region without bound every scrolled frame. */
    if ( gui()->button( "Bake & Preview (stb)" ) )
        ft_bake_preview();
    gui()->same_line( -1 );
    gui()->help_marker( "A quick stb bake into assets/font_cache/ for previewing.  A shipped bake is a\n"
                        "recipe under content/font/<family>/ that the build cooks when an image names it." );

    gui()->stack();
    if ( s_ft.bake_status[ 0 ] )
    {
        if ( s_ft.bake_ok ) gui()->text_disabled( s_ft.bake_status );
        else                gui()->text_colored( GUI_COLOR( 0xFF, 0x60, 0x60, 0xFF ), s_ft.bake_status );
    }
    
    /* --- Preview -------------------------------------------------------------- */
    if ( s_ft.preview_ready )
    {
        gui()->separator_text( "Preview" );
        gui()->input_text_with_hint( "##sample", "Custom preview text...",
                                     s_ft.sample_text, sizeof( s_ft.sample_text ) );
        gui()->new_line( -1.0f );
    
        /* NOTE -- the renderer resolves glyphs from ONE global active font at tessellation time,
           so push_font here recolors the WHOLE frame's text, not just these lines.  For a font
           bench that is the point (you see the face applied everywhere); a truly isolated preview
           would need a separate texture path the single-global-font model does not offer. */
        gui()->push_font( s_ft.preview_id );
        gui()->stack();
        if ( s_ft.sample_text[ 0 ] )
            gui()->text( s_ft.sample_text );
        gui()->text( "ABCDEFGHIJKLMNOPQRSTUVWXYZ" );
        gui()->text( "abcdefghijklmnopqrstuvwxyz" );
        gui()->text( "0123456789  !@#$%^&*()-+=[]{};:" );
        gui()->pop_font();
    
        gui()->separator_text( "Apply" );
        gui()->textf( "Live: %s  %d px", s_ft.preview_name, s_ft.preview_size );
        if ( gui()->button( "Use as UI font" ) )
             gui()->font_use( s_ft.preview_id );
    }
    
    /* --- Atlas ------------------------------------------------------------------ */
    /* Independent of the bake/preview flow above -- shows the CURRENTLY ACTIVE font's atlas
       (whatever the app booted with, or last font_use'd via "Use as UI font"), so the controls are
       there from the first frame instead of only appearing after a bake.  NOTE -- the atlas is an
       R8_UNORM coverage texture, but image_texture samples it via the RGBA path (texel.rgb as
       color): only the red channel carries data, so glyph ink renders red-on-black rather than
       white-on-black.  Fine for judging packing (ink vs gap is still obvious); a true grayscale
       view would need a dedicated coverage-sampling draw path. */

    gui()->separator_text( "Atlas" );
    if ( gui()->button( "Show Atlas" ) )
        s_ft.show_atlas = !s_ft.show_atlas;
    gui()->same_line( -1 );
    gui()->checkbox( "2x", &s_ft.atlas_2x );
    
    if ( s_ft.show_atlas )
    {
        u32 active_id = gui()->font_active_id();
        u32 atlas_idx = gui()->font_atlas_idx( active_id );
        if ( atlas_idx )
        {
            gui_vec2_t asz = gui()->font_atlas_size( active_id );
    
            gui()->textf( "Active font #%u -- %.0f x %.0f px  (bindless #%u)",
                          active_id, asz.x, asz.y, atlas_idx );
    
            /* Native resolution (or 2x via the checkbox) -- no fit-to-window scaling, so packing/
               coverage reads exactly as baked. */
            f32 scale = s_ft.atlas_2x ? 2.0f : 1.0f;
            gui()->image_texture( atlas_idx, asz.x * scale, asz.y * scale, 0 );
        }
        else
        {
            gui()->text_disabled( "No active font atlas." );
        }
    }

    gui()->window_end();
}

// clang-format on
