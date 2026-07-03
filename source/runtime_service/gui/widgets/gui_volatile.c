/*==============================================================================================

    runtime_service/gui/widgets/gui_volatile.c -- Volatile widgets, UI-unit half.

    A "volatile" callback contains ordinary UI emit calls (text, colored rects, etc).  It runs
    inline during a real (dirty) frame via gui()->volatile_cb -- its widgets render exactly like
    any other code, no special behavior.  On an idle frame (frame_dirty()==false), the host calls
    gui()->update_volatile() instead of ctx_begin/emit/ctx_end; the backend (BUILD unit,
    backend/gui_build_volatile.c) re-invokes the same callback standalone and, if the replay
    reproduces the same command topology real emit recorded, patches the geometry in place --
    see gui.h (gui_volatile_fn) for the full contract and gui_backend.h for the unit-seam
    declarations shared with the backend half.

    Everything in THIS file is the UI-unit side of the seam:

        gui_volatile_cb / gui_volatile_begin / gui_volatile_end
            The public API (gui_api.h vtable: volatile_cb / volatile_begin / volatile_end).
            gui_volatile_cb wraps one inline invocation of the caller's callback so the backend
            can bracket the exact command range it produces; the callback itself calls
            volatile_begin/end from inside its own body to stamp the layout cursor position.

        layout_push_scoped / layout_pop_scoped
            A minimal layout-frame push/pop at an explicit (x, y, w) -- lighter than
            layout_push_region (no scrollbar gutter, no clip push, no id_push): the replay scope
            gui_replay_scope_enter/_exit installs around a standalone callback invocation.

        gui_replay_scope_enter / gui_replay_scope_exit
            The reverse half of the unit seam: gui_update_volatile (backend unit) calls these
            around each row's replay so the callback's ordinary gui()->text()/rect_filled()/...
            calls have a valid (if minimal) layout frame and id scope to emit into, without
            running ctx_begin/ctx_new_frame or touching anything else about the real frame's UI
            state.  s_replay_mode itself lives in gui_ctx.c (ambient state, same tier as
            s_interaction) so widget_behavior (gui_widget_core.c) can read it inline; this file
            is the only place that sets it.

    Included by gui.c after gui_widget_draw.c -- needs lf() / layout_frame_t (gui_ctx.c,
    gui_layout_core.c), id_push/id_pop (gui_ctx_id.c), and layout_set_default
    (gui_layout_core.c) already in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Volatile widget callback -- see gui.h (gui_volatile_fn) for the full contract.
----------------------------------------------------------------------------------------------*/

/* gui_volatile_cb wraps one real-emit invocation of `fn` so the backend can bracket the exact
   command range it produces (gui_volatile_cb_open/_close, gui_backend.h); the callback itself
   calls gui_volatile_begin/end from inside its own body, per the caller's own code -- begin
   stamps the layout cursor position the callback started at (needed to reconstruct a matching
   scope on replay), end is reserved for now.  `id` must be stable across frames -- widget_id(),
   or any other hash the caller keeps constant call to call. */
void
gui_volatile_cb( gui_id_t id, gui_volatile_fn fn )
{
    gui_volatile_cb_open( id );
    fn( false );
    gui_volatile_cb_close( fn );
}

void
gui_volatile_begin( void )
{
    layout_frame_t* f = lf();
    gui_volatile_stamp( f->content_x, f->content_y, f->content_w );
}

void
gui_volatile_end( void )
{
    /* Reserved for future per-command-type stamping; no-op in v1. */
}

/*----------------------------------------------------------------------------------------------
    layout_push_scoped / layout_pop_scoped -- a minimal layout frame at an explicit (x, y, w),
    used only by gui_replay_scope_enter/_exit below.  Unlike layout_push_region this reserves no
    scrollbar gutter, pushes no clip, and calls no id_push -- the caller handles id scoping
    itself.  layout_set_default installs a plain single-column stack and resets the
    modifier/template state, so a widget can be placed immediately without tripping the
    emit-before-header guard in widget_next_rect_w.  content_y_max is set far below y since a
    replay frame never opens a grid.
----------------------------------------------------------------------------------------------*/

static void
layout_push_scoped( f32 x, f32 y, f32 w )
{
    u32 slot = s_layout_sp < GUI_LAYOUT_DEPTH ? s_layout_sp : GUI_LAYOUT_DEPTH - 1;
    ++s_layout_sp;
    layout_frame_t* f = &s_layout_stack[ slot ];

    f->content_x     = x;
    f->content_y     = y;
    f->content_w     = w;
    f->content_max_x = x;
    f->content_max_y = y;
    f->content_y_max = y + 1.0e6f;

    layout_set_default( f );
}

/* Pop a scope opened by layout_push_scoped -- no measurement, no scrollbar draw, just unwind
   the stack pointer (the replay path never scrolls or reports content size). */
static void
layout_pop_scoped( void )
{
    if ( s_layout_sp > 0 ) --s_layout_sp;
}

/*----------------------------------------------------------------------------------------------
    gui_replay_scope_enter / _exit -- the reverse half of the volatile-widget seam (see
    gui_backend.h).  gui_update_volatile (backend/gui_build_volatile.c) calls these around each
    row's standalone replay invocation so the callback's ordinary gui()->text()/rect_filled()/...
    calls have a valid (if minimal) layout frame and id scope to emit into, without running
    ctx_begin/ctx_new_frame or touching anything else about the real frame's UI state.
----------------------------------------------------------------------------------------------*/

void
gui_replay_scope_enter( gui_id_t id, f32 x, f32 y, f32 w )
{
    id_push( id );
    layout_push_scoped( x, y, w );
    s_replay_mode = true;
}

void
gui_replay_scope_exit( bool force_redraw )
{
    layout_pop_scoped();
    id_pop();
    s_replay_mode = false;
    if ( force_redraw )
        s_retained.wants_redraw = true;
}

// clang-format on
/*============================================================================================*/
