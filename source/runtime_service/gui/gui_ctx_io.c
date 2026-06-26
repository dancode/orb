/*==============================================================================================

    runtime_service/gui/gui_ctx_io.c -- Public IO accessor API.

    The frame-coherent input snapshot the widgets see, exposed for UI / tool code that wants
    to read keys, the mouse, or the clock without re-querying app() -- which bypasses gui's
    frame timing and, more importantly, its input capture.

    want_capture_* are the fence: gate any direct app() input read in non-UI code on them, so
    gameplay never acts on a keystroke gui consumed (typing in a field) or a click that was
    really a widget / window drag.

    Included by gui.c after gui_ctx.c (which defines s_interaction, s_build, s_nav,
    s_popup_open_count, rect_hit) and gui_input.c (which defines s_io).

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

/* True when gui owns the keyboard this frame -- a text field is focused, keyboard nav is engaged,
   or a popup/menu is open -- so gameplay / tools must not also act on the same keystrokes (an arrow
   driving the nav cursor, an Enter activating the nav item).  The fence for non-UI key reads. */
bool
gui_want_capture_keyboard( void )
{
    return s_interaction.focused_id != GUI_ID_NONE || s_nav.highlight
        || s_popup_open_count > 0 || s_nav.bar_win != GUI_ID_NONE;
}

/* True when the cursor is over rect r and r is actually interactable: it lies in the front-most
   window (occlusion), inside the active region clip (scrolled-away content does not hover), and no
   other widget owns a drag in flight.  The IsMouseHoveringRect analogue -- gates a hover tint or a
   manual is_mouse_clicked test on custom-drawn geometry exactly as the widgets gate their own. */
bool
gui_is_mouse_hovering_rect( gui_rect_t r )
{
    bool can_hover = ( s_interaction.active_id == GUI_ID_NONE );
    bool win_hover = ( s_build.win_id == s_interaction.hover_win );
    return can_hover && win_hover && rect_hit( s_build.clip_rect ) && rect_hit( r );
}

/*----------------------------------------------------------------------------------------------
    Last-item introspection (the Dear ImGui IsItem* family).

    Every reader reports on "the widget just emitted" -- the item whose rect and interaction state
    widget_behavior latched into s_build.last_item_* (gui_widget_core.c).  Call immediately after a
    widget, the way set_item_tooltip / popup_context_item_begin already bind to the previous item:

        gui()->button( "Save" );
        if ( gui()->is_item_hovered() ) gui()->set_item_tooltip( "Write to disk" );
        if ( gui()->is_item_clicked() ) save();

    The activated / deactivated edges compare this frame's active id against the previous-frame
    baseline (active_id_prev, snapshot at new_frame): activated fires the frame an item first grabs
    active, deactivated the frame it lets go -- the natural seam for "commit on release" handling.
----------------------------------------------------------------------------------------------*/

bool gui_is_item_hovered ( void ) { return s_build.last_item_status.hover;   }
bool gui_is_item_active  ( void ) { return s_build.last_item_status.active;  }
bool gui_is_item_clicked ( void ) { return s_build.last_item_status.clicked; }
bool gui_is_item_focused ( void ) { return s_build.last_item_status.focused; }

/* True the frame the last item first became active (press / nav-activate), false while it stays held. */
bool
gui_is_item_activated( void )
{
    gui_id_t id = s_build.last_item_id;
    return id != GUI_ID_NONE && s_interaction.active_id == id && s_interaction.active_id_prev != id;
}

/* True the frame the last item stops being active (release / focus loss) -- the commit edge. */
bool
gui_is_item_deactivated( void )
{
    gui_id_t id = s_build.last_item_id;
    return id != GUI_ID_NONE && s_interaction.active_id != id && s_interaction.active_id_prev == id;
}

/* True when the last item's rect has any visible (unclipped) area in the active region clip. */
bool
gui_is_item_visible( void )
{
    gui_rect_t v = rect_intersect( s_build.last_item_rect, s_build.clip_rect );
    return v.w > 0.0f && v.h > 0.0f;
}

/* The last item's screen rect (the GetItemRectMin/Max/Size trio in one rect-centric accessor):
   anchor custom draw to a widget just emitted, or measure it. */
gui_rect_t gui_get_item_rect( void ) { return s_build.last_item_rect; }

/* Per-key state from the frame snapshot.  An out-of-range key reads as up; the public app_key_t
   range is bounded by APP_KEY_COUNT <= GUI_KEY_COUNT (asserted in gui_input.c).  is_key_pressed
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
