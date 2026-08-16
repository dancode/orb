/*==============================================================================================

    runtime_service/gui/frame/gui_frame_type.c -- the TYPE RAMP: SMALL / NORMAL / LARGE
    chrome type sizes resolved from the style.

    The style owns one knob, GUI_VAR_TYPE_STEP (px at em=12, em-scaled): SMALL sits that far
    below the body em, LARGE that far above.  This file turns the knob into two loaded font
    ids whenever a style lands (theme change, font change, DPI retarget), and hands chrome a
    scoped bracket -- gui_type_push / gui_type_pop -- that swaps measurement and the TEXT
    command stamp for one label without touching layout metrics, the style, or the redraw
    flag.  Widgets keep their body-derived cells; only the glyphs change size.

    gui ships no bake at "body plus 2px", so the sizes come from the font resolver
    (gui_frame_resolve.c): shipped bakes serve exact sizes, the host-installed baker
    (font_baker_set -> dev_font_get is the canonical wiring) fills the gaps.  A role demands
    its EXACT size -- a resolver answer laddered to a neighbour (no baker, size not shipped)
    reads as "role off" so the ramp never silently aliases a role to the body font.  Under a
    host-driven font (the same lineage guard the DPI engine uses) both role ids stay 0 and
    the bracket is a saved no-op.

    Included by gui_frame.c after gui_frame_dpi.c: resolution reads the DPI engine's managed
    lineage (dpi_managed / dpi_base_family) to know which family is managed and whether the
    ramp may act at all.

==============================================================================================*/
// clang-format off

#define GUI_TYPE_STACK_MAX    4             /* push/pop nesting -- role scopes are per-label */
#define GUI_TYPE_MIN_PX       8             /* SMALL floor -- below this nothing reads */

static struct
{
    u32              small_id;              // landed role ids; 0 = role off (body size)
    u32              large_id;

    struct { u32 font_id; u32 draw_font; } stack[ GUI_TYPE_STACK_MAX ];
    u32              depth;                 // counted past capacity so pop stays paired

    bool             resolving;             // re-entrancy latch (belt over the lineage guard)

} s_type;

/* A role font: the resolver's answer ONLY when it landed the exact size -- a laddered
   neighbour would change layout-vs-glyph agreement subtly, so the role turns off instead. */

static u32
type_font_exact( gui_font_family_t fam, u32 size_px )
{
    u32 landed = 0;
    u32 id     = font_resolve( fam, NULL, size_px, false, &landed );
    return ( id && landed == size_px ) ? id : 0;
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

    if ( font_valid() && dpi_managed() )
    {
        f32 step = style_var( GUI_VAR_TYPE_STEP );   /* landed style: already em-scaled */
        if ( step >= 0.5f )
        {
            f32 em    = font_em();
            u32 small = type_size_small( em, step );
            u32 large = (u32)( em + step + 0.5f );
            if ( small )
                s_type.small_id = type_font_exact( dpi_base_family(), small );
            s_type.large_id = type_font_exact( dpi_base_family(), large );
        }
    }

    /* Live role ids are eviction-exempt while landed (0 clears the pin). */
    font_resolve_pin( FONT_PIN_SMALL, s_type.small_id );
    font_resolve_pin( FONT_PIN_LARGE, s_type.large_id );

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
    gui_type_resolve();

    if ( dpi_managed() )
    {
        f32 base_step = gui_style_peek()->var[ GUI_VAR_TYPE_STEP ];
        if ( base_step > 0.0f )
        {
            for ( i32 v = 0; v < s_vp_count; ++v )
            {
                gui_viewport_t* vp = &s_vp_pool[ v ];
                if ( !rhi_handle_valid( vp->vb ) )
                    continue;   /* slot not live */

                u32 size = vp->dpi_size_px;
                f32 step = base_step * (f32)size / 12.0f;   /* the style var is authored at em 12 */
                if ( !size || step < 0.5f )
                    continue;

                u32 small = type_size_small( (f32)size, step );
                if ( small )
                    type_font_exact( dpi_base_family(), small );
                type_font_exact( dpi_base_family(), (u32)( (f32)size + step + 0.5f ) );
            }
        }
    }
    return font_resolve_fresh_take();
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

/* Drop the landed roles and the bracket stack -- the managed lineage / family changed
   underneath (gui_dpi_base_set) or the GUI is shutting down.  The minted registry slots
   belong to the resolver's memo; releasing them is font_resolve_clear's job, called beside
   this at both call sites.  This only unpins and forgets. */

void
gui_type_clear( void )
{
    s_type.small_id = 0;
    s_type.large_id = 0;
    s_type.depth    = 0;
    font_resolve_pin( FONT_PIN_SMALL, 0 );
    font_resolve_pin( FONT_PIN_LARGE, 0 );
}

/*==============================================================================================
    Public surface
==============================================================================================*/

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
