/*==============================================================================================

    runtime_service/gui/interact/gui_feature.c -- Window features as freestanding id-keyed
    mechanisms (GUI_STACK_PLAN section 6): the feat_* kit.

    Every entry here is a MECHANISM, not a widget: inputs are (id, rect, io) plus
    caller-owned state pointers, so anything can be a move handle, a collapse, or a maximize
    -- a pane with hand-built chrome composes them exactly like the stock window does
    (gui_window_free.c is the reference recipe: its clamp, collapse tween, and three-state
    maximize / shelf-chip rect channel all resolve here).  The
    state rule of the kit: IN-FLIGHT gesture state is a service singleton arbitrated by
    active_id (one drag at a time -- the house pattern); PERSISTENT state is the CALLER's
    pointer (x/y, rect, open bool, restore rect) -- you see every byte.  Only the tween
    bookkeeping (edge detect + from-value) lives in the keyed pool, since it is animation
    scratch, not document state.

    Dependency classes (the section-6 table): move / resize / collapse are A (freestanding:
    id + rect + io only); maximize / clamp are B (the WORK AREA is passed IN -- the feature
    never finds the viewport itself).  raise/shelf (population) and titlebar (composite)
    are not mechanisms and stay with chrome.

    The move and resize gestures are THE existing services (gui_move.c / gui_resize.c --
    already record-agnostic; windows, children, and floating dock groups share them); this
    file only gives them their public feat_ form.  Hover gating reads the ambient
    interaction scope (s_scope.win), so call these INSIDE the owning pane_begin / your
    window bracket.

    Included by gui_interact.c (the interact unit), after gui_move.c and gui_resize.c -- the
    feat_ verbs are their public form; the tweens ride the interact server's gui_anim_timer
    across the core/gui_core.h seams.

==============================================================================================*/
// clang-format off

/* Gesture ids are salted from the caller's id so a feature never collides with a widget or
   another feature on the same id.  (The resize service salts internally with GUI_RESIZE_SALT.) */
#define GUI_FEAT_MOVE_SALT      0x0FEA7800u
#define GUI_FEAT_COLLAPSE_SALT  0x0FEA7801u
#define GUI_FEAT_MAX_SALT       0x0FEA7802u

/* Tween feel: THE window-state animation constants.  The stock window's collapse and
   maximize / minimize transitions and the dock's maximize ease all ride these (via
   feat_collapse / feat_pin below, or directly), honoring the global window_anim_enable
   preference -- feature-built chrome and stock chrome move identically by construction. */
/* FEAT_ANIM_SECS moved to gui_internal.h at the TU split (inc 10). */

f32 feat_ease( f32 t ) { return f32_ease_out_cubic( t ); }

/* Liveness peek for a feature's tween timer: reads the slot WITHOUT advancing the clock (a
   second gui_anim_timer sample in the same frame would double-step elapsed).  duration is
   zeroed by the sample that crosses the end, so this reads false from that frame on --
   the same frame the sampling caller sees active drop. */
static bool
feat_timer_live( gui_id_t salted_id )
{
    const gui_anim_timer_t* pk = GUI_STATE_PEEK( gui_anim_timer_t, salted_id );
    return pk && pk->duration > 0.0f;
}

/*==============================================================================================
    feat_move (A) -- a drag handle over any rect.

    Call every frame with the handle rect and the caller-owned origin.  While the caller's
    pane/window owns hover, a press in the handle arms the deferred-press latch (click vs
    drag: a release before the threshold stays a click, so the handle can also host buttons
    or a double-click); crossing it grabs the drag, and the origin then follows the cursor
    with the grabbed point pinned.  Returns true on frames the origin moved.
==============================================================================================*/

bool
gui_feat_move( gui_id_t id, gui_rect_t handle, f32* x, f32* y )
{
    gui_id_t mid = id_combine( id, GUI_FEAT_MOVE_SALT );

    /* In-flight: follow the cursor through the grab offset. */
    if ( s_interaction.active_id == mid )
    {
        f32 nx, ny;
        if ( move_track( mid, s_io.mouse_x, s_io.mouse_y, &nx, &ny )
             && ( nx != *x || ny != *y ) )
        {
            *x = nx;
            *y = ny;
            gui_request_redraw();   /* caller-owned position: the cache cannot see it change */
            return true;
        }
        return false;
    }

    /* Arm on a press in the handle while our scope owns hover and nothing else is active. */
    if ( s_interaction.hover_win == s_scope.win
         && s_interaction.active_id == GUI_ID_NONE
         && s_io.mouse_pressed[ 0 ]
         && rect_hit( handle ) )
        press_defer_arm( mid );

    /* Polled OUTSIDE the hover gate (the press-defer contract): sliding off the handle
       mid-press must not strand the gesture. */
    if ( press_defer_crossed( mid ) )
        move_grab( mid, 0, *x, *y );

    return false;
}

/*==============================================================================================
    feat_resize (A) -- edge-resize any rect.

    One call per frame over the caller-owned rect: hit-tests the grab band, shows the
    directional cursor, grabs on press, and while dragging writes the new geometry back
    through *r, floored at min_w/min_h against the grabbed edge's pinned far side.  `edges`
    masks which sides this caller exposes (GUI_RESIZE_L/R/T/B; corners fall out of adjacent
    bits).  Returns the edges live this frame (hot or dragging) -- feed it to a highlight if
    wanted.  The gesture core is the same edge-resize service windows and children ride.
==============================================================================================*/

u8
gui_feat_resize( gui_id_t id, gui_rect_t* r, u8 edges, f32 min_w, f32 min_h )
{
    bool dragging = false;
    u8   live     = resize_item( id, s_scope.win, *r, (u8)( edges & 0x0Fu ), false, &dragging );

    if ( dragging && live )
    {
        resize_apply_edges( r, live );

        /* Min floor against the pinned far edge: an L/T drag shrinks toward its fixed
           right/bottom, so the floor re-anchors the origin, not the size alone. */
        f32 right = r->x + r->w;
        f32 bot   = r->y + r->h;
        if ( r->w < min_w ) { r->w = min_w; if ( live & GUI_RESIZE_L ) r->x = right - min_w; }
        if ( r->h < min_h ) { r->h = min_h; if ( live & GUI_RESIZE_T ) r->y = bot   - min_h; }

        gui_request_redraw();   /* caller-owned rect: the cache cannot see it change */
    }
    return live;
}

/*==============================================================================================
    feat_collapse (A) -- a tweened height channel over a caller-owned bool.

    Returns the height to use this frame: full_h while open, head_h while closed, eased
    between on the frames after *the caller* toggles `open` (the mechanism never decides to
    collapse -- your button does).  Only the tween scratch (last height + edge latch) lives
    in the keyed pool; the open bool is yours.
==============================================================================================*/

typedef struct
{
    f32 last_h;     // height returned last frame -- the tween's from-value on a toggle
    f32 from;       // captured at the toggle edge
    u8  was_open;   // edge detect against the caller's bool
    u8  seen;       // first-call seed guard (a fresh / evicted slot snaps, never tweens)

} gui_feat_collapse_t;

f32
gui_feat_collapse( gui_id_t id, bool open, f32 head_h, f32 full_h )
{
    gui_id_t             cid = id_combine( id, GUI_FEAT_COLLAPSE_SALT );
    gui_feat_collapse_t* st  = GUI_STATE( gui_feat_collapse_t, cid );

    f32 target = open ? full_h : head_h;

    if ( !st->seen )
    {
        st->seen     = 1;
        st->was_open = (u8)open;
        st->last_h   = target;
        return target;
    }

    if ( (bool)st->was_open != open )   /* the caller toggled: start the height tween */
    {
        st->was_open = (u8)open;
        st->from     = st->last_h;
        gui_anim_timer_start( cid, gui_window_anim_is_enabled() ? FEAT_ANIM_SECS : 0.0f );
    }

    bool active = false;
    f32  t      = gui_anim_timer( cid, feat_ease, &active );   /* holds wants_redraw while live */
    f32  h      = active ? f32_lerp( st->from, target, t ) : target;

    st->last_h = h;
    return h;
}

/* Is id's collapse height tween in flight this frame?  A recipe gate (the stock window
   holds edge-resize armability and the autosize refit off while the body height eases). */
bool
feat_collapse_live( gui_id_t id )
{
    return feat_timer_live( id_combine( id, GUI_FEAT_COLLAPSE_SALT ) );
}

/*==============================================================================================
    feat_pin -- the state-pinned rect channel (the core under feat_maximize).

    `state` is the caller's pinned-state ordinal: 0 = normal (the CALLER owns the rect --
    move / resize mutate it freely and this touches nothing), nonzero = pinned to `target`,
    which the caller recomputes every frame so a settled pin TRACKS a live target (surface
    resize, shelf reorder) by writing it straight through.  A state CHANGE tweens: entering
    a pin from normal saves *restore first; moving between two pinned states re-aims without
    touching the save (the first pin's save survives a maximize -> minimize hop); returning
    to 0 tweens back to *restore, the mechanism owning the rect until it lands (the
    restoring latch).  Returns true while a tween eases -- the caller's cue to suppress its
    own rect gestures so a half-finished ease is never fought.

    Internal: the public form is gui_feat_maximize's bool below; the stock window recipe
    (gui_window_free.c) calls this directly with its three states (normal / maximized /
    shelf chip) -- the chip TARGET (population, order) stays chrome, only the rect channel
    lives here.
==============================================================================================*/

typedef struct
{
    gui_rect_t last;        // rect seen last frame -- the tween's from-value on a toggle
    gui_rect_t from;        // captured at the toggle edge
    u32        was_state;   // edge detect against the caller's state ordinal
    u8         seen;        // first-call seed guard
    u8         restoring;   // restore tween in flight -- the mechanism still owns the rect

} gui_feat_pin_t;

static gui_rect_t
feat_rect_lerp( gui_rect_t a, gui_rect_t b, f32 t )
{
    return ( gui_rect_t ){ f32_lerp( a.x, b.x, t ), f32_lerp( a.y, b.y, t ),
                           f32_lerp( a.w, b.w, t ), f32_lerp( a.h, b.h, t ) };
}

bool
feat_pin( gui_id_t id, u32 state, gui_rect_t* r, gui_rect_t* restore, gui_rect_t target )
{
    gui_id_t        pid = id_combine( id, GUI_FEAT_MAX_SALT );
    gui_feat_pin_t* st  = GUI_STATE( gui_feat_pin_t, pid );

    if ( !st->seen )
    {
        st->seen      = 1;
        st->was_state = state;
        if ( state )
            *r = target;                    /* seeded already-pinned: snap to the pin */
        st->last = *r;
        return false;
    }

    if ( st->was_state != state )           /* the caller toggled */
    {
        if ( !st->was_state )
            *restore = *r;                  /* leaving normal: save the rect to come back to */
        st->was_state = state;
        st->restoring = (u8)( state == 0 ); /* going down: keep owning the rect until landed */
        st->from      = st->last;
        gui_anim_timer_start( pid, gui_window_anim_is_enabled() ? FEAT_ANIM_SECS : 0.0f );
    }

    /* Steady normal state: the CALLER owns the rect (move/resize mutate it freely) -- the
       mechanism must not touch it, or it would stomp those edits with a stale restore. */
    if ( !state && !st->restoring )
    {
        st->last = *r;
        return false;
    }

    gui_rect_t goal = state ? target : *restore;

    bool active = false;
    f32  t      = gui_anim_timer( pid, feat_ease, &active );

    *r       = active ? feat_rect_lerp( st->from, goal, t ) : goal;
    st->last = *r;

    if ( !state && !active )
        st->restoring = 0;                  /* restore landed: hand the rect back */

    return active;
}

/*==============================================================================================
    feat_maximize (B) -- rect <-> work-area swap over a caller-owned bool + restore slot.

    The work area is passed IN (the B rule: the feature never finds the viewport).  On the
    caller's toggle to maximized the current rect is saved into *restore and the rect tweens
    to `work`; on the way back it tweens to *restore.  While maximized the rect tracks a
    changing work area (viewport resize).  Sugar over feat_pin above -- two states.
==============================================================================================*/

void
gui_feat_maximize( gui_id_t id, bool maximized, gui_rect_t* r, gui_rect_t* restore,
                   gui_rect_t work )
{
    feat_pin( id, maximized ? 1u : 0u, r, restore, work );
}

/*==============================================================================================
    feat_clamp (B) -- keep a dragged rect's grab handle reachable inside the work area.

    Pure geometry over passed-in bounds -- THE boundary policy the stock window rides too
    (window_clamp wraps this with its viewport + work-top resolve): the handle row can never
    slide above the work area's top (where the grab would be lost under viewport chrome),
    and at every other edge a `margin` sliver must remain on screen.
==============================================================================================*/

void
gui_feat_clamp( gui_rect_t* r, gui_rect_t work, f32 margin )
{
    if ( r->x > work.x + work.w - margin ) r->x = work.x + work.w - margin;
    if ( r->y > work.y + work.h - margin ) r->y = work.y + work.h - margin;
    if ( r->x < work.x + margin - r->w )   r->x = work.x + margin - r->w;
    if ( r->y < work.y )                   r->y = work.y;
}

// clang-format on
/*============================================================================================*/
