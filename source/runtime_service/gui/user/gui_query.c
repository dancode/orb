/*==============================================================================================

    runtime_service/gui/user/gui_query.c -- Public readers over the io + interaction services.

    The frame-coherent input snapshot the widgets see, exposed for UI / tool code that wants
    to read keys, the mouse, or the clock without re-querying app() -- which bypasses gui's
    frame timing and, more importantly, its input capture.  Pure caller vocabulary: the
    machinery (foundation/gui_io.c snapshot, interact/gui_item.c last-item latch) stays with the
    services; the readers a user speaks live here.

    want_capture_* are the fence: gate any direct app() input read in non-UI code on them, so
    gameplay never acts on a keystroke gui consumed (typing in a field) or a click that was
    really a widget / window drag.

    Included by gui.c in the user/ tier (last of the tiers); reads s_interaction, s_build,
    g_ctx->nav, g_ctx->popup.open_count, rect_hit (foundation/gui_ctx.c) and s_io (foundation/gui_io.c), all in
    scope far above.  Internal readers (the frame overlay's hotkeys, the dashboard's hover
    check) are deliberate dogfooding through the gui_host.h declarations.

==============================================================================================*/
// clang-format off

/* True when the cursor is over an gui window, or a widget owns the mouse (a drag in flight) --
   the signal that a click belongs to the UI, not the scene behind it.  hover_win lags the cursor
   by one frame (the deferred occlusion resolve), matching how the widgets gate their own hover. */
bool
gui_want_capture_mouse( void )
{
    return s_interaction.hover_win != GUI_ID_NONE || s_interaction.active_id != GUI_ID_NONE;
}

/*----------------------------------------------------------------------------------------------
    Keyboard routing model

    One key press is resolved by exactly one of four tiers, evaluated in this order every frame.
    There is no explicit routing table (contrast Dear ImGui's SetKeyOwner/Shortcut routing, or
    Godot's _input -> _gui_input -> _unhandled_input chain) -- ORB gets the same priority order
    for free from frame call order, because each tier's code runs strictly after the ones above it
    within a frame.  The two primitives that make this hold:

      - HARD BLOCK  (this function): true means every key this frame belongs to gui, full stop --
        used only for states where handling must not depend on which key it is (typing captures
        everything so nothing types past a text field; a modal must block everything under it).
      - key_claim(k) (foundation/gui_io.c): zeroes one key's press/repeat edge the instant a
        consumer actually uses it, so nothing later in the frame also reacts to that same press.
        Unclaimed keys simply survive to the next tier -- there is nothing to opt into.

    +-------------------------------------------------------------------------------------------+
    | TIER 0  ACTIVE CAPTURE   s_interaction.focused_id != NONE   (a text field is mid-edit)     |
    |         HARD BLOCK -- typing must not also drive nav or an app hotkey.                     |
    +-------------------------------------------------------------------------------------------+
    | TIER 1  MODAL / MENU     popup.open_count > 0   or   nav.bar_win != NONE                   |
    |         HARD BLOCK -- modal correctness: nothing underneath may react, key or no key.      |
    +-------------------------------------------------------------------------------------------+
    | TIER 2  FOCUSED WINDOW   nav.focused_win's widgets: move, type-ahead, mnemonics, activate  |
    |         key_claim() -- only the keys actually used vanish.  Most are read AND claimed in   |
    |         nav_new_frame (frame_begin, after input sampling); Enter/Space at the activation   |
    |         seam during widget emission.  Both after tiers 0-1, before tier 3 (frame_end).     |
    +-------------------------------------------------------------------------------------------+
    | TIER 3  APP / GLOBAL     debug_hotkeys() (gui_frame_end), future gameplay bindings          |
    |         Runs last, after all emission -- sees only what tiers 0-2 left unclaimed.           |
    +-------------------------------------------------------------------------------------------+

    Tier 3 code must read keys through gui()->is_key_pressed()/is_key_down() (this file, below) --
    NOT app()->key_pressed() directly.  app()'s state is the raw OS snapshot; key_claim only ever
    zeroes gui's own copy (s_io), so reading app() directly bypasses the whole model.

    Deliberately NOT part of the hard block: nav.highlight.  It means "the keyboard was the last
    input instrument used" (e.g. arrow-navigating onto a checkbox), not that any particular key is
    spoken for -- that distinction is exactly what tier 2's per-key claim exists to make precise.

    Multi-context: s_io and the interaction record are shared by every gui context, but only a
    LISTENING context (gui_ctx_set_listening) runs this model in a given frame -- nav_new_frame and
    the widgets both early-out for a deaf context, so a passive context never reads or claims the
    shared keys, and the keyboard belongs to whichever context the app left listening. */

/* True when gui owns EVERY key this frame (tiers 0-1 above) -- the fence for non-UI key reads.
   Individual nav key paths below tier 1 consume only the keys they use (key_claim), so they gate
   through their own call-order position instead of this function. */
bool
gui_want_capture_keyboard( void )
{
    return s_interaction.focused_id != GUI_ID_NONE
        || g_ctx->popup.open_count > 0 || g_ctx->nav.bar_win != GUI_ID_NONE;
}

/* True when the cursor is over rect r and r is actually interactable: it lies in the front-most
   window (occlusion), inside the active region clip (scrolled-away content does not hover), and no
   other widget owns a drag in flight.  The IsMouseHoveringRect analogue -- gates a hover tint or a
   manual is_mouse_clicked test on custom-drawn geometry exactly as the widgets gate their own. */
bool
gui_is_mouse_hovering_rect( gui_rect_t r )
{
    bool can_hover = interact_idle();
    bool win_hover = ( s_scope.win == s_interaction.hover_win );
    return can_hover && win_hover && rect_hit( s_scope.clip ) && rect_hit( r );
}

/*----------------------------------------------------------------------------------------------
    Last-item introspection (the Dear ImGui IsItem* family).

    Every reader reports on "the widget just emitted" -- the item whose rect and interaction state
    item_state latched into s_scope.last_* (interact/gui_item.c).  Call immediately after a
    widget, the way set_item_tooltip / popup_context_item_begin already bind to the previous item:

        gui()->button( "Save" );
        if ( gui()->is_item_hovered() ) gui()->set_item_tooltip( "Write to disk" );
        if ( gui()->is_item_clicked() ) save();

    The activated / deactivated edges compare this frame's active id against the previous-frame
    baseline (active_id_prev, snapshot at new_frame): activated fires the frame an item first grabs
    active, deactivated the frame it lets go -- the natural seam for "commit on release" handling.
----------------------------------------------------------------------------------------------*/

bool gui_is_item_hovered ( void ) { return s_scope.last_status.hover;   }
bool gui_is_item_active  ( void ) { return s_scope.last_status.active;  }
bool gui_is_item_clicked ( void ) { return s_scope.last_status.clicked; }
bool gui_is_item_focused ( void ) { return s_scope.last_status.focused; }

/* True the frame the last item first became active (press / nav-activate), false while it stays held. */
bool
gui_is_item_activated( void )
{
    gui_id_t id = s_scope.last_id;
    return id != GUI_ID_NONE && s_interaction.active_id == id && s_interaction.active_id_prev != id;
}

/* True the frame the last item stops being active (release / focus loss) -- the commit edge. */
bool
gui_is_item_deactivated( void )
{
    gui_id_t id = s_scope.last_id;
    return id != GUI_ID_NONE && s_interaction.active_id != id && s_interaction.active_id_prev == id;
}

/* True the frame after the last item lost keyboard focus, only when the buffer was modified
   during that focus session.  Use this after input_text to detect blur-with-edit uniformly with
   input_int / input_float (which commit and return true on blur directly).
   Fires on both Enter and click-away as long as any edit occurred while the field was focused. */
bool
gui_is_item_deactivated_after_edit( void )
{
    gui_id_t id = s_scope.last_id;
    return id != GUI_ID_NONE && id == s_interaction.focus_ended_id && s_interaction.focus_ended_edited;
}

/* True when the last item's rect has any visible (unclipped) area in the active region clip. */
bool
gui_is_item_visible( void )
{
    gui_rect_t v = rect_intersect( s_scope.last_rect, s_scope.clip );
    return v.w > 0.0f && v.h > 0.0f;
}

/* The last item's screen rect (the GetItemRectMin/Max/Size trio in one rect-centric accessor):
   anchor custom draw to a widget just emitted, or measure it. */
gui_rect_t gui_get_item_rect( void ) { return s_scope.last_rect; }

/* Per-key state from the frame snapshot.  An out-of-range key reads as up; the public app_key_t
   range is bounded by APP_KEY_COUNT <= GUI_KEY_COUNT (asserted in foundation/gui_io.c).  is_key_pressed
   is the initial press this frame; is_key_pressed_repeat also fires on each OS auto-repeat tick (the
   Dear ImGui repeat=true case) -- the user's system rate drives it. */
static bool key_in_range( app_key_t key ) { return (i32)key >= 0 && (i32)key < APP_KEY_COUNT; }

bool gui_is_key_down          ( app_key_t key ) { return key_in_range( key ) && s_io.keys_down          [ key ]; }
bool gui_is_key_pressed       ( app_key_t key ) { return key_in_range( key ) && s_io.keys_pressed       [ key ]; }
bool gui_is_key_pressed_repeat( app_key_t key ) { return key_in_range( key ) && s_io.keys_pressed_repeat[ key ]; }
bool gui_is_key_released      ( app_key_t key ) { return key_in_range( key ) && s_io.keys_released      [ key ]; }

/* Per-button mouse state; app_mouse_button_t (0=left,1=right,2=middle) indexes the snapshot
   directly.  is_mouse_clicked is the press-down edge, mirroring ImGui::IsMouseClicked. */
static bool mb_in_range( app_mouse_button_t b ) { return (i32)b >= 0 && (i32)b < 3; }

bool gui_is_mouse_down          ( app_mouse_button_t b ) { return mb_in_range( b ) && s_io.mouse_down    [ b ]; }
bool gui_is_mouse_clicked       ( app_mouse_button_t b ) { return mb_in_range( b ) && s_io.mouse_pressed [ b ]; }
bool gui_is_mouse_released      ( app_mouse_button_t b ) { return mb_in_range( b ) && s_io.mouse_released[ b ]; }
bool gui_is_mouse_double_clicked( app_mouse_button_t b ) { return mb_in_range( b ) && s_io.mouse_double  [ b ]; }

/* Request a hardware cursor shape for this frame.  The widgets already set the resize / text shapes
   from their own hover; call this from UI code for a shape gui cannot infer -- e.g. a HAND over a
   custom clickable.  The last request of the frame wins and is flushed to the OS window the cursor
   is over (only while gui owns the mouse).  Reset to APP_CURSOR_ARROW at the top of every frame. */
void         gui_set_mouse_cursor( app_cursor_t c ) { set_mouse_cursor( c ); }
app_cursor_t gui_get_mouse_cursor( void )           { return s_interaction.mouse_cursor; }

/* Pointer position, wheel delta, and timing straight from the snapshot. */
void gui_get_mouse_pos  ( f32* x, f32* y ) { if ( x ) *x = s_io.mouse_x; if ( y ) *y = s_io.mouse_y; }
f32  gui_get_mouse_wheel( void )           { return s_io.mouse_wheel; }
f32  gui_get_delta_time ( void )           { return s_io.dt; }
f64  gui_get_time       ( void )           { return s_io.time; }

// clang-format on
/*============================================================================================*/
