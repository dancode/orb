/*==============================================================================================

    runtime_service/gui/core/gui_anim.c -- Animation stepping service.

    The value-stepping primitive behind every smoothed transition: peek-guard logic, channel
    stepping, slot stamping, and wants_redraw signalling in one function, so callers stay
    simple.  Pure timing -- what a stepped value MEANS (a color blend, an eased extent) is the
    caller's business: the animated background color lives with the skin
    (style/gui_style_core.c: col_item_bg_anim), the size ease with the layout engine
    (flow/gui_layout_core.c: size_animate).

    Storage lives in the keyed state pool (core/gui_state.c) with peek-then-stamp semantics:
    pool pressure stays proportional to in-flight animations, not total widget count.  Idle
    values with no animation history pay only a single non-stamping probe.

    Core-unit resident; flow-and-above callers reach it through the forward
    declaration in core/gui_core.h.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Single-channel exponential-decay damper -- gui_anim_f32_from, and gui_anim_f32 over it

    Steps a named float toward `target` each frame.  `speed` is in Hz-like units:
    10  ~  250 ms to 95% of target
    20  ~  150 ms to 95% of target

    Peek-then-stamp: the pool is not touched when the value has no history or has already
    settled.  Compose anim_id via id_combine( item_id, tag ) so each channel occupies its
    own slot without colliding with other per-widget state:

        f32 t = gui_anim_f32( id_combine( id, 1u ), hovered ? 1.0f : 0.0f, 10.0f );

    The two differ only in what a channel with NO history reads as.  gui_anim_f32 assumes it is
    already AT its target -- correct when a first appearance should not animate (size_animate: a
    widget shows at its natural size and eases only on later changes).  A transient highlight is
    the opposite: `rest` is its natural start and the target is the passing state, so first sight
    must ramp FROM rest.  gui_anim_f32 is just the _from variant with rest == target; give
    hover/press/focus channels rest = their off value (0).
==============================================================================================*/

typedef struct { f32 current; } gui_anim_f32_t;
f32
gui_anim_f32_from( gui_id_t anim_id, f32 rest, f32 target, f32 speed )
{
    const gui_anim_f32_t* peek = GUI_STATE_PEEK( gui_anim_f32_t, anim_id );
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

f32
gui_anim_f32( gui_id_t anim_id, f32 target, f32 speed )
{
    return gui_anim_f32_from( anim_id, target, target, speed );
}

/*==============================================================================================
    gui_anim_track -- ease a value that SITS STILL between changes

    The two above are built for values that return to a rest: a hover weight, a press weight.
    Their slot is deliberately dropped once the value settles AT rest, because reseeding from
    rest reproduces it exactly, so the eviction is invisible and idle UI holds nothing.

    A MEASURED quantity is the opposite shape and that rule breaks it.  A natural column's width
    or a box's height settles and then stays there for thousands of frames -- with rest == target
    (gui_anim_f32) the slot stops being stamped, ages out, and the next change reseeds from the
    NEW value, which is a snap.  The damper is not wrong; it simply has no history left to ease
    FROM.  That is not a tuning problem, it is why an eased size can look like it never eased.

    So this one ALWAYS stamps.  It costs one retained float per animated quantity for as long as
    the caller keeps asking -- the honest price of easing something that is otherwise static --
    and it never ramps in from nothing: with no history it ADOPTS the target, so a first
    appearance lands at its size instead of growing into it.
==============================================================================================*/

f32
gui_anim_track( gui_id_t anim_id, f32 target, f32 speed )
{
    const gui_anim_f32_t* peek = GUI_STATE_PEEK( gui_anim_f32_t, anim_id );
    f32 current = peek ? peek->current : target;   /* no history: adopt, never ramp in */

    if ( fabsf( target - current ) >= 0.001f )
    {
        f32 dt = s_io.dt > 0.0001f ? s_io.dt : 0.0001f;
        current += ( target - current ) * ( 1.0f - expf( -speed * dt ) );
        g_ctx->retained.wants_redraw = true;   /* value changed this frame -- keep frames coming */
    }
    else
    {
        current = target;   /* settled: snap exactly onto the target */
    }

    GUI_STATE( gui_anim_f32_t, anim_id )->current = current;
    return current;
}

/*==============================================================================================
    gui_anim4 -- four independent damper channels in ONE keyed slot

    The storage unit for any animated widget value.  gui_anim_f32_from keys one probe per channel, so
    an N-channel widget costs N peeks and N pool entries; this packs four channels behind a single key
    -- one peek, one stamp, one 16-byte slot -- so a two-channel hover/active blend, a four-channel
    RGBA color, or an x/y/w/h rect each cost exactly one probe, and spare channels are free scratch.

    Per channel: seed from rest on first sight, damp toward target at speed (Hz-like; see gui_anim_f32).
    speed <= 0 SNAPS the channel to target -- the "no animation" path, and the safe way to leave an
    unused channel (rest == target == 0 then costs nothing and never pins wants_redraw).  The whole
    slot persists while ANY channel is moving or parked away from its rest, and goes cold only when all
    four sit at rest, so a held non-rest value (a steady hover) never re-ramps after an eviction.
==============================================================================================*/

gui_anim4_t
gui_anim4( gui_id_t id, gui_anim4_t rest, gui_anim4_t target, gui_anim4_t speed )
{
    const gui_anim4_t* peek = GUI_STATE_PEEK( gui_anim4_t, id );
    gui_anim4_t        cur  = peek ? *peek : rest;
    f32                dt   = s_io.dt > 0.0001f ? s_io.dt : 0.0001f;

    /* gui_anim4_t is four contiguous floats -- index the channels through a flat view. */
    f32*       c = (f32*)&cur;
    const f32* r = (const f32*)&rest;
    const f32* t = (const f32*)&target;
    const f32* s = (const f32*)&speed;

    bool moved = false, persist = false;
    for ( u32 i = 0; i < 4; ++i )
    {
        if ( s[ i ] <= 0.0f )                          /* instant / unused channel: snap */
            c[ i ] = t[ i ];
        else if ( fabsf( t[ i ] - c[ i ] ) >= 0.001f ) /* moving: one damper step */
        {
            c[ i ] += ( t[ i ] - c[ i ] ) * ( 1.0f - expf( -s[ i ] * dt ) );
            moved = true;
        }
        else                                           /* settled: snap exactly onto target */
            c[ i ] = t[ i ];

        if ( fabsf( c[ i ] - r[ i ] ) >= 0.001f )      /* away from rest -> keep the slot warm */
            persist = true;
    }

    if ( moved )
        g_ctx->retained.wants_redraw = true;
    if ( moved || persist )
        *GUI_STATE( gui_anim4_t, id ) = cur;
    return cur;
}

/*==============================================================================================
    gui_anim_timer -- fixed-duration from/to easing (the timed-tween model)

    The complement to gui_anim_f32's exponential damping: instead of chasing a moving target at a
    decay rate, this runs a normalized clock 0 -> 1 over a fixed duration and hands back an eased
    progress the caller lerps its OWN from/to values with.  Two properties the damper cannot give:

      - Shared clock.  Several channels driven off ONE timer id (a rect's x/y/w/h, say) all read the
        same t, so they depart and ARRIVE together -- no channel lags because its delta was smaller.
      - Definite end.  A damper only ever approaches its target; a timer reaches t == 1 exactly on
        the frame the duration elapses, FORCES that end, reports "done", and evicts.  The caller can
        then snap to its target and drop its own animating flag with no risk of a stuck mid-tween.

    Storage is a single keyed slot (elapsed + duration).  gui_anim_start seeds it; each
    gui_anim_timer call advances and shapes it through `ease` (any f32 shaper: f32_ease_out_cubic,
    f32_smoothstep01, ... or NULL for linear).  A slot that is absent or already finished reads as
    settled at 1.0, so a caller that samples one extra frame still sees the end, never a restart.
==============================================================================================*/

/* gui_anim_timer_t (the timer slot payload) lives in core/gui_core.h: the feat_* kit
   (interact/gui_feature.c) peeks the slot across the unit seam. */

/* Start (or restart) a duration-based timer on `id`: clock at 0, running for `duration` seconds.  A
   zero / negative duration is the "no animation" request -- no slot is stamped, so the first
   gui_anim_timer reads settled and the caller snaps straight to its target (the toggle-off path). */
void
gui_anim_start( gui_id_t id, f32 duration )
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
f32
gui_anim_timer( gui_id_t id, gui_ease_fn ease, bool* out_active )
{
    const gui_anim_timer_t* pk = GUI_STATE_PEEK( gui_anim_timer_t, id );
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

/*==============================================================================================
    Public animation surface (gui_api_t)

    The two primitives above are the whole engine; these are the callable face of them.  anim_f32 and
    anim_start are the primitives verbatim (same signature, assigned straight onto the vtable in
    gui_api.c).  The rest are thin: anim_ease maps the public gui_ease_t enum onto a shaper before
    calling gui_anim_timer; the typed channels run one damper per component so a color, a point, or a
    rect glides to a new data state without every caller re-deriving the blend.
==============================================================================================*/

/* Map the public enum onto the base math_ease shapers.  NULL == linear (gui_anim_timer treats a NULL
   shaper as the identity), so GUI_EASE_LINEAR and any out-of-range value fall through to it. */
static gui_ease_fn
ease_lookup( gui_ease_t e )
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
   anim_start (== gui_anim_start) which seeds the clock; *out_active is false once settled. */
f32
gui_anim_ease( gui_id_t id, gui_ease_t ease, bool* out_active )
{
    return gui_anim_timer( id, ease_lookup( ease ), out_active );
}

/* The typed channels are all "chase" animations -- glide to a new data state, no intro from a rest
   value -- so each rides gui_anim4 with rest == target (snap on first sight, then damp on change) at
   one uniform speed across its channels.  One peek / one 16-byte slot apiece; unused channels (vec2's
   z/w) sit at 0 with rest == target and cost nothing. */

/* Damped ABGR blend: the four color channels glide to target_abgr at `speed` (Hz-like, gui_anim_f32). */
u32
gui_anim_color( gui_id_t id, u32 target_abgr, f32 speed )
{
    gui_anim4_t tgt = { (f32)( ( target_abgr ) & 0xFF ), (f32)( ( target_abgr >> 8 ) & 0xFF ),
                        (f32)( ( target_abgr >> 16 ) & 0xFF ), (f32)( ( target_abgr >> 24 ) & 0xFF ) };
    gui_anim4_t sp  = { speed, speed, speed, speed };
    gui_anim4_t o   = gui_anim4( id, tgt, tgt, sp );
    return (u32)( o.x + 0.5f ) | ( (u32)( o.y + 0.5f ) << 8 ) | ( (u32)( o.z + 0.5f ) << 16 ) | ( (u32)( o.w + 0.5f ) << 24 );
}

/* Damped 2D point: x/y glide to target; z/w unused. */
gui_vec2_t
gui_anim_vec2( gui_id_t id, gui_vec2_t target, f32 speed )
{
    gui_anim4_t tgt = { target.x, target.y, 0.0f, 0.0f };
    gui_anim4_t sp  = { speed, speed, 0.0f, 0.0f };
    gui_anim4_t o   = gui_anim4( id, tgt, tgt, sp );
    return ( gui_vec2_t ){ o.x, o.y };
}

/* Damped rect: x/y/w/h glide together (position + extent) in one slot. */
gui_rect_t
gui_anim_rect( gui_id_t id, gui_rect_t target, f32 speed )
{
    gui_anim4_t tgt = { target.x, target.y, target.w, target.h };
    gui_anim4_t sp  = { speed, speed, speed, speed };
    gui_anim4_t o   = gui_anim4( id, tgt, tgt, sp );
    return ( gui_rect_t ){ o.x, o.y, o.z, o.w };
}

// clang-format on
/*============================================================================================*/
