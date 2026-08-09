/*==============================================================================================

    runtime_service/gui/frame/gui_frame_dpi.c -- the DPI response engine.

    The state (mode / manual factor / managed lineage / shared bake cache), the per-viewport
    bake resolve, the per-window landing, and the frame-begin poll.  The model -- scale enters
    ONLY via the font bake, so one scale factor drives metrics, quantum, and window rects --
    is documented on the section banner below.

    Included by the gui_frame.c unit root AFTER gui_frame_font.c (bake loads ride
    gui_font_load_builtin; gui_style_apply reads dpi_scale_landed() through its forward decl)
    and BEFORE gui_viewport.c (viewport_create seeds dpi_bake from s_dpi.base; the tear-off
    path drives gui_dpi_vp_resolve + dpi_bake_scale).

==============================================================================================*/
// clang-format off

/*==============================================================================================
    DPI response -- monitor scale -> font retarget, PER VIEWPORT.

    The engine works in physical pixels end to end, so gui scales by swapping the ACTIVE FONT
    for a bake of the same family nearer the wanted size: a bigger bake raises em, and every
    layout metric already rescales from em (metrics_compute).  No second scale factor exists
    anywhere -- the response granularity is the set of baked sizes the family ships.

    Mixed-DPI monitors: each viewport carries its own bake (gui_viewport_t.dpi_bake), resolved
    from ITS OS window's monitor scale.  Two scales are never live at once -- contexts and
    windows build sequentially -- so gui_dpi_land re-lands the one live style whenever the
    window about to emit sits on a surface whose bake differs from the last landed one
    (window_begin drives it).  The bake set loads once into shared ids (loaded_id[]); the
    frame-begin poll resolves per-viewport wants, rescales that surface's window footprints on
    a change, and re-lands the primary so the ambient (pre-window) state is viewport 0's.

    Managed only while the host's init() preset lineage is active: poll and land act when the
    active font is the one the landing last activated (slot 0 at init).  A host that
    font_use()s / font_load()s its own font takes over; management resumes when the managed
    font is active again.  GUI_FONT_NONE at init disables the mechanism entirely.
==============================================================================================*/

static struct
{
    gui_dpi_mode_t     mode;                                // OFF / AUTO / MANUAL
    f32                manual;                              // MANUAL-mode factor
    gui_builtin_font_t base;                                // init() preset; NONE = unmanaged
    gui_builtin_font_t landed;                              // bake whose metrics are in s_style now
    u32                landed_id;                           // font id the landing last activated (lineage guard)
    u32                loaded_id[ GUI_FONT_BUILTIN_COUNT ]; // preset -> loaded font id (0 = not yet)

} s_dpi = { .mode = GUI_DPI_AUTO, .manual = 1.0f };

/* A missing asset latches this sentinel in loaded_id[]: one load attempt, not one per frame. */
#define GUI_DPI_LOAD_FAILED 0xFFFFFFFFu

/* Scale a bake represents: its size over the init() preset's size.  1.0 while unmanaged. */

static f32
dpi_bake_scale( gui_builtin_font_t bake )
{
    u32 base = font_builtin_size( s_dpi.base );
    u32 now  = font_builtin_size( bake );
    return ( base && now ) ? (f32)now / (f32)base : 1.0f;
}

/* The scale gui_style_apply feeds metrics_compute (grid quantum): the bake being APPLIED. */

static f32
dpi_scale_landed( void )
{
    return dpi_bake_scale( s_dpi.landed );
}

/* Debug cross-check: dpi_bake_scale trusts the builtin table's MIRRORED size, while metrics
   scale from the file's real em -- a table row that drifts from its .orb_font header would
   silently split the one scale factor in two (quantum + window rescale vs layout metrics).
   Called right after a preset load activates, when font_em() IS the file's baked size; a
   mismatch downgrades that silent geometry drift to a log line. */

static void
dpi_bake_size_check( gui_builtin_font_t bake )
{
    u32 expect = font_builtin_size( bake );
    u32 got    = (u32)( font_em() + 0.5f );
    if ( expect && got && got != expect )
        gui_log( GUI_LOG_WARN,
                 "builtin bake size mismatch: preset %u bakes %upx, table says %upx",
                 (u32)bake, got, expect );
}

/* Font id holding a bake: the base preset lives in init's slot 0; every other bake must have
   been loaded by gui_dpi_vp_resolve first.  False = not resident (never landed with). */

static bool
dpi_bake_id( gui_builtin_font_t bake, u32* out_id )
{
    if ( bake == s_dpi.base )
    {
        *out_id = 0;
        return true;
    }
    u32 id = s_dpi.loaded_id[ bake ];
    if ( id == 0 || id == GUI_DPI_LOAD_FAILED )
        return false;
    *out_id = id;
    return true;
}

/* Seed the managed lineage after init() loads the host's preset into slot 0. */

void
gui_dpi_base_set( gui_builtin_font_t font )
{
    s_dpi.base      = font;
    s_dpi.landed    = font;
    s_dpi.landed_id = 0;
    dpi_bake_size_check( font );   /* init just loaded + activated the preset into slot 0 */
    for ( u32 i = 0; i < GUI_FONT_BUILTIN_COUNT; ++i )
        s_dpi.loaded_id[ i ] = 0;
    for ( u32 v = 0; v < GUI_MAX_VIEWPORTS; ++v )
        s_vp_pool[ v ].dpi_bake = font;   /* surfaces open post-init inherit via viewport_create */
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

/* The scale actually in effect on the PRIMARY surface -- the wanted monitor scale is
   app()->window_dpi_scale; this is what the UI IS.  Secondary surfaces may differ (mixed
   DPI); their scale is their own bake's, landed per window during the build. */

f32
gui_dpi_scale( void )
{
    return dpi_bake_scale( s_vp_pool[ 0 ].dpi_bake );
}

/* Resolve the bake ONE viewport wants (mode + its own OS window's monitor scale) and stamp it,
   loading a first-use bake into the shared id set.  Returns true when the stamp changed.
   Non-static: frame/gui_viewport.c resolves a fresh floater at spawn (before its first poll). */

bool
gui_dpi_vp_resolve( u32 v )
{
    if ( s_dpi.base == GUI_FONT_NONE )
        return false;

    /* A host-driven font is active (font_use / push_font / a custom load): not ours to move.
       Poll and land guard this too, but tear-off calls resolve directly -- unguarded, a
       first-use bake load below would ACTIVATE the bake over the host's font and adopt the
       fresh id as the managed lineage. */
    if ( font_active_id() != s_dpi.landed_id )
        return false;

    gui_viewport_t* vp = &s_vp_pool[ v ];

    f32 want = 1.0f;
    if ( s_dpi.mode == GUI_DPI_AUTO )
        want = vp->win_id >= 0 ? app()->window_dpi_scale( vp->win_id ) : 1.0f;
    else if ( s_dpi.mode == GUI_DPI_MANUAL )
        want = s_dpi.manual;

    if ( want < 0.5f ) want = 0.5f;
    if ( want > 4.0f ) want = 4.0f;

    gui_builtin_font_t pick = font_builtin_pick( s_dpi.base, want );
    if ( pick == vp->dpi_bake )
        return false;

    /* First use of a bake loads it once, shared by every surface that later wants it.  The load
       activates the new font, so commit the landing bookkeeping FIRST: the gui_style_apply
       inside reads dpi_scale_landed() for the grid quantum, and the lineage guard must not read
       the fresh id as a host takeover.  Rolled back on a failed load (sentinel: no retries). */
    if ( pick != s_dpi.base && s_dpi.loaded_id[ pick ] == 0 )
    {
        gui_builtin_font_t prev = s_dpi.landed;
        s_dpi.landed            = pick;

        u32 id = gui_font_load_builtin( pick );   // new id, activated, layout rescaled
        if ( id == 0 )
        {
            s_dpi.landed            = prev;
            s_dpi.loaded_id[ pick ] = GUI_DPI_LOAD_FAILED;
            return false;
        }
        s_dpi.loaded_id[ pick ] = id;
        s_dpi.landed_id         = id;
        dpi_bake_size_check( pick );   /* the load activated it -- font_em() is the file's size */
    }
    if ( pick != s_dpi.base && s_dpi.loaded_id[ pick ] == GUI_DPI_LOAD_FAILED )
        return false;

    vp->dpi_bake = pick;
    return true;
}

/* Land a viewport's bake: activate its font and rescale the live metrics to it.  The per-window
   half of the mixed-DPI response -- window_begin calls it as each window's surface comes up, so
   one sequential frame emits every surface with its own scale.  QUIET on purpose: no
   redraw_request -- a routine landing re-applies known state and must not mark the frame dirty,
   or two surfaces at different scales would rebuild every frame forever. */

void
gui_dpi_land( u32 viewport )
{
    if ( s_dpi.base == GUI_FONT_NONE || viewport >= GUI_MAX_VIEWPORTS )
        return;

    /* A host-driven font is active (font_use / push_font / a custom load): not ours to move. */
    if ( font_active_id() != s_dpi.landed_id )
        return;

    gui_builtin_font_t bake = s_vp_pool[ viewport ].dpi_bake;
    if ( bake == GUI_FONT_NONE || bake == s_dpi.landed )
        return;

    u32 id;
    if ( !dpi_bake_id( bake, &id ) )
        return;   /* bake never loaded (failed asset): the surface keeps the landed scale */

    /* Commit BEFORE the switch: the metrics recompute reads dpi_scale_landed() for the quantum
       and must see the scale being applied, not the one being left. */
    s_dpi.landed = bake;

    font_use( id );
    metrics_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h(), dpi_scale_landed() );
    style_landing();

    /* Stamp the emit layer like gui_font_use does -- cuts no segment; subsequent TEXT commands
       carry this font.  landed_id from what is ACTUALLY active (font_use ignores a bad id). */
    draw_set_font( font_active_id() );
    s_dpi.landed_id = font_active_id();
}

/* Frame-begin poll (gui_frame_loop.c): resolve every live surface's wanted scale, retarget on
   change.  Returns true when any bake changed -- the caller forces a full rebuild.  Runs before
   gui_font_flush_deferred so a fresh bake's atlas pixels upload in the same frame's flush, and
   before draw_reset, which re-seeds the ambient text font from font_active_id(). */

bool
gui_dpi_poll( void )
{
    if ( s_dpi.base == GUI_FONT_NONE )
        return false;

    /* A host-driven font is active (font_use / push_font / a custom load): not ours to move. */
    if ( font_active_id() != s_dpi.landed_id )
        return false;

    bool changed = false;
    for ( u32 v = 0; v < s_vp_count; ++v )
    {
        gui_viewport_t* vp = &s_vp_pool[ v ];
        if ( !rhi_handle_valid( vp->vb ) )
            continue;   /* slot not live */

        /* Snapshot the hosting window's own OS scale to tell an OS-driven change (WM_DPICHANGED
           already resized the window to the new monitor) from a gui-driven one (ui_scale / mode
           flip: gui must resize the owned floater itself, below). */
        f32  os       = vp->win_id >= 0 ? app()->window_dpi_scale( vp->win_id ) : 1.0f;
        bool os_moved = os != vp->dpi_os_scale;
        vp->dpi_os_scale = os;

        f32 old_scale = dpi_bake_scale( vp->dpi_bake );
        if ( !gui_dpi_vp_resolve( v ) )
            continue;
        f32 ratio = dpi_bake_scale( vp->dpi_bake ) / old_scale;

        /* Metrics and text on this surface just changed by `ratio` -- carry its persisted
           window footprints along so everything keeps its apparent size. */
        windows_dpi_rescale( v, ratio );

        /* gui-owned floater whose change did NOT come from its own monitor: grow the OS client
           area so the pinned shell keeps its footprint (the autosize-grip resize primitive).
           An OS-driven change already applied the suggested rect -- resizing again would
           double-scale. */
        if ( vp->owned && !os_moved && vp->win_id >= 0 )
        {
            i32 w = 0, h = 0;
            app()->window_get_size( vp->win_id, &w, &h );
            if ( w > 0 && h > 0 )
                app()->window_resize( vp->win_id,
                                      (i32)( (f32)w * ratio + 0.5f ),
                                      (i32)( (f32)h * ratio + 0.5f ) );
        }
        changed = true;
    }
    return changed;
}

// clang-format on
/*============================================================================================*/
