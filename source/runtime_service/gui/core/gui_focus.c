/*==============================================================================================

    runtime_service/gui/core/gui_focus.c -- Keyboard focus: who owns typed input.

    The whole focus story in one file: the exclusive input MODE that scopes it, the two rules
    that make focus behave like a menu selection, the programmatic request latch, and the
    release verb the edit engines call.  The focused_id / focused_win fields themselves live in
    the ambient record (s_interaction, core/gui_ctx.c) with the rest of hover / active; this
    file owns the POLICY over them.

    Who writes focused_id, and where:

      item_state (core/gui_item.c)  -- a press on a focusable item, a nav activation, and a
                                       granted focus_request_take: the CLAIM paths, all gated
                                       by focus_allowed.
      focus_release (here)          -- Enter commit / Escape revert: the widget drops its own
                                       capture.
      interaction_frame_reset       -- a press on dead space drops focus, unless the exclusive
        (core/gui_ctx.c)               mode holds it (focus_scope_holds).

    Included by gui_core.c after core/gui_ctx.c (it reads the ambient record and g_ctx->modal)
    and before core/gui_item.c, the caller of focus_allowed / focus_request_take.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Exclusive input mode (focus scope) -- the game-menu model of UI focus.

    A GUI_WIN_MODAL window is an exclusive input MODE, the immediate-mode analogue of a game's
    menu screen: while it is up it owns interaction (the hover fence, window_modal_apply) AND
    keyboard focus.  Two rules make focus behave like a menu selection rather than a desktop
    caret:

      confine  -- only the mode's own widgets may TAKE focus (focus_allowed below); no
                  background window can steal it.
      hold     -- focus is STICKY within the mode: it never falls to nothing.  Focus can drop two
                  ways and the mode blocks both -- a press on non-focusable dead space
                  (interaction_frame_reset, core/gui_ctx.c) and a widget releasing its own
                  capture on Enter / Escape (focus_release below).  You cannot "select nothing"
                  inside a menu; focus only MOVES when another focusable widget in the mode claims
                  it (a direct focused_id overwrite, not a release), so the console input keeps its
                  caret across dead-space clicks AND across every command it submits.

    Both key off modal.win_id + seen_frame, exactly like the hover fence -- one exclusive-mode
    fact, read three ways.  FUTURE: a stack of modes (nested dialogs) would push/pop this the way
    the popup layer already stacks; today there is one level, which the console needs.
==============================================================================================*/

/* An exclusive input mode is live -- a GUI_WIN_MODAL window emitted this frame or last (the
   console re-stamps modal.seen_frame at its window_begin every frame it is open, so the fence
   never lapses while it is up and lapses one frame after it closes). */

static bool
modal_scope_live( void )
{
    u32 f = g_ctx->retained.frame;
    return g_ctx->modal.win_id != 0u &&
           ( g_ctx->modal.seen_frame == f || g_ctx->modal.seen_frame + 1u == f );
}

/* True when the live exclusive mode owns `id` -- the focused widget belongs to the modal window.
   The frame-begin focus-clear (interaction_frame_reset) consults this to keep the mode's focus
   sticky: the HOLD rule's first half. */
bool
focus_scope_holds( gui_id_t id )
{
    return id != GUI_ID_NONE && modal_scope_live() &&
           s_interaction.focused_win == g_ctx->modal.win_id;
}

/* The CONFINE rule: may a widget in `win` take focus at all?  While a mode is live only its own
   window's widgets may, exactly as a held active_id denies interaction to every other item during
   a drag.  This is what lets an exclusive window (the dev console) seat focus once, as an event,
   instead of re-stealing it every frame. */
bool
focus_allowed( gui_id_t win )
{
    return !modal_scope_live() || win == g_ctx->modal.win_id;
}

/*==============================================================================================
    Programmatic focus request
==============================================================================================*/

/* Latched until the next focusable widget passes through item_state, which takes keyboard focus
   as if clicked.  Persisting across frames is deliberate: a request queued after this frame's
   field has already emitted lands on that field next frame. */

static bool s_focus_request = false;

/* Consume a pending request -- true exactly once, for the item that gets the focus.  item_state
   calls this only for a focusable item the confine rule already allowed, so a request aimed at a
   modal-fenced window stays latched rather than landing on a background field. */
bool
focus_request_take( void )
{
    if ( !s_focus_request )
        return false;
    s_focus_request = false;
    return true;
}

/*==============================================================================================
    Release + the public door
==============================================================================================*/

/* Drop keyboard/text focus entirely (Enter commit, Escape revert).  The arbitration verb for
   ending a focus capture -- widgets call this instead of writing the interaction record raw;
   the record itself stays owned by core/gui_ctx.c.

   The HOLD rule's second half: while the live exclusive mode owns the focused widget this
   release is a no-op -- the console input keeps its caret after every command instead of going
   dead until the next click.  A focus MOVE (clicking / tabbing to another field) never comes
   through here; it overwrites focused_id directly in item_state, so navigation within the mode
   still works. */
void
focus_release( void )
{
    if ( modal_scope_live() && s_interaction.focused_win == g_ctx->modal.win_id )
        return;
    s_interaction.focused_id = GUI_ID_NONE;
}

/* Public (gui()->set_keyboard_focus): arm the request latch above. */
void
gui_set_keyboard_focus( void )
{
    s_focus_request = true;
}

// clang-format on
/*============================================================================================*/
