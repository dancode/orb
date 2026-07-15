/*==============================================================================================

    runtime_service/gui/interact/gui_anim.c -- Animation stepping service.

    The value-stepping primitive behind every smoothed transition: peek-guard logic, channel
    stepping, slot stamping, and wants_redraw signalling in one function, so callers stay
    simple.  Pure timing -- what a stepped value MEANS (a color blend, an eased extent) is the
    caller's business: the animated background color lives with the skin
    (present/gui_paint_core.c: widget_bg_color_anim), the size ease with the layout engine
    (compose/gui_layout_core.c: size_animate).

    Storage lives in the keyed state pool (foundation/gui_state.c) with peek-then-stamp semantics:
    pool pressure stays proportional to in-flight animations, not total widget count.  Idle
    values with no animation history pay only a single non-stamping probe.

    Included by gui.c after the compose/ files; earlier callers reach it through the forward
    declaration in gui_internal.h.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    gui_anim_f32 -- single-channel exponential-decay animation

    Steps a named float toward `target` each frame.  `speed` is in Hz-like units:
    10  ~  250 ms to 95% of target
    20  ~  150 ms to 95% of target

    Peek-then-stamp: the pool is not touched when the value has no history or has already
    settled.  Compose anim_id via id_combine( widget_id, tag ) so each channel occupies its
    own slot without colliding with other per-widget state:

        f32 t = gui_anim_f32( id_combine( id, 1u ), hovered ? 1.0f : 0.0f, 10.0f );
----------------------------------------------------------------------------------------------*/

typedef struct { f32 current; } gui_anim_f32_t;

/* gui_anim_f32_from -- damper seeded from an explicit rest value on first sight.

   The primitive above assumes a channel with no history is already AT its target -- correct when a
   value's first appearance should not animate (size_animate: a widget shows at its natural size and
   eases only on later changes).  A transient highlight is the opposite: its rest state (`rest`) is
   the natural start and the target is the passing state, so first sight must ramp FROM rest, not
   snap to target.  This variant seeds `current` from `rest` when there is no slot; gui_anim_f32 is
   just this with rest == target.  Give hover/press/focus channels rest = their off value (0). */
static f32
gui_anim_f32_from( gui_id_t anim_id, f32 rest, f32 target, f32 speed )
{
    const gui_anim_f32_t* peek = (const gui_anim_f32_t*)gui_state_peek( anim_id );
    f32 current = peek ? peek->current : rest;

    bool moving = fabsf( target - current ) >= 0.001f;
    if ( moving )
    {
        f32 dt = s_io.dt > 0.0001f ? s_io.dt : 0.0001f;
        current += ( target - current ) * ( 1.0f - expf( -speed * dt ) );
        g_ctx->retained.wants_redraw = true;   /* value changed this frame -- keep frames coming */
    }
    else
    {
        current = target;   /* settled: snap exactly onto the target */
    }

    /* Persist the slot while the value sits away from rest (moving, or parked on a non-rest hold like
       a steady hover) so peek keeps it warm and it never re-ramps from rest after an eviction.  Only
       when parked exactly AT rest do we skip the stamp and let the slot go cold: reseeding from rest
       then reproduces the same value, so eviction is invisible.  gui_anim_f32 (rest == target) always
       takes that cold path once settled, preserving its original evict-when-idle behavior. */
    if ( moving || fabsf( current - rest ) >= 0.001f )
        GUI_STATE( gui_anim_f32_t, anim_id )->current = current;

    return current;
}

static f32
gui_anim_f32( gui_id_t anim_id, f32 target, f32 speed )
{
    return gui_anim_f32_from( anim_id, target, target, speed );
}

/*----------------------------------------------------------------------------------------------
    gui_anim_timer -- fixed-duration from/to easing (the timed-tween model)

    The complement to gui_anim_f32's exponential damping: instead of chasing a moving target at a
    decay rate, this runs a normalized clock 0 -> 1 over a fixed duration and hands back an eased
    progress the caller lerps its OWN from/to values with.  Two properties the damper cannot give:

      - Shared clock.  Several channels driven off ONE timer id (a rect's x/y/w/h, say) all read the
        same t, so they depart and ARRIVE together -- no channel lags because its delta was smaller.
      - Definite end.  A damper only ever approaches its target; a timer reaches t == 1 exactly on
        the frame the duration elapses, FORCES that end, reports "done", and evicts.  The caller can
        then snap to its target and drop its own animating flag with no risk of a stuck mid-tween.

    Storage is a single keyed slot (elapsed + duration).  gui_anim_timer_start seeds it; each
    gui_anim_timer call advances and shapes it through `ease` (any f32 shaper: f32_ease_out_cubic,
    f32_smoothstep01, ... or NULL for linear).  A slot that is absent or already finished reads as
    settled at 1.0, so a caller that samples one extra frame still sees the end, never a restart.
----------------------------------------------------------------------------------------------*/

typedef struct { f32 elapsed; f32 duration; } gui_anim_timer_t;
typedef f32 ( *gui_ease_fn )( f32 );

/* Start (or restart) a duration-based timer on `id`: clock at 0, running for `duration` seconds.  A
   zero / negative duration is the "no animation" request -- no slot is stamped, so the first
   gui_anim_timer reads settled and the caller snaps straight to its target (the toggle-off path). */
static void
gui_anim_timer_start( gui_id_t id, f32 duration )
{
    if ( duration <= 0.0f )
        return;   /* instant: leave the slot unseeded so the next sample reports done at t=1 */

    gui_anim_timer_t* a = GUI_STATE( gui_anim_timer_t, id );
    a->elapsed  = 0.0f;
    a->duration = duration;
    g_ctx->retained.wants_redraw = true;
}

/* Advance and sample the timer: returns the eased progress in [0,1].  While running it stamps the
   slot and holds wants_redraw so the frames keep coming; on the frame the clock reaches the
   duration it returns exactly 1.0, clears the slot's duration (so it goes cold and evicts), and
   sets *out_active false.  An absent / finished slot also returns 1.0 with *out_active false -- the
   settled end, forced, so the value can never be read half-finished once the tween is over. */
static f32
gui_anim_timer( gui_id_t id, gui_ease_fn ease, bool* out_active )
{
    const gui_anim_timer_t* pk = (const gui_anim_timer_t*)gui_state_peek( id );
    if ( !pk || pk->duration <= 0.0f )
    {
        if ( out_active ) *out_active = false;
        return 1.0f;
    }

    gui_anim_timer_t* a  = GUI_STATE( gui_anim_timer_t, id );   /* restamp seen_frame */
    f32 dt = s_io.dt > 0.0001f ? s_io.dt : 0.0001f;
    a->elapsed += dt;

    if ( a->elapsed >= a->duration )
    {
        a->duration = 0.0f;                 /* done: mark settled; slot evicts once cold */
        if ( out_active ) *out_active = false;
        return 1.0f;                        /* force the exact end -- caller snaps to its target */
    }

    g_ctx->retained.wants_redraw = true;
    if ( out_active ) *out_active = true;
    f32 t = a->elapsed / a->duration;
    return ease ? ease( t ) : t;
}

/*----------------------------------------------------------------------------------------------
    Public animation surface (gui_api_t)

    The two primitives above are the whole engine; these are the callable face of them.  anim_f32 and
    anim_start are the primitives verbatim (same signature, assigned straight onto the vtable in
    gui_api.c).  The rest are thin: anim_ease maps the public gui_ease_t enum onto a shaper before
    calling gui_anim_timer; the typed channels run one damper per component so a color, a point, or a
    rect glides to a new data state without every caller re-deriving the blend.
----------------------------------------------------------------------------------------------*/

/* Map the public enum onto the base math_ease shapers.  NULL == linear (gui_anim_timer treats a NULL
   shaper as the identity), so GUI_EASE_LINEAR and any out-of-range value fall through to it. */
static gui_ease_fn
gui_ease_lookup( gui_ease_t e )
{
    switch ( e )
    {
        case GUI_EASE_SMOOTH:      return f32_smoothstep01;
        case GUI_EASE_IN_CUBIC:    return f32_ease_in_cubic;
        case GUI_EASE_OUT_CUBIC:   return f32_ease_out_cubic;
        case GUI_EASE_INOUT_CUBIC: return f32_ease_inout_cubic;
        case GUI_EASE_OUT_EXPO:    return f32_ease_out_expo;
        case GUI_EASE_OUT_BACK:    return f32_ease_out_back;
        default:                   return NULL;   /* GUI_EASE_LINEAR + guard */
    }
}

/* Public tween sampler: advance the timer on `id` and return eased progress in [0,1].  Pair with
   anim_start (== gui_anim_timer_start) which seeds the clock; *out_active is false once settled. */
static f32
gui_api_anim_ease( gui_id_t id, gui_ease_t ease, bool* out_active )
{
    return gui_anim_timer( id, gui_ease_lookup( ease ), out_active );
}

/* Damped ABGR blend: each of the four channels chases its target through gui_anim_f32, so the color
   glides to target_abgr at `speed` (Hz-like, see gui_anim_f32).  Per-channel ids are derived from a
   single caller id so the four slots never collide with each other or with other per-widget state. */
static u32
gui_api_anim_color( gui_id_t id, u32 target_abgr, f32 speed )
{
    f32 r = gui_anim_f32( id_combine( id, 0x51u ), (f32)( ( target_abgr       ) & 0xFF ), speed );
    f32 g = gui_anim_f32( id_combine( id, 0x52u ), (f32)( ( target_abgr >>  8 ) & 0xFF ), speed );
    f32 b = gui_anim_f32( id_combine( id, 0x53u ), (f32)( ( target_abgr >> 16 ) & 0xFF ), speed );
    f32 a = gui_anim_f32( id_combine( id, 0x54u ), (f32)( ( target_abgr >> 24 ) & 0xFF ), speed );
    return (u32)( r + 0.5f ) | ( (u32)( g + 0.5f ) << 8 ) | ( (u32)( b + 0.5f ) << 16 ) | ( (u32)( a + 0.5f ) << 24 );
}

/* Damped 2D point: x and y chase independently off one caller id. */
static gui_vec2_t
gui_api_anim_vec2( gui_id_t id, gui_vec2_t target, f32 speed )
{
    gui_vec2_t out;
    out.x = gui_anim_f32( id_combine( id, 0x61u ), target.x, speed );
    out.y = gui_anim_f32( id_combine( id, 0x62u ), target.y, speed );
    return out;
}

/* Damped rect: four independent dampers off one caller id (position + extent glide together). */
static gui_rect_t
gui_api_anim_rect( gui_id_t id, gui_rect_t target, f32 speed )
{
    gui_rect_t out;
    out.x = gui_anim_f32( id_combine( id, 0x71u ), target.x, speed );
    out.y = gui_anim_f32( id_combine( id, 0x72u ), target.y, speed );
    out.w = gui_anim_f32( id_combine( id, 0x73u ), target.w, speed );
    out.h = gui_anim_f32( id_combine( id, 0x74u ), target.h, speed );
    return out;
}

// clang-format on
/*============================================================================================*/
