/*==============================================================================================

    runtime_service/gui/3_widgets/gui_volatile.c -- Volatile widgets, UI-unit half.

    A "volatile" callback contains ordinary UI emit calls (text, colored rects, etc).  It runs
    inline during a real (dirty) frame via gui()->volatile_cb -- its widgets render exactly like
    any other code, no special behavior.  On an idle frame (frame_begin returned false, no
    ctx_begin/emit ran), gui_frame_end calls gui_update_volatile internally; the backend (BUILD unit,
    backend/pipeline/gui_build_volatile.c) re-invokes the same callback standalone, re-tessellates its
    output, and patches it into the padded region reserved for the block inside its window's
    cached geometry (any output that fits is accepted; only outgrowing the reservation costs a
    real frame) -- see gui.h (gui_volatile_fn) for the full contract and gui_backend.h for the
    unit-seam declarations shared with the backend half.

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
            s_interaction) so widget_behavior (2_interact/gui_item.c) can read it inline; this file
            is the only place that sets it.

    Included by gui.c after gui_widget.c -- needs lf() / layout_frame_t (gui_ctx.c,
    2_compose/gui_layout_core.c), id_push/id_pop (0_foundation/gui_id.c), and layout_set_default
    (2_compose/gui_layout_core.c) already in scope.

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    Volatile widget callback -- see gui.h (gui_volatile_fn) for the full contract.
----------------------------------------------------------------------------------------------*/

/* gui_volatile_cb wraps one real-emit invocation of `fn` so the backend can bracket the exact
   command range it produces (gui_volatile_cb_open/_close, gui_backend.h); the callback itself
   calls gui_volatile_begin/end from inside its own body, per the caller's own code -- begin
   stamps the layout cursor position the callback started at (needed to reconstruct a matching
   scope on replay), end is a reserved no-op (see gui_volatile_end).  `label` is hashed the same way widget_id() hashes a
   widget label (id_combine(id_seed(), id_hash(label))) -- callers pass an ordinary string, same as
   any other widget call, rather than manufacturing their own gui_id_t. */
void
gui_volatile_cb( const char* label, gui_volatile_fn fn )
{
    gui_id_t id = id_combine( id_seed(), id_hash( label ) );
    gui_volatile_cb_open( id );
    fn( false );
    gui_volatile_cb_close( fn );
}

void
gui_volatile_begin( void )
{
    /* s_build.item_flags (the begin_disabled/end_disabled stack) and the style push/pop stacks
       (s_col_sp/s_var_sp, gui_style.c) are NOT part of the seam gui_replay_scope_enter reconstructs
       on an idle-frame replay -- only the id scope and a minimal layout frame are.  ctx_new_frame
       resets both to empty at the start of every REAL frame (style_new_frame, gui_ctx.c's
       frame_begin), so by the time an idle frame's gui_update_volatile re-invokes this callback
       standalone, they read back as "nothing pushed" regardless of what ancestor
       begin_disabled()/push_style_color()/push_style_var() scope this volatile_cb call was
       actually nested in at real emit.  draw_set_alpha/draw_set_rounding (item_flags_resolve) and
       style_col/style_var would then silently resolve to the wrong value on replay -- and nothing
       downstream can catch it, since the patched geometry is valid either way.  A callback is only safe to
       replay if it does not depend on inherited disabled/style-push state from its call site;
       assert here so a violation is caught at the call site immediately instead of shipping a
       widget that quietly un-dims or re-colors itself on idle frames. */
    ORB_ASSERT( s_build.item_flags == GUI_ITEM_NONE
             && "gui_volatile_cb: callback runs under an ambient begin_disabled() scope -- "
                "not reproduced on idle-frame replay" );
    ORB_ASSERT( s_col_sp == 0 && s_var_sp == 0
             && "gui_volatile_cb: callback runs under an ambient push_style_color/var() scope -- "
                "not reproduced on idle-frame replay" );

    layout_frame_t* f = lf();
    gui_volatile_stamp( f->content_x, f->content_y, f->content_w );
}

void
gui_volatile_end( void )
{
    /* FUTURE: per-command-type stamping; currently a no-op (paired bookend to gui_volatile_begin). */
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
    gui_backend.h).  gui_update_volatile (backend/pipeline/gui_build_volatile.c) calls these around each
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
