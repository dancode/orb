/*==============================================================================================

    runtime_service/gui/frame/gui_frame_type.c -- the TYPE RAMP: SMALL / NORMAL / LARGE
    chrome type sizes resolved from the style.

    The style owns one knob, GUI_VAR_TYPE_STEP (px at em=12, em-scaled): SMALL sits that far
    below the body em, LARGE that far above.  This file turns the knob into two loaded font
    ids whenever a style lands (theme change, font change, DPI retarget), and hands chrome a
    scoped bracket -- gui_type_push / gui_type_pop -- that swaps measurement and the TEXT
    command stamp for one label without touching layout metrics, the style, or the redraw
    flag.  Widgets keep their body-derived cells; only the glyphs change size.

    gui ships no bake at "body plus 2px", so the sizes come from a host-installed runtime
    baker (font_baker_set -> dev_font_get is the canonical wiring).  Resolved ids are
    memoized per pixel size; a failed bake latches a sentinel so a missing face costs one
    attempt, not one per frame.  Unwired, or under a host-driven font (the same lineage guard
    the DPI engine uses), both role ids stay 0 and the bracket is a saved no-op.

    Included by gui_frame.c after gui_frame_dpi.c: resolution reads the DPI engine's managed
    lineage (dpi_managed / dpi_landed_bake) to know which family is landed and whether the
    ramp may act at all.

==============================================================================================*/
// clang-format off

#define GUI_TYPE_MEMO_MAX     8             /* distinct baked sizes held at once */
#define GUI_TYPE_STACK_MAX    4             /* push/pop nesting -- role scopes are per-label */
#define GUI_TYPE_LOAD_FAILED  0xFFFFFFFFu   /* memo id: bake attempted and failed; no retries */
#define GUI_TYPE_MIN_PX       8             /* SMALL floor -- below this nothing reads */

static struct
{
    gui_font_bake_fn baker;                 // host-installed runtime baker; NULL = ramp off
    void*            baker_user;

    struct { u32 size_px; u32 id; } memo[ GUI_TYPE_MEMO_MAX ];   // size -> loaded font id
    u32              memo_count;

    u32              small_id;              // landed role ids; 0 = role off (body size)
    u32              large_id;

    struct { u32 font_id; u32 draw_font; } stack[ GUI_TYPE_STACK_MAX ];
    u32              depth;                 // counted past capacity so pop stays paired

    bool             resolving;             // re-entrancy latch (belt over the lineage guard)
    bool             loaded;                // a resolve/prewarm loaded fresh pixels this call

} s_type;

/*==============================================================================================

    The size -> font id memo.  A hit returns the loaded id (0 for the failure sentinel); a
    miss asks the baker for a file and font_load's it.  font_load ACTIVATES what it loads, so
    the active font and the draw stamp are restored around it -- a ramp load must never move
    the ambient state the DPI lineage guard and draw_reset read.

==============================================================================================*/

static u32
type_font_for( const char* family, u32 size_px )
{
    for ( u32 i = 0; i < s_type.memo_count; ++i )
        if ( s_type.memo[ i ].size_px == size_px )
            return s_type.memo[ i ].id == GUI_TYPE_LOAD_FAILED ? 0 : s_type.memo[ i ].id;

    char path[ 576 ];
    bool baked = s_type.baker( family, size_px, path, sizeof( path ), s_type.baker_user );

    u32 prev      = font_active_id();
    u32 prev_draw = draw_get_font();
    u32 id        = 0;

    if ( baked )
    {
        /* A full memo reloads an existing ramp slot in place rather than minting registry ids
           without bound (an editor scrubbing the step knob mints a size per notch).  Live role
           ids are exempt; failure latches are free to overwrite.  The reloaded slot's metrics
           are live on return; its atlas pixels ride the next frame_begin flush. */
        if ( s_type.memo_count >= GUI_TYPE_MEMO_MAX )
        {
            u32 evict = GUI_TYPE_MEMO_MAX;
            for ( u32 i = 0; i < s_type.memo_count; ++i )
            {
                u32 mid = s_type.memo[ i ].id;
                if ( mid == GUI_TYPE_LOAD_FAILED ) { evict = i; break; }   /* free: no slot held */
                if ( mid != s_type.small_id && mid != s_type.large_id )
                    evict = i;
            }
            if ( evict == GUI_TYPE_MEMO_MAX )
                return 0;   /* cannot happen with MEMO_MAX > 2 live roles; stay safe */

            if ( s_type.memo[ evict ].id == GUI_TYPE_LOAD_FAILED )
                id = font_load( path );
            else if ( font_load_into( s_type.memo[ evict ].id, path ) )
                id = s_type.memo[ evict ].id;

            if ( id )
            {
                s_type.memo[ evict ].size_px = size_px;
                s_type.memo[ evict ].id      = id;
            }
            else
                s_type.memo[ evict ] = s_type.memo[ --s_type.memo_count ];   /* drop the corpse */
        }
        else
        {
            id = font_load( path );   /* new registry id, activated -- restored below */
            s_type.memo[ s_type.memo_count ].size_px = size_px;
            s_type.memo[ s_type.memo_count ].id      = id ? id : GUI_TYPE_LOAD_FAILED;
            s_type.memo_count++;
        }
        font_use( prev );
        draw_set_font( prev_draw );
    }
    else if ( s_type.memo_count < GUI_TYPE_MEMO_MAX )
    {
        s_type.memo[ s_type.memo_count ].size_px = size_px;
        s_type.memo[ s_type.memo_count ].id      = GUI_TYPE_LOAD_FAILED;
        s_type.memo_count++;
    }

    if ( id )
        s_type.loaded = true;
    else
        GUI_WARN_ONCE( "type ramp: no bake for '%s' at %upx -- role falls back to the body size",
                       family, size_px );
    return id;
}

/* Role sizes for a body em under the em-scaled step: SMALL floors at GUI_TYPE_MIN_PX and
   collapses to "off" when the clamp lands it back on the body size. */

static u32
type_size_small( f32 em, f32 step )
{
    f32 s = em - step;
    if ( s < (f32)GUI_TYPE_MIN_PX ) s = (f32)GUI_TYPE_MIN_PX;
    u32 px = (u32)( s + 0.5f );
    return px == (u32)( em + 0.5f ) ? 0 : px;
}

/*==============================================================================================

    gui_type_resolve -- re-aim the landed role ids at the current style + landed font.

    Runs at every landing that can move the answer: gui_style_apply (theme / font / deferred
    flush), gui_dpi_land (per-window mixed-DPI re-land), and the frame_begin prewarm.  Cheap
    when nothing changed -- two memo probes.  Bails to "ramp off" whenever the baker is
    absent, the step is zero, the DPI-managed lineage is not the active font (a host-driven
    font is none of our business -- the dpi engine's own rule), or the family has no runtime
    bake source.

==============================================================================================*/

void
gui_type_resolve( void )
{
    if ( s_type.resolving )
        return;
    s_type.resolving = true;

    s_type.small_id = 0;
    s_type.large_id = 0;

    if ( s_type.baker && font_valid() && dpi_managed() )
    {
        f32 step = style_var( GUI_VAR_TYPE_STEP );   /* landed style: already em-scaled */
        const char* family = font_builtin_bake_source( dpi_landed_bake() );
        if ( step >= 0.5f && family )
        {
            f32 em    = font_em();
            u32 small = type_size_small( em, step );
            u32 large = (u32)( em + step + 0.5f );
            if ( small )
                s_type.small_id = type_font_for( family, small );
            s_type.large_id = type_font_for( family, large );
        }
    }
    s_type.resolving = false;
}

/*==============================================================================================

    gui_type_prewarm -- frame_begin step, after gui_dpi_land(0) and before the font flush.

    Resolves the primary surface's roles, then walks every live viewport's bake and warms the
    memo for its two role sizes -- so a mid-frame per-window landing (mixed DPI) is a pure
    memo hit and never loads a font mid-build.  The step is hand-scaled from the BASE style
    per bake: the landed style_var carries only the primary's scale.  Returns true when any
    fresh bake loaded (its pixels ride this frame's atlas flush; the caller dirties the frame
    so the new sizes paint).

==============================================================================================*/

bool
gui_type_prewarm( void )
{
    s_type.loaded = false;
    gui_type_resolve();

    if ( s_type.baker && dpi_managed() )
    {
        const char* family    = font_builtin_bake_source( dpi_landed_bake() );
        f32         base_step = gui_style_peek()->var[ GUI_VAR_TYPE_STEP ];
        if ( family && base_step > 0.0f )
        {
            for ( i32 v = 0; v < s_vp_count; ++v )
            {
                gui_viewport_t* vp = &s_vp_pool[ v ];
                if ( !rhi_handle_valid( vp->vb ) )
                    continue;   /* slot not live */

                u32 size = font_builtin_size( vp->dpi_bake );
                f32 step = base_step * (f32)size / 12.0f;
                if ( !size || step < 0.5f )
                    continue;

                u32 small = type_size_small( (f32)size, step );
                if ( small )
                    type_font_for( family, small );
                type_font_for( family, (u32)( (f32)size + step + 0.5f ) );
            }
        }
    }
    return s_type.loaded;
}

/*==============================================================================================

    gui_type_frame_reset -- frame_begin safety net.  A push without its pop would carry a ramp
    font across frame_begin as the ACTIVE font, which reads as a host takeover to the DPI
    lineage guard and silently freezes retargeting -- so a leak is repaired loudly here: the
    bottom stack entry is the pre-ramp ambient, restore it and start the frame balanced.

==============================================================================================*/

void
gui_type_frame_reset( void )
{
    GUI_CONTRACT( s_type.depth == 0,
                  "gui_type_push without gui_type_pop survived to frame_begin -- bracket the "
                  "label, not the widget's early-outs." );
    if ( s_type.depth )
    {
        font_use( s_type.stack[ 0 ].font_id );
        draw_set_font( s_type.stack[ 0 ].draw_font );
        s_type.depth = 0;
    }
}

/* Forget everything resolved and memoized -- the managed lineage / family changed underneath
   (gui_dpi_base_set) or the GUI is shutting down.  Registry slots stay resident like the DPI
   bakes' do; only the aim is dropped. */

void
gui_type_clear( void )
{
    s_type.memo_count = 0;
    s_type.small_id   = 0;
    s_type.large_id   = 0;
    s_type.depth      = 0;
}

/*==============================================================================================
    Public surface
==============================================================================================*/

/* Install / replace the runtime baker.  Failure latches are dropped -- the new baker may
   succeed where the old one could not -- but loaded sizes are kept.  NULL uninstalls. */

void
gui_font_baker_set( gui_font_bake_fn fn, void* user )
{
    s_type.baker      = fn;
    s_type.baker_user = user;

    u32 keep = 0;
    for ( u32 i = 0; i < s_type.memo_count; ++i )
        if ( s_type.memo[ i ].id != GUI_TYPE_LOAD_FAILED )
            s_type.memo[ keep++ ] = s_type.memo[ i ];
    s_type.memo_count = keep;

    gui_type_resolve();   /* pre-init the guards bail; post-init the roles re-aim now */
}

/* The chrome bracket.  Push saves the active font and the TEXT stamp, then switches both to
   the role's font -- measurement (font_text_w / text_center_y) and the emitted glyphs agree
   for everything inside.  A role that resolved to 0 (ramp off, failed bake, NORMAL) still
   saves, so the matching pop is unconditional.  Never touches metrics or the style: the
   widget's cell stays body-sized and only the glyphs change. */

void
gui_type_push( gui_type_role_t role )
{
    if ( s_type.depth < GUI_TYPE_STACK_MAX )
    {
        s_type.stack[ s_type.depth ].font_id   = font_active_id();
        s_type.stack[ s_type.depth ].draw_font = draw_get_font();

        u32 id = role == GUI_TYPE_SMALL ? s_type.small_id
               : role == GUI_TYPE_LARGE ? s_type.large_id : 0;
        if ( id )
        {
            font_use( id );
            draw_set_font( id );
        }
    }
    s_type.depth++;   /* counted past capacity so pop stays paired */
}

void
gui_type_pop( void )
{
    if ( s_type.depth == 0 )
        return;
    s_type.depth--;
    if ( s_type.depth < GUI_TYPE_STACK_MAX )
    {
        font_use( s_type.stack[ s_type.depth ].font_id );
        draw_set_font( s_type.stack[ s_type.depth ].draw_font );
    }
}

// clang-format on
/*============================================================================================*/
