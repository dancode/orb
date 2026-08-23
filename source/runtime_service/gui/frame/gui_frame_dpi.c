/*==============================================================================================

    runtime_service/gui/frame/gui_frame_dpi.c -- keeps the UI the right size on high-DPI
    displays.

    Everything scales off one number: the display's DPI factor.  That factor comes from the
    font size the surface landed (the loaded atlas already matches the target scale), and
    from there it drives widget sizing, layout spacing, and window placement -- a single
    source of truth for "how big is a pixel" across the whole GUI.

    This file holds the current DPI mode and scale, resolves the right font SIZE for each
    viewport through the font resolver, applies that scale when a window lands on screen,
    and checks for DPI changes at the start of every frame.

    Include order matters: this file loads after gui_frame_resolve.c (every size it wants
    goes through font_resolve) and before gui_viewport.c, which needs a scale factor already
    in place when it creates a viewport.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    DPI response -- monitor scale -> font size retarget, PER VIEWPORT.

    The engine works in physical pixels end to end, so gui scales by asking the resolver for
    the managed family at base_size * monitor_scale: a bigger size raises em, and every layout
    metric already rescales from em (metrics_compute).  No second scale factor exists
    anywhere.  With a runtime baker installed the response is EXACT (any size bakes on
    demand); without one the resolver ladders to the nearest shipped size, and the granularity
    is whatever the family ships.

    Mixed-DPI monitors: each viewport carries its own landed size (gui_viewport_t.dpi_size_px),
    resolved from ITS OS window's monitor scale.  Two scales are never live at once --
    contexts and windows build sequentially -- so gui_dpi_land re-lands the one live style
    whenever the window about to emit sits on a surface whose size differs from the last
    landed one (window_begin drives it).  The frame-begin poll resolves per-viewport wants,
    rescales that surface's window footprints on a change, and re-lands the primary so the
    ambient (pre-window) state is viewport 0's.  The poll re-resolves every frame (a memo
    probe when nothing changed), so a baker installed after boot upgrades laddered sizes to
    exact ones on its next pass.

    Managed only while the host's init() lineage is active: poll and land act when the active
    font is the one the landing last activated (slot 0 at init).  A host that font_use()s /
    font_load()s its own font takes over; management resumes when the managed font is active
    again.  A GUI_FONT_NONE boot disables the mechanism entirely.

==============================================================================================*/

static struct
{
    gui_dpi_mode_t    mode;         // OFF / AUTO / MANUAL
    f32               manual;       // MANUAL-mode factor
    gui_font_family_t base_family;  // init() family; GUI_FONT_NONE = unmanaged
    u32               base_size;    // the size init() landed -- scale 1.0 by definition
    u32               landed_size;  // size whose metrics are in s_style now
    u32               landed_id;    // font id the landing last activated (lineage guard)

} s_dpi = { .mode = GUI_DPI_AUTO, .manual = 1.0f };

/* Latched by gui_dpi_poll, consumed once by cache_place_slots -- see gui_dpi_frame_changed. */
static bool s_dpi_changed_frame;

/*==============================================================================================

    Scale a landed size represents: its px over the init() base size.  1.0 while unmanaged.

==============================================================================================*/

static f32
dpi_bake_scale( u32 size_px )
{
    return ( s_dpi.base_size && size_px ) ? (f32)size_px / (f32)s_dpi.base_size : 1.0f;
}

/*==============================================================================================

    Scale gui_style_apply feeds metrics_compute (grid quantum): the size being APPLIED.

==============================================================================================*/

static f32
dpi_scale_landed( void )
{
    return dpi_bake_scale( s_dpi.landed_size );
}

/* The type ramp's view of this engine (gui_frame_type.c, later in this TU): whether the
   managed lineage is what is active -- the same test every dpi entry point gates on -- and
   which family it manages (what the ramp sizes resolve from). */

static bool
dpi_managed( void )
{
    return s_dpi.base_family != GUI_FONT_NONE && font_active_id() == s_dpi.landed_id;
}

static gui_font_family_t
dpi_base_family( void )
{
    return s_dpi.base_family;
}

/* Seed the managed lineage after init() loads the boot font into slot 0.  landed_px is what
   the boot resolve actually landed (the wanted size, or the nearest shipped neighbour). */

void
gui_dpi_base_set( gui_font_family_t fam, u32 landed_px )
{
    s_dpi.base_family = fam;
    s_dpi.base_size   = landed_px;
    s_dpi.landed_size = landed_px;
    s_dpi.landed_id   = 0;
    for ( u32 v = 0; v < GUI_MAX_VIEWPORTS; ++v )
        s_vp_pool[ v ].dpi_size_px = landed_px;   /* surfaces open post-init inherit via viewport_create */
    gui_type_clear();      /* ramp roles resolved against the old lineage are stale */
    font_resolve_clear();  /* the memo's minted sizes belong to the old family -- release them */
    if ( fam != GUI_FONT_NONE && landed_px )
        font_resolve_adopt_default( fam, landed_px );   /* the base size answers slot 0, not a duplicate */
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
   DPI); their scale is their own landed size's, applied per window during the build. */

f32
gui_dpi_scale( void )
{
    return dpi_bake_scale( s_vp_pool[ 0 ].dpi_size_px );
}

/* Resolve the size ONE viewport wants (mode + its own OS window's monitor scale) and stamp it.
   Returns true when the stamp changed.  The resolver never activates anything, so this is
   safe anywhere; the landing (gui_dpi_land) is the only activation point.  Non-static:
   frame/gui_viewport.c resolves a fresh floater at spawn (before its first poll). */

bool
gui_dpi_vp_resolve( i32 v )
{
    if ( s_dpi.base_family == GUI_FONT_NONE )
        return false;

    /* A host-driven font is active (font_use / push_font / a custom load): not ours to move. */
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

    u32 want_px = (u32)( (f32)s_dpi.base_size * want + 0.5f );
    if ( want_px == 0 )
        return false;

    /* The resolver's LANDED size is the stamp: with a baker it is want_px exactly, without
       one the nearest shipped size -- either way the memo answers the next poll for free.
       A request no layer could serve (landed 0) keeps the current stamp. */
    u32 landed = 0;
    u32 id     = font_resolve( s_dpi.base_family, NULL, want_px, false, &landed );
    if ( landed == 0 )
        return false;
    if ( landed == vp->dpi_size_px )
        return false;

    vp->dpi_size_px = landed;
    font_resolve_pin( FONT_PIN_VP0 + (u32)v, id );   /* this surface's landed font: not evictable */
    return true;
}

/* Land a viewport's size: activate its font and rescale the live metrics to it.  The per-window
   half of the mixed-DPI response -- window_begin calls it as each window's surface comes up, so
   one sequential frame emits every surface with its own scale.  QUIET on purpose: no
   redraw_request -- a routine landing re-applies known state and must not mark the frame dirty,
   or two surfaces at different scales would rebuild every frame forever. */

void
gui_dpi_land( i32 viewport )
{
    if ( s_dpi.base_family == GUI_FONT_NONE || viewport < 0 || viewport >= GUI_MAX_VIEWPORTS )
        return;

    /* A host-driven font is active (font_use / push_font / a custom load): not ours to move. */
    if ( font_active_id() != s_dpi.landed_id )
        return;

    u32 size = s_vp_pool[ viewport ].dpi_size_px;
    if ( size == 0 || size == s_dpi.landed_size )
        return;

    /* A stamped size is always a memo hit -- vp_resolve stamped what the resolver landed. */
    u32 landed = 0;
    u32 id     = font_resolve( s_dpi.base_family, NULL, size, false, &landed );
    if ( landed != size )
        return;   /* the answer moved underneath (family change mid-frame): keep the landed scale */

    /* Commit BEFORE the switch: the metrics recompute reads dpi_scale_landed() for the quantum
       and must see the scale being applied, not the one being left. */
    s_dpi.landed_size = size;

    font_use( id );
    metrics_compute( (u32)font_em(), (u32)font_char_h(), (u32)font_line_h(), dpi_scale_landed() );
    style_landing();

    /* Stamp the emit layer like gui_font_use does -- cuts no segment; subsequent TEXT commands
       carry this font.  landed_id from what is ACTUALLY active (font_use ignores a bad id). */
    draw_set_font( font_active_id() );
    s_dpi.landed_id = font_active_id();

    /* Re-aim the type ramp at this surface's em -- prewarmed, so a memo hit, and QUIET like
       the landing itself. */
    gui_type_resolve();

    /* This surface's scale is now one the frame will emit at, so the palette bake needs rows for
       it: a radius or a border reaches the prim record already scaled, and a 2x record shares no
       bytes with its 1x twin.  Without this the shapes on every non-primary monitor would miss the
       palette and take per-slot records -- correct, but none of the saving. */
    pal_style_note();
}

/* Frame-begin poll (gui_frame_loop.c): resolve every live surface's wanted scale, retarget on
   change.  Returns true when any size changed -- the caller forces a full rebuild.  Runs before
   gui_font_flush_deferred so a fresh bake's atlas pixels upload in the same frame's flush, and
   before draw_reset, which re-seeds the ambient text font from font_active_id(). */

bool
gui_dpi_poll( void )
{
    if ( s_dpi.base_family == GUI_FONT_NONE )
        return false;

    /* A host-driven font is active (font_use / push_font / a custom load): not ours to move. */
    if ( font_active_id() != s_dpi.landed_id )
        return false;

    bool changed = false;
    for ( i32 v = 0; v < s_vp_count; ++v )
    {
        gui_viewport_t* vp = &s_vp_pool[ v ];
        if ( !vp->live )
            continue;   /* slot not live */

        /* Snapshot the hosting window's own OS scale to tell an OS-driven change (WM_DPICHANGED
           already resized the window to the new monitor) from a gui-driven one (ui_scale / mode
           flip: gui must resize the owned floater itself, below). */
        f32  os       = vp->win_id >= 0 ? app()->window_dpi_scale( vp->win_id ) : 1.0f;
        bool os_moved = os != vp->dpi_os_scale;
        vp->dpi_os_scale = os;

        f32 old_scale = dpi_bake_scale( vp->dpi_size_px );
        if ( !gui_dpi_vp_resolve( v ) )
            continue;
        f32 ratio = dpi_bake_scale( vp->dpi_size_px ) / old_scale;

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
    s_dpi_changed_frame = changed;
    return changed;
}

/* See gui.h.  Read once by cache_place_slots, which forces a full re-place (reuse off, geometry
   generation bumped) exactly the way a palette epoch does -- a scale change is rare enough that
   trading the retained-cache win for a guaranteed-clean rebuild is free, and correctness across a
   move that touches every layout metric at once matters far more than one saved tessellation. */
bool
gui_dpi_frame_changed( void )
{
    return s_dpi_changed_frame;
}

// clang-format on
/*============================================================================================*/
