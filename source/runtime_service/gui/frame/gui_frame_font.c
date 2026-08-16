/*==============================================================================================

    runtime_service/gui/frame/gui_frame_font.c -- Font API + the font -> layout bridge.

    The PUBLIC font surface (load / load_builtin / load_into / use / push / pop / active_id),
    the push/pop font stack, the asset-path helper the builtin loader resolves through, and
    gui_style_apply -- the font -> layout bridge that rebuilds the scaled layout metrics from
    the active font whenever a theme, font, or deferred reload lands.

    Nothing about a font is stored here.  The registry and the parse are the font/ leaf
    (font/gui_font.h), the atlas upload is the draw unit (draw/gui_glyph.c); this file only
    orders those around and, on every landing, re-derives layout from the active font's metrics
    (font_em / font_char_h / font_line_h).  That last step is the reason the surface sits in the
    orchestrator rather than in font/: a font change is a LAYOUT event, and only this unit sees
    both sides of it.

    The between-frames commit of deferred reloads (gui_font_flush_deferred) stays in
    gui_frame_loop.c -- it is a frame_begin step, not part of this public surface.

    Included by the gui_frame.c unit root next to gui_frame_loop.c; the root supplies every
    header (fmt / sys / the unit seams) before including either constituent.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Font API
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

static f32 dpi_scale_landed( void );   /* gui_frame_dpi.c (next in the TU): scale of the bake being applied */

void
gui_style_apply( void )
{
    if ( !font_valid() )
        return;

    /* The DPI factor scales only the grid quantum inside -- metrics scale from em, and under
       font-driven DPI the em already carries the monitor scale.  The LANDED bake's scale, not
       the primary viewport's: mid-frame the landing walks surfaces at different scales. */
    metrics_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h(), dpi_scale_landed() );

    /* A style LANDING: every style block re-derives its installed values from the freshly
       scaled metrics -- the color grid through the theme compile plus whatever a
       registered style source overwrites (a kit that owns the look -- style_source_set), the
       rest from the active theme.  This is how style tracks a theme / font change. */
    style_landing();

    /* A style / theme change restyles every window, but the retained cache only re-tessellates on a
       dirty frame.  Request a redraw so the NEXT frame does a full clean rebuild with the reseeded
       style -- otherwise a change made while the UI is idle (e.g. picking a theme in a style editor,
       then no further input) freezes into a half-restyled cached frame: windows already emitted /
       tessellated this frame keep their old colors and metrics.  Guarded because style_apply also
       runs at init-time font load, before any context exists. */
    if ( g_ctx )
        redraw_request();

    /* The landed style or font moved -- re-aim the type ramp's role fonts at the new em/step. */
    gui_type_resolve();
}

void
gui_asset_path( const char* relative, char* out, int out_size )
{
    fmt_snprintf( out, (size_t)out_size, "%s/%s", sys_root_dir(), relative );
}

u32
gui_font_load( const char* path )
{
    u32 id = font_load( path );     // loads into a new id and activates it
    if ( id == 0 )
        return 0;
    gui_style_apply();
    draw_set_font( font_active_id() );   // load also activates -> stamp it onto subsequent text
    return id;
}

/* Resolve a font by request -- family + pixel size -- WITHOUT activating it.  The resolver
   finds a shipped bake, asks the installed baker, or degrades to the nearest in-family size
   (warn-once); apply the id with font_use / push_font.  Retention is IMMEDIATE-MODE: call
   this every frame the font is in use (steady-state it is a memo probe), and the request
   itself is the hold.  A font not requested for an emitted frame goes stale and its slot may
   be reclaimed under registry pressure -- do NOT park the id in a static and stop asking;
   re-requesting after a lapse just reloads from the bake cache.  Never 0-fails into nothing:
   worst case the answer is the default font, id 0. */

u32
gui_font_get( const char* family, u32 size_px )
{
    u32 landed = 0;
    return font_resolve( GUI_FONT_NONE, family, size_px, true, &landed );
}

u32
gui_font_get_builtin( gui_font_family_t fam, u32 size_px )
{
    u32 landed = 0;
    return font_resolve( fam, NULL, size_px, true, &landed );
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
gui_font_use( u32 id )
{
    font_use( id );
    gui_style_apply();
    /* Tell the emit layer which font to stamp onto subsequent TEXT commands.  Cuts no segment --
       the font selects glyph metrics and atlas UVs, not a draw batch (see draw_set_font).
       font_use ignores a bad id, so tag with whatever is actually active now. */
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
