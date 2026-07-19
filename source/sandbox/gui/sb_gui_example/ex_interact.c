/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_interact.c -- "Interact" category demos.

    The interaction plumbing: drag-and-drop payloads, the is_item_* introspection family, the
    keyboard focus / caret / edit-hook seams, and the mouse + hardware-cursor readers.
    Included by ex_demos.c (the root unity TU).

==============================================================================================*/

/*==============================================================================================
    Drag & Drop -- typed payloads between color swatches and drop slots.
==============================================================================================*/

static void
ex_interact_dragdrop( void )
{
    static u32  slots[ 4 ]  = { 0xFF303030u, 0xFF303030u, 0xFF303030u, 0xFF303030u };
    static bool peek        = false;
    static bool no_preview  = false;

    if ( ex_begin( "Drag & Drop", 440, 460, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Drag a color source onto a slot below." );
        gui()->checkbox( "GUI_DRAG_ACCEPT_PEEK (slots take the color while hovered)", &peek );
        gui()->checkbox( "GUI_DRAG_NO_PREVIEW (no cursor tooltip / no highlight)",    &no_preview );

        gui_drag_flags_t src_flags = no_preview ? GUI_DRAG_NO_PREVIEW : GUI_DRAG_NONE;
        gui_drag_flags_t acc_flags = ( peek ? GUI_DRAG_ACCEPT_PEEK : GUI_DRAG_NONE )
                                   | ( no_preview ? GUI_DRAG_NO_PREVIEW : GUI_DRAG_NONE );

        /* Sources: three swatches, each publishing its color as an "EX_COLOR" payload. */
        gui()->separator_text( "Sources" );
        static const u32 src_cols[ 3 ] = {
            GUI_COLOR( 0xE6, 0x50, 0x50, 0xFF ),
            GUI_COLOR( 0x50, 0xC8, 0x60, 0xFF ),
            GUI_COLOR( 0x50, 0x90, 0xE6, 0xFF ),
        };
        gui()->row_cols_n( 36, 3 );
        for ( i32 i = 0; i < 3; i++ )
        {
            gui()->push_id_int( i );
            gui_rect_t cell = gui()->empty( 0.0f, 32.0f );
            gui()->draw_frame( cell, src_cols[ i ], 0xFF000000u, 1.0f );
            gui()->invisible_button( "swatch", cell );      /* the draggable item */
            if ( gui()->drag_source_begin( src_flags ) )
            {
                gui()->drag_payload_set( "EX_COLOR", &src_cols[ i ], sizeof( u32 ) );
                gui()->textf( "color #%08X", src_cols[ i ] );   /* cursor preview */
                gui()->drag_source_end();
            }
            gui()->pop_id();
        }
        gui()->row( 0 );

        /* Targets: four slots accepting the payload (on release, or live with PEEK). */
        gui()->separator_text( "Drop slots" );
        gui()->row_cols_n( 44, 4 );
        for ( i32 i = 0; i < 4; i++ )
        {
            gui()->push_id_int( i );
            gui_rect_t cell = gui()->empty( 0.0f, 40.0f );
            gui()->draw_frame( cell, slots[ i ], 0xFF606060u, 1.0f );
            gui()->invisible_button( "slot", cell );
            if ( gui()->drag_target_begin() )
            {
                const gui_drag_payload_t* p = gui()->drag_payload_accept( "EX_COLOR", acc_flags );
                if ( p && p->size == sizeof( u32 ) )
                    memcpy( &slots[ i ], p->data, sizeof( u32 ) );
                gui()->drag_target_end();
            }
            gui()->pop_id();
        }
        gui()->row( 0 );

        /* Global drag state -- readable anywhere, target or not. */
        gui()->separator_text( "drag_active / drag_payload_peek" );
        if ( gui()->drag_active() )
        {
            const gui_drag_payload_t* p = gui()->drag_payload_peek();
            gui()->textf( "drag in flight: type '%s', %u bytes", p ? p->type : "?", p ? p->size : 0 );
        }
        else
        {
            gui()->text_disabled( "no drag in flight" );
        }

        if ( gui()->button( "Reset slots" ) )
            for ( i32 i = 0; i < 4; i++ ) slots[ i ] = 0xFF303030u;
    }
    gui()->window_end();
}

/*==============================================================================================
    Item Queries -- the is_item_* family reported live for a chosen target widget.
==============================================================================================*/

static void
ex_interact_queries( void )
{
    if ( ex_begin( "Item Queries", 420, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Each is_item_* reports on the widget just" );
        gui()->text( "emitted -- interact with the target below." );

        static i32 kind = 0;
        gui()->radio_button( "button",     &kind, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "slider",     &kind, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "input",      &kind, 2 ); gui()->same_line( -1.0f );
        gui()->radio_button( "selectable", &kind, 3 );

        gui()->separator_text( "Target" );
        static f32  q_val       = 5.0f;
        static char q_buf[ 32 ] = "type here";
        static bool q_sel       = false;
        switch ( kind )
        {
            case 0:  gui()->button( "target button" );                       break;
            case 1:  gui()->slider_float( "target", &q_val, 0.0f, 10.0f );   break;
            case 2:  gui()->input_text( "target", q_buf, sizeof( q_buf ) );  break;
            default: gui()->selectable( "target selectable", &q_sel );       break;
        }

        /* Snapshot every query IMMEDIATELY -- the readout rows below are items too. */
        bool       q_hovered   = gui()->is_item_hovered();
        bool       q_active    = gui()->is_item_active();
        bool       q_clicked   = gui()->is_item_clicked();
        bool       q_focused   = gui()->is_item_focused();
        bool       q_activated = gui()->is_item_activated();
        bool       q_deact     = gui()->is_item_deactivated();
        bool       q_deact_ed  = gui()->is_item_deactivated_after_edit();
        bool       q_visible   = gui()->is_item_visible();
        gui_rect_t q_rect      = gui()->get_item_rect();

        /* Outline the reported rect right where the item sits. */
        gui()->draw_round_rect( q_rect, 3.0f, 3.0f, 3.0f, 3.0f, false, 1.0f,
                                GUI_COLOR( 0x4F, 0xC3, 0xF7, 0xFF ) );

        /* Edge events latch for a moment so a one-frame pulse is visible. */
        static f32 t_clicked = -10.0f, t_activated = -10.0f, t_deact = -10.0f, t_deact_ed = -10.0f;
        f32 now = (f32)gui()->get_time();
        if ( q_clicked )   t_clicked   = now;
        if ( q_activated ) t_activated = now;
        if ( q_deact )     t_deact     = now;
        if ( q_deact_ed )  t_deact_ed  = now;

        gui()->separator_text( "Live state" );
        gui()->textf( "hovered   %d    active  %d", q_hovered, q_active );
        gui()->textf( "focused   %d    visible %d", q_focused, q_visible );
        gui()->textf( "rect      (%.0f, %.0f) %.0f x %.0f", q_rect.x, q_rect.y, q_rect.w, q_rect.h );

        gui()->separator_text( "Edges (lit ~1s after firing)" );
        gui()->textf( "clicked                  %s", now - t_clicked   < 1.0f ? "FIRED" : "-" );
        gui()->textf( "activated (press)        %s", now - t_activated < 1.0f ? "FIRED" : "-" );
        gui()->textf( "deactivated (release)    %s", now - t_deact     < 1.0f ? "FIRED" : "-" );
        gui()->textf( "deactivated_after_edit   %s", now - t_deact_ed  < 1.0f ? "FIRED" : "-" );
        gui()->text_disabled( "deactivated_after_edit = the commit-on-release seam" );
    }
    gui()->window_end();
}

/*==============================================================================================
    Keyboard & Focus -- capture fences, key readers, focus + caret requests, the edit hook.
==============================================================================================*/

/* Edit-key hook: consume Up to request a history recall (the Quake-console seam). */
static bool s_kb_recall = false;

static bool
ex_kb_edit_hook( u32 key, bool ctrl, bool shift, bool repeat, void* user )
{
    UNUSED( ctrl ); UNUSED( shift ); UNUSED( user );
    if ( key == APP_KEY_UP && !repeat )
    {
        s_kb_recall = true;
        return true;                    /* consumed: the field never sees the key */
    }
    return false;
}

static void
ex_interact_keyboard( void )
{
    static char field[ 64 ]   = "";
    static char history[ 64 ] = "previous command";

    if ( ex_begin( "Keyboard & Focus", 440, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* The capture fences non-UI code gates its own input reads on. */
        gui()->separator_text( "want_capture_*" );
        gui()->textf( "want_capture_mouse:    %s", gui()->want_capture_mouse()    ? "YES" : "no" );
        gui()->textf( "want_capture_keyboard: %s", gui()->want_capture_keyboard() ? "YES" : "no" );
        gui()->text_disabled( "focus a text field to flip the keyboard fence" );

        /* Key readers: initial press vs OS auto-repeat, down state, release edge. */
        gui()->separator_text( "SPACE key readers" );
        static i32 n_pressed = 0, n_repeat = 0, n_released = 0;
        if ( gui()->is_key_pressed( APP_KEY_SPACE ) )        n_pressed++;
        if ( gui()->is_key_pressed_repeat( APP_KEY_SPACE ) ) n_repeat++;
        if ( gui()->is_key_released( APP_KEY_SPACE ) )       n_released++;
        gui()->textf( "down now: %s", gui()->is_key_down( APP_KEY_SPACE ) ? "YES" : "no" );
        gui()->textf( "pressed %d   with-repeat %d   released %d", n_pressed, n_repeat, n_released );
        gui()->text_disabled( "hold SPACE: only the repeat counter keeps climbing" );

        /* Programmatic focus + the edit-key hook (history on Up). */
        gui()->separator_text( "edit hook: press Up in the field" );
        gui()->set_edit_key_hook( ex_kb_edit_hook, NULL );      /* one-shot: re-arm every frame */
        gui()->input_text( "command", field, sizeof( field ) );
        if ( s_kb_recall )
        {
            snprintf( field, sizeof( field ), "%s", history );
            gui()->set_edit_cursor_end();       /* seat the caret after the programmatic edit */
            s_kb_recall = false;
        }
        if ( gui()->button( "Submit (stores as history)" ) && field[ 0 ] )
        {
            snprintf( history, sizeof( history ), "%s", field );
            field[ 0 ] = '\0';
            gui()->set_keyboard_focus();        /* refocus the field for the next line */
        }
        gui()->textf( "history: %s", history );

        /* Keyboard nav entry points. */
        gui()->separator_text( "keyboard navigation" );
        gui()->text( "Ctrl+Tab cycles windows; arrows walk items;" );
        gui()->text( "typing jumps selectable lists (type-ahead)." );
        if ( gui()->button( "window_set_nav( this window )" ) )
            gui()->window_set_nav( "Keyboard & Focus" );
        gui()->button( "nav stop 1" );
        gui()->button( "nav stop 2" );
        static bool nav_check = false;
        gui()->checkbox( "nav stop 3", &nav_check );
    }
    gui()->window_end();
}

/*==============================================================================================
    Mouse & Cursor -- position / wheel / click readers and the hardware cursor requests.
==============================================================================================*/

static void
ex_interact_mouse( void )
{
    if ( ex_begin( "Mouse & Cursor", 440, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();

        gui()->separator_text( "Position / buttons / wheel" );
        f32 mx, my;
        gui()->get_mouse_pos( &mx, &my );
        gui()->textf( "pos: (%.0f, %.0f)", mx, my );
        gui()->textf( "down: L=%d R=%d M=%d",
                      gui()->is_mouse_down( APP_MOUSE_LEFT ),
                      gui()->is_mouse_down( APP_MOUSE_RIGHT ),
                      gui()->is_mouse_down( APP_MOUSE_MIDDLE ) );

        static f32 wheel_accum = 0.0f;
        wheel_accum += gui()->get_mouse_wheel();
        gui()->textf( "wheel accumulated: %+.1f", wheel_accum );

        static i32 n_click = 0, n_dclick = 0, n_release = 0;
        if ( gui()->is_mouse_clicked( APP_MOUSE_LEFT ) )        n_click++;
        if ( gui()->is_mouse_double_clicked( APP_MOUSE_LEFT ) ) n_dclick++;
        if ( gui()->is_mouse_released( APP_MOUSE_LEFT ) )       n_release++;
        gui()->textf( "clicks %d   double %d   releases %d", n_click, n_dclick, n_release );

        /* Frame clock -- live display churns content every frame, so it is opt-in. */
        gui()->separator_text( "get_time / get_delta_time" );
        static bool live_clock = false;
        gui()->checkbox( "Live clock", &live_clock );
        gui()->same_line( -1.0f );
        gui()->help_marker( "Changing text every frame keeps frame_dirty true, defeating the "
                            "idle-skip -- exactly what volatile widgets exist to avoid.  On is "
                            "fine for testing." );
        if ( live_clock )
            gui()->textf( "t = %.2f s   dt = %.1f ms", gui()->get_time(), gui()->get_delta_time() * 1000.0f );
        else
            gui()->text_disabled( "t = (off)" );

        /* Hardware cursor: request a shape while hovering the strip below. */
        gui()->separator_text( "set_mouse_cursor" );
        static const char* cur_names[] = {
            "ARROW", "TEXT", "RESIZE_ALL", "RESIZE_NS", "RESIZE_EW",
            "RESIZE_NESW", "RESIZE_NWSE", "HAND", "NOT_ALLOWED",
        };
        static i32 cur_sel = 7;     /* HAND */
        gui()->combo( "shape", &cur_sel, cur_names, 9 );

        gui_rect_t strip = gui()->empty( 0.0f, 40.0f );
        bool hot = gui()->is_mouse_hovering_rect( strip );
        gui()->draw_frame( strip, hot ? 0xFF35485Eu : 0xFF262C33u, 0xFF505050u, 1.0f );
        gui()->draw_text_in( strip, GUI_ALIGN_CENTER, 0xFFE0E0E0u,
                             "hover here for the selected cursor" );
        if ( hot )
            gui()->cursor_set( (app_cursor_t)cur_sel );
        gui()->textf( "current request: %s", cur_names[ gui()->get_mouse_cursor() < 9 ? gui()->get_mouse_cursor() : 0 ] );

        gui()->text_disabled( "The widgets already drive resize / I-beam shapes" );
        gui()->text_disabled( "themselves; this is for custom clickables." );
    }
    gui()->window_end();
}

/*============================================================================================*/
