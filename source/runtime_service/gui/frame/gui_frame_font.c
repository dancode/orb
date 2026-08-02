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

void
gui_style_apply( void )
{
    if ( !font_valid() )
        return;

    metrics_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h() );

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

/*==============================================================================================
    DPI response -- monitor scale -> font retarget.

    The engine works in physical pixels end to end, so gui scales by swapping the ACTIVE FONT
    for a bake of the same family nearer the wanted size: a bigger bake raises em, and every
    layout metric already rescales from em (metrics_compute).  No second scale factor exists
    anywhere -- the response granularity is the set of baked sizes the family ships.

    Managed only while the host's init() preset lineage is active: gui_dpi_poll retargets when
    the active font is the one it last activated (slot 0 at init).  A host that font_use()s /
    font_load()s its own font takes over; retargeting resumes when the managed font is active
    again.  GUI_FONT_NONE at init disables the mechanism entirely.
==============================================================================================*/

static struct
{
    gui_dpi_mode_t     mode;                                // OFF / AUTO / MANUAL
    f32                manual;                              // MANUAL-mode factor
    gui_builtin_font_t base;                                // init() preset; NONE = unmanaged
    gui_builtin_font_t applied;                             // bake currently in effect
    u32                applied_id;                          // font id holding `applied` (0 = init slot)
    u32                loaded_id[ GUI_FONT_BUILTIN_COUNT ]; // preset -> loaded font id (0 = not yet)

} s_dpi = { .mode = GUI_DPI_AUTO, .manual = 1.0f };

/* Seed the managed lineage after init() loads the host's preset into slot 0. */

void
gui_dpi_base_set( gui_builtin_font_t font )
{
    s_dpi.base       = font;
    s_dpi.applied    = font;
    s_dpi.applied_id = 0;
    for ( u32 i = 0; i < GUI_FONT_BUILTIN_COUNT; ++i )
        s_dpi.loaded_id[ i ] = 0;
}

void
gui_dpi_set( gui_dpi_mode_t mode, f32 scale )
{
    s_dpi.mode = mode;
    if ( scale > 0.0f )
        s_dpi.manual = scale;
}

gui_dpi_mode_t
gui_dpi_mode( void )
{
    return s_dpi.mode;
}

/* The scale actually in effect: applied bake size over the init() preset's size.  1.0 while
   unmanaged -- the wanted monitor scale is app()->window_dpi_scale; this is what the UI IS. */

f32
gui_dpi_scale( void )
{
    u32 base = font_builtin_size( s_dpi.base );
    u32 now  = font_builtin_size( s_dpi.applied );
    return ( base && now ) ? (f32)now / (f32)base : 1.0f;
}

/* Frame-begin poll (gui_frame_loop.c): resolve the wanted scale, retarget the font on change.
   Returns true when the active font changed -- the caller forces a full rebuild.  Runs before
   gui_font_flush_deferred so a fresh bake's atlas pixels upload in the same frame's flush, and
   before draw_reset, which re-seeds the ambient text font from font_active_id(). */

bool
gui_dpi_poll( void )
{
    if ( s_dpi.base == GUI_FONT_NONE )
        return false;

    /* A host-driven font is active (font_use / push_font / a custom load): not ours to move. */
    if ( font_active_id() != s_dpi.applied_id )
        return false;

    f32 want = 1.0f;
    if ( s_dpi.mode == GUI_DPI_AUTO )
        want = app()->window_dpi_scale( s_vp_count > 0 ? s_vp_pool[ 0 ].win_id : -1 );
    else if ( s_dpi.mode == GUI_DPI_MANUAL )
        want = s_dpi.manual;

    if ( want < 0.5f ) want = 0.5f;
    if ( want > 4.0f ) want = 4.0f;

    gui_builtin_font_t pick = font_builtin_pick( s_dpi.base, want );
    if ( pick == s_dpi.applied )
        return false;

    /* The base preset lives in init's slot 0; every other bake loads once into its own id.
       A failed load latches a sentinel so a missing asset costs one attempt, not one per frame. */
    #define GUI_DPI_LOAD_FAILED 0xFFFFFFFFu
    u32 id = 0;
    if ( pick != s_dpi.base )
    {
        id = s_dpi.loaded_id[ pick ];
        if ( id == GUI_DPI_LOAD_FAILED )
            return false;                         // asset missing -- stay at the current bake
        if ( id == 0 )
        {
            id = gui_font_load_builtin( pick );   // new id, activated, layout rescaled
            if ( id == 0 )
            {
                s_dpi.loaded_id[ pick ] = GUI_DPI_LOAD_FAILED;
                return false;
            }
            s_dpi.loaded_id[ pick ] = id;
            s_dpi.applied           = pick;
            s_dpi.applied_id        = id;
            return true;
        }
    }

    gui_font_use( id );                           // cached bake: activate + rescale + stamp
    s_dpi.applied    = pick;
    s_dpi.applied_id = id;
    return true;
}

// clang-format on
/*============================================================================================*/
