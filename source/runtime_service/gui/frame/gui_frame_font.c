/*==============================================================================================

    runtime_service/gui/frame/gui_frame_font.c -- Font API + the font -> layout bridge.

    The UI-unit font surface (load / load_builtin / load_into /
    use / push / pop / active_id), the push/pop font stack, the asset-path helper the builtin
    loader resolves through, and gui_style_apply -- the font -> layout bridge that rebuilds the
    scaled layout metrics from the active font whenever a theme, font, or deferred reload lands.

    The font REGISTRY itself lives in the render backend unit; this file drives it through the
    font_load / font_use / font_active_id accessors (render/gui_render.h) and rescales layout
    from the active font's metrics (font_em / font_char_h / font_line_h).  The between-frames
    commit of deferred reloads (gui_font_flush_deferred) stays in gui_frame_loop.c -- it is a
    frame_begin step, not part of this public surface.

    Included by the gui_frame.c unit root next to gui_frame_loop.c; the root supplies every
    header (fmt / sys / the unit seams) before including either constituent.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Font API

    The font registry lives in the render backend unit;

    This UI-unit API drives it through the font_load / font_use accessors (gui_render.h)
    and rebuilds layout from the active font's metrics (font_em / font_char_h / font_line_h)
        -- the font -> layout bridge.
==============================================================================================*/

/* Saved active-font ids for push_font / pop_font; small fixed depth -- font pushes are coarse
   (a section or one widget), not deeply nested. */

#define GUI_FONT_STACK_MAX 8
static u32 s_font_stack[ GUI_FONT_STACK_MAX ];
static u32 s_font_stack_depth = 0;

/* Rebuild layout metrics from whatever font is now active.  A safe no-op before any font has
   activated (font_valid() false) -- s_style just stays at its last computed value (zero-init
   pre-first-font).  Every caller (theme reset, init, font load/use, deferred-reload flush) can
   call this unconditionally and trust it to do the right thing either way. */

void
gui_style_apply( void )
{
    if ( !font_valid() )
        return;

    layout_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h() );

    /* Re-install the element style from the freshly scaled metrics: the registered style
       source (a kit that owns the look -- style_source_set), else the default S2 -> S1 compile
       from the active theme.  Either way el_* elements track every theme / font landing. */
    el_style_install();

    /* A style / theme change restyles every window, but the retained cache only re-tessellates on a
       dirty frame.  Request a redraw so the NEXT frame does a full clean rebuild with the reseeded
       style -- otherwise a change made while the UI is idle (e.g. picking a theme in a style editor,
       then no further input) freezes into a half-restyled cached frame: windows already emitted /
       tessellated this frame keep their old colors and metrics.  Guarded because style_apply also
       runs at init-time font load, before any context exists. */
    if ( g_ctx )
        redraw_request();
}

u32
gui_font_load( const char* path )
{
    u32 id = font_load( path );     // loads into a new id and activates it
    if ( id == 0 )
        return 0;
    gui_style_apply();
    draw_set_font( font_active_id() );   // load also activates -> retag the atlas batch context
    return id;
}

u32
gui_font_load_builtin( gui_builtin_font_t font )
{
    /* The preset enum already knows its asset path -- resolve it and take the normal
       font_load path (new id + activate).  Unlike init's slot-0 preset load, this never
       touches the default font; GUI_FONT_NONE / an unknown preset returns 0. */
    const char* rel = font_builtin_rel_path( font );
    if ( rel == NULL )
        return 0;

    char path[ 576 ];
    gui_asset_path( rel, path, sizeof( path ) );
    return gui_font_load( path );
}

bool
gui_font_load_into( u32 id, const char* path )
{
    /* font_load_into parses the .orb_font into the slot now (metrics live immediately); a slot that
       already shows a font keeps showing its old atlas pixels until the upload lands at the next
       frame_begin (font_atlas_sync), where layout follows via gui_font_flush_deferred.  Nothing to
       rescale here. */
    return font_load_into( id, path );
}

void
gui_asset_path( const char* relative, char* out, int out_size )
{
    fmt_snprintf( out, (size_t)out_size, "%s/%s", sys_root_dir(), relative );
}

void
gui_font_use( u32 id )
{
    font_use( id );
    gui_style_apply();
    /* The active font is also the per-segment atlas batch context: cut a new draw segment so the
       tessellator re-activates this font for the span and its glyphs / fills / dashes sample the
       right atlas.  font_use ignores a bad id, so tag with whatever is actually active now. */
    draw_set_font( font_active_id() );
}

void
gui_push_font( u32 id )
{
    if ( s_font_stack_depth < GUI_FONT_STACK_MAX )
        s_font_stack[ s_font_stack_depth++ ] = font_active_id();
    gui_font_use( id );
}

void
gui_pop_font( void )
{
    if ( s_font_stack_depth == 0 )
        return;
    gui_font_use( s_font_stack[ --s_font_stack_depth ] );
}

u32
gui_font_active_id( void )
{
    return font_active_id();
}

// clang-format on
/*============================================================================================*/
