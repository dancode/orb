/*==============================================================================================

    runtime_service/imgui/imgui_anim.c -- Widget animation utilities.

    A growing collection of reusable stateful effects.  Each effect is a single function
    that a widget calls with its id and resolved interaction state; all peek-guard logic,
    channel stepping, slot stamping, and wants_redraw signalling is handled here so widgets
    stay simple.

    Storage lives in the keyed state pool (imgui_ctx_id.c) with peek-then-stamp semantics:
    pool pressure stays proportional to in-flight animations, not total widget count.  Idle
    widgets with no animation history pay only a single non-stamping probe.

    Included by imgui.c after imgui_widget_core.c (COL_*, col_lerp, widget_state_t) and
    before imgui_widget.c (which calls into this).

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    imgui_anim_f32 -- single-channel exponential-decay animation

    Steps a named float toward `target` each frame.  `speed` is in Hz-like units:
    10  ~  250 ms to 95% of target
    20  ~  150 ms to 95% of target

    Peek-then-stamp: the pool is not touched when the value has no history or has already
    settled.  Compose anim_id via id_combine( widget_id, tag ) so each channel occupies its
    own slot without colliding with other per-widget state:

        f32 t = imgui_anim_f32( id_combine( id, 1u ), hovered ? 1.0f : 0.0f, 10.0f );
----------------------------------------------------------------------------------------------*/

typedef struct { f32 current; } imgui_anim_f32_t;

static f32
imgui_anim_f32( imgui_id_t anim_id, f32 target, f32 speed )
{
    const imgui_anim_f32_t* peek = (const imgui_anim_f32_t*)imgui_state_peek( anim_id );
    f32 current = peek ? peek->current : target;

    if ( fabsf( target - current ) < 0.001f )
        return target;   /* settled: do not stamp; slot evicts via seen_frame */

    f32 dt   = s_io.dt > 0.0001f ? s_io.dt : 0.0001f;
    current += ( target - current ) * ( 1.0f - expf( -speed * dt ) );

    IMGUI_STATE( imgui_anim_f32_t, anim_id )->current = current;
    s_retained.wants_redraw = true;
    return current;
}

/*----------------------------------------------------------------------------------------------
    imgui_anim_bg -- hover/active background blend for button-like widgets

    Smoothly blends COL_WIDGET_BG -> COL_WIDGET_HOT on hover/nav and -> COL_WIDGET_ACT on
    press.  Both channels share one 8-byte slot keyed via an internal salt, so the widget needs
    no knowledge of animation storage.

    Returns the ABGR background color to pass to draw_push_rect_filled.

    Fast path: an idle widget with no prior animation history returns COL_WIDGET_BG with
    zero state-pool operations.
----------------------------------------------------------------------------------------------*/

typedef struct { f32 t_hot; f32 t_active; } imgui_hover_anim_t;

#define ANIM_TAG_BG  0xA501u   /* id_combine salt; keeps this slot distinct from all other per-widget state */

static u32
imgui_anim_bg( imgui_id_t id, widget_state_t st )
{
    imgui_id_t                anim_id    = id_combine( id, ANIM_TAG_BG );
    bool                      needs_anim = st.hover || st.nav || st.active;
    const imgui_hover_anim_t* peek       = (const imgui_hover_anim_t*)imgui_state_peek( anim_id );

    if ( !needs_anim && !peek )
        return COL_WIDGET_BG;

    f32 hot_t    = peek ? peek->t_hot    : 0.0f;
    f32 active_t = peek ? peek->t_active : 0.0f;
    f32 dt       = s_io.dt > 0.0001f ? s_io.dt : 0.0001f;

    f32 hot_tgt = ( st.hover || st.nav ) ? 1.0f : 0.0f;
    f32 act_tgt = st.active ? 1.0f : 0.0f;

    f32 new_hot = fabsf( hot_tgt - hot_t ) < 0.001f
                ? hot_tgt
                : hot_t + ( hot_tgt - hot_t ) * ( 1.0f - expf( -10.0f * dt ) );
    f32 new_act = fabsf( act_tgt - active_t ) < 0.001f
                ? act_tgt
                : active_t + ( act_tgt - active_t ) * ( 1.0f - expf( -20.0f * dt ) );

    bool settled = ( new_hot == hot_tgt ) && ( new_act == act_tgt );

    if ( !settled || needs_anim )
    {
        imgui_hover_anim_t* s = IMGUI_STATE( imgui_hover_anim_t, anim_id );
        s->t_hot    = new_hot;
        s->t_active = new_act;
        if ( !settled ) s_retained.wants_redraw = true;
    }
    /* settled && !needs_anim: do not stamp -- slot evicts via seen_frame within 1-2 frames. */

    return col_lerp( col_lerp( COL_WIDGET_BG, COL_WIDGET_HOT, new_hot ), COL_WIDGET_ACT, new_act );
}

// clang-format on
/*============================================================================================*/
