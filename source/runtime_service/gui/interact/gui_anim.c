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

static f32
gui_anim_f32( gui_id_t anim_id, f32 target, f32 speed )
{
    const gui_anim_f32_t* peek = (const gui_anim_f32_t*)gui_state_peek( anim_id );
    f32 current = peek ? peek->current : target;

    if ( fabsf( target - current ) < 0.001f )
        return target;   /* settled: do not stamp; slot evicts via seen_frame */

    f32 dt   = s_io.dt > 0.0001f ? s_io.dt : 0.0001f;
    current += ( target - current ) * ( 1.0f - expf( -speed * dt ) );

    GUI_STATE( gui_anim_f32_t, anim_id )->current = current;
    g_ctx->retained.wants_redraw = true;
    return current;
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

// clang-format on
/*============================================================================================*/
