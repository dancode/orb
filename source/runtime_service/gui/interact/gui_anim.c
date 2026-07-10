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

// clang-format on
/*============================================================================================*/
