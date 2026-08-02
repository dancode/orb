/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_widgets.c -- "Widgets" category demos.

    The stock interactive controls, each wrapped in a small interactive test suite: sibling
    widgets drive the demonstrated widget's own parameters (ranges, steps, speeds, flags) so the
    parametric surface is visible live.  Included by ex_demos.c (the root unity TU).

==============================================================================================*/

/*==============================================================================================
    Basic Widgets -- button family, checkbox / radio, progress, repeat, disabled scope.
==============================================================================================*/

static void
ex_widgets_basic( void )
{
    if ( ex_begin( "Basic Widgets", 640, 640, GUI_WIN_HSCROLL ) )
    {
        gui()->stack();
        gui()->text( "Every widget returns true on the frame it fires." );
        gui()->separator();

        /* button / small_button -- press counters show the fire frame. */
        static i32 clicks = 0;
        if ( gui()->button( "Click me" ) )
            clicks++;
        gui()->same_line( -1.0f );
        gui()->textf( "clicked %d time(s)", clicks );
        
        gui()->text( "small_button packs onto a text line:" );
        gui()->same_line( -1.0f );
        static i32 small_clicks = 0;
        if ( gui()->small_button( "tap" ) )
            small_clicks++;
        gui()->same_line( -1.0f );
        gui()->textf( "%d", small_clicks );
        
        /* arrow_button spinners -- BUTTON_REPEAT makes a held button fire repeatedly. */
        gui()->separator_text( "arrow_button + GUI_ITEM_BUTTON_REPEAT" );
        static bool repeat = true;
        static i32  spin   = 0;
        gui()->checkbox( "Hold to auto-repeat", &repeat );
        gui()->push_item_flag( GUI_ITEM_BUTTON_REPEAT, repeat );
        if ( gui()->arrow_button( "##spin_dn", GUI_DIR_LEFT ) )  spin--;
        gui()->same_line( -1.0f );
        if ( gui()->arrow_button( "##spin_up", GUI_DIR_RIGHT ) ) spin++;
        gui()->pop_item_flag();
        gui()->same_line( 12.0f );
        gui()->textf( "value: %d", spin );
        
        /* checkbox / radio_button -- the two toggle shapes. */
        gui()->separator_text( "checkbox / radio_button" );
        static bool checked = true;
        gui()->checkbox( "Enable feature", &checked );
        
        static i32 mode = 0;
        gui()->radio_button( "Off",    &mode, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Low",    &mode, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "High",   &mode, 2 );
        gui()->textf( "mode = %d", mode );
        
        /* progress_bar -- fraction + overlay caption, with an interactive driver. */
        gui()->separator_text( "progress_bar" );
        static bool p_anim    = false;
        static f32  p_speed   = 0.4f;
        static f32  p_frac    = 0.66f;
        static i32  p_overlay = 0;      /* 0 percent, 1 custom, 2 none */
        gui()->checkbox( "Animate", &p_anim );
        gui()->same_line( -1.0f );
        gui()->help_marker( "Animation keeps content changing, so idle-skip stays awake while on." );
        if ( p_anim )
        {
            p_frac = 0.5f + 0.5f * sinf( (f32)gui()->get_time() * p_speed * 2.0f * GUI_PI );
            gui()->slider_float( "Speed (Hz)", &p_speed, 0.05f, 2.0f );
        }
        else
        {
            gui()->slider_float( "Fraction", &p_frac, 0.0f, 1.0f );
        }
        gui()->radio_button( "\"NN%\" caption", &p_overlay, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Custom",          &p_overlay, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "No text",         &p_overlay, 2 );
        
        char p_buf[ 32 ];
        snprintf( p_buf, sizeof( p_buf ), "%.0f / 100 units", p_frac * 100.0f );
        gui()->progress_bar( p_frac, p_overlay == 0 ? NULL : ( p_overlay == 1 ? p_buf : "" ) );
        
        /* label_text -- read-only value rows that align like the editable widgets. */
        gui()->separator_text( "label_text" );
        gui()->label_text( "Backend", "vulkan" );
        gui()->label_text( "Status",  checked ? "enabled" : "disabled" );
        
        /* disabled scope -- inert + dimmed, honored by every widget. */
        gui()->separator_text( "disabled_begin / disabled_end" );
        static bool section_off = true;
        gui()->checkbox( "Disable the block below", &section_off );
        gui()->disabled_begin( section_off );
        gui()->button( "Inert button" );
        static bool d_check = true;
        gui()->checkbox( "Inert checkbox", &d_check );
        static f32 d_val = 4.0f;
        gui()->slider_float( "Inert slider", &d_val, 0.0f, 10.0f );
        gui()->disabled_end();
    }
    gui()->window_end();
}

/*==============================================================================================
    Text & Trees -- read-only text runs, wrapping, folding headers, tree nodes, indents.
==============================================================================================*/

/* Recursive tree body -- depth/breadth driven by the sliders in ex_widgets_text. */
static void
ex_tree_rec( i32 depth, i32 max_depth, i32 breadth )
{
    for ( i32 i = 0; i < breadth; i++ )
    {
        gui()->push_id_int( i );
        char label[ 32 ];
        snprintf( label, sizeof( label ), "Node %d.%d", depth, i );
        if ( depth >= max_depth )
        {
            gui()->bullet_text( label );
        }
        else if ( gui()->tree_node( label ) )
        {
            ex_tree_rec( depth + 1, max_depth, breadth );
            gui()->tree_pop();
        }
        gui()->pop_id();
    }
}

static void
ex_widgets_text( void )
{
    if ( ex_begin( "Text & Trees", 420, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Plain text line." );
        gui()->textf( "Formatted: pi ~= %.4f, frame %d", 3.14159f, 42 );
        gui()->text_colored( GUI_COLOR( 0xFF, 0xB0, 0x40, 0xFF ), "text_colored in amber." );
        gui()->text_disabled( "text_disabled -- the dim secondary color." );
        gui()->text_wrapped( "text_wrapped word-wraps a paragraph to the region content width, "
                             "re-flowing as the window is resized -- drag the window edge to see "
                             "these lines break at different points." );

        gui()->separator_text( "Bullets" );
        gui()->bullet_text( "bullet_text -- glyph + label" );
        gui()->bullet();
        gui()->same_line( -1.0f );
        gui()->text( "bullet() alone, then same_line text" );

        gui()->separator();

        /* collapsing_header returns its open state; guard the body with it. */
        if ( gui()->collapsing_header( "Details (click to fold)" ) )
        {
            gui()->text( "These lines only draw while the" );
            gui()->text( "header above is expanded." );
        }
        if ( gui()->collapsing_header( "More details" ) )
        {
            gui()->bullet_text( "another folded section" );
            gui()->bullet_text( "independent open state" );
        }

        /* tree_node -- the frameless fold; sliders drive the recursive shape. */
        gui()->separator_text( "tree_node (parametric)" );
        static i32 depth   = 2;
        static i32 breadth = 3;
        gui()->slider_int( "Depth",    &depth,   1, 4 );
        gui()->slider_int( "Children", &breadth, 1, 4 );
        ex_tree_rec( 1, depth, breadth );

        /* indent / unindent -- the raw mechanism behind tree nesting. */
        gui()->separator_text( "indent / unindent" );
        static i32 ind = 24;
        gui()->slider_int( "Indent (px)", &ind, 4, 80 );
        gui()->text( "at the margin" );
        gui()->indent( (f32)ind );
        gui()->text( "indented" );
        gui()->indent( (f32)ind );
        gui()->text( "indented twice" );
        gui()->unindent( (f32)ind );
        gui()->unindent( (f32)ind );
        gui()->text( "back at the margin" );
    }
    gui()->window_end();
}

/*==============================================================================================
    Text Inputs -- input_text variants, the change callback, programmatic focus + caret.
==============================================================================================*/

/* input_text_ex change callback: count edits and remember the live length. */
static i32 s_edit_count = 0;
static u32 s_edit_len   = 0;

static void
ex_input_on_change( char* buf, u32 len, u32 bufsz, void* user )
{
    UNUSED( buf ); UNUSED( bufsz ); UNUSED( user );
    s_edit_count++;
    s_edit_len = len;
}

static void
ex_widgets_input_text( void )
{
    if ( ex_begin( "Text Inputs", 420, 420, GUI_WIN_NONE ) )
    {
        gui()->stack();

        static char name[ 32 ] = "orb";
        gui()->input_text( "Name", name, sizeof( name ) );
        gui()->textf( "hello, %s", name );

        gui()->separator_text( "input_text_with_hint" );
        static char search[ 48 ] = "";
        gui()->input_text_with_hint( "Search", "type to filter...", search, sizeof( search ) );

        gui()->separator_text( "input_text_ex (change callback)" );
        static char watched[ 48 ] = "edit me";
        gui()->input_text_ex( "Watched", watched, sizeof( watched ), ex_input_on_change, NULL );
        gui()->textf( "edits: %d   live length: %u", s_edit_count, s_edit_len );

        gui()->separator_text( "Programmatic focus + caret" );
        static char target[ 48 ] = "focus lands here";
        if ( gui()->button( "Focus the field" ) )
            gui()->set_keyboard_focus();            /* next focusable widget takes focus */
        gui()->same_line( -1.0f );
        if ( gui()->button( "Append + caret to end" ) )
        {
            size_t n = strlen( target );
            if ( n + 1 < sizeof( target ) )
            {
                target[ n ]     = '!';
                target[ n + 1 ] = '\0';
            }
            gui()->set_edit_cursor_end();           /* seat the caret after the programmatic edit */
        }
        gui()->input_text( "Target", target, sizeof( target ) );
    }
    gui()->window_end();
}

/*==============================================================================================
    Numeric Inputs -- parse-on-enter fields with step buttons, and the vector rows.
==============================================================================================*/

static void
ex_widgets_numeric( void )
{
    if ( ex_begin( "Numeric Inputs", 440, 460, GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Fields parse on Enter or focus loss; step != 0" );
        gui()->text( "shows [-][+] buttons (Ctrl steps by step_fast)." );

        /* The step sizes themselves are live parameters. */
        gui()->separator_text( "Step parameters" );
        static i32 step      = 1;
        static i32 step_fast = 10;
        gui()->slider_int( "step",      &step,      0, 10  );
        gui()->slider_int( "step_fast", &step_fast, 0, 100 );

        gui()->separator_text( "input_int / input_float / input_double" );
        static i32 iv = 42;
        gui()->input_int( "int", &iv, step, step_fast );

        static i32 fmt_sel = 0;
        gui()->radio_button( "%.3f", &fmt_sel, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "%.1f", &fmt_sel, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "%e",   &fmt_sel, 2 );
        static const char* fmts[] = { "%.3f", "%.1f", "%e" };

        static f32 fv = 3.14159f;
        gui()->input_float( "float", &fv, (f32)step, (f32)step_fast, fmts[ fmt_sel ] );

        static f64 dv = 2.718281828459;
        gui()->input_double( "double", &dv, (f64)step, (f64)step_fast, "%.9f" );

        gui()->separator_text( "Vector rows (input_float2/3/4)" );
        static f32 v2[ 2 ] = { 1.0f, 2.0f };
        static f32 v3[ 3 ] = { 0.0f, 1.0f, 0.0f };
        static f32 v4[ 4 ] = { 0.1f, 0.2f, 0.3f, 1.0f };
        gui()->input_float2( "vec2", v2, NULL );
        gui()->input_float3( "vec3", v3, NULL );
        gui()->input_float4( "vec4", v4, "%.2f" );
        gui()->textf( "vec3 = (%.2f, %.2f, %.2f)", v3[ 0 ], v3[ 1 ], v3[ 2 ] );
    }
    gui()->window_end();
}

/*==============================================================================================
    Sliders & Drags -- tracked sliders vs unbounded drag fields, all knobs parametric.
==============================================================================================*/

static void
ex_widgets_sliders( void )
{
    if ( ex_begin( "Sliders & Drags", 440, 560, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* Shared parameters driving the sample widgets below. */
        gui()->separator_text( "Suite parameters" );
        static f32  lo         = 0.0f;
        static f32  hi         = 10.0f;
        static bool hide_value = false;
        static bool circle     = false;
        gui()->drag_float( "min", &lo, 0.05f, -100.0f, 100.0f, "%.1f" );
        gui()->drag_float( "max", &hi, 0.05f, -100.0f, 100.0f, "%.1f" );
        if ( hi < lo ) hi = lo;
        gui()->checkbox( "GUI_ITEM_NO_VALUE_TEXT (bare tracks)", &hide_value );
        gui()->checkbox( "Circle knob (GUI_VAR_KNOB_SHAPE)",    &circle );

        gui()->push_item_flag( GUI_ITEM_NO_VALUE_TEXT, hide_value );
        gui()->push_style_var( GUI_VAR_KNOB_SHAPE, circle ? 1.0f : 0.0f );

        gui()->separator_text( "slider_float / _step / slider_int" );
        static f32 sf = 5.0f;
        gui()->slider_float( "float", &sf, lo, hi );

        static f32 step = 0.25f;
        static f32 sq   = 2.0f;
        gui()->slider_float_step( "quantized", &sq, lo, hi, step );
        gui()->slider_float( "step size", &step, 0.0f, 2.0f );

        static i32 si = 3;
        gui()->slider_int( "int", &si, (i32)lo, (i32)hi );

        gui()->separator_text( "drag_int / drag_float (no track)" );
        static f32  speed   = 0.05f;
        static bool bounded = true;
        gui()->slider_float( "v_speed (units/px)", &speed, 0.005f, 1.0f );
        gui()->checkbox( "Bounded to [min, max]", &bounded );

        f32 dlo = bounded ? lo : 0.0f;
        f32 dhi = bounded ? hi : 0.0f;      /* min == max = unbounded */

        /* units/px must scale WITH the span, not just be proportional to it (the earlier attempt's
           mistake): pixels-to-cross-the-whole-range is (span / v_speed), so a flat or span-scaled-up
           multiplier still shrinks that pixel distance as span shrinks -- exactly the "un" repro,
           where the default 0..10 span was already only ~10px wide.  Dividing by span instead makes
           pixels-to-cross equal (10.0f / speed) regardless of span: the drag always takes the same
           throw to sweep min..max, whether that range is 0..3 or 0..300.  10.0f is both the
           reference span (the default lo/hi here) and the fallback for unbounded/inverted dlo/dhi,
           so the demo's out-of-the-box feel at those defaults is unchanged. */
        f32 int_span = ( dhi > dlo ) ? ( dhi - dlo ) : 10.0f;
        static i32 di = 50;
        gui()->drag_int( "drag int", &di, speed * int_span / 10.0f, (i32)dlo, (i32)dhi, "%d units" );
        static f32 df = 1.0f;
        gui()->drag_float( "drag float", &df, speed, dlo, dhi, NULL );

        gui()->separator_text( "Vector drags (drag_float2/3/4)" );
        static f32 d2[ 2 ] = { 0.5f, 1.5f };
        static f32 d3[ 3 ] = { 1.0f, 2.0f, 3.0f };
        static f32 d4[ 4 ] = { 0.0f, 0.25f, 0.5f, 1.0f };
        gui()->drag_float2( "vec2", d2, speed, dlo, dhi, "%.2f" );
        gui()->drag_float3( "vec3", d3, speed, dlo, dhi, "%.2f" );
        gui()->drag_float4( "vec4", d4, speed, dlo, dhi, "%.2f" );

        gui()->pop_style_var( 1 );
        gui()->pop_item_flag();
    }
    gui()->window_end();
}

/*==============================================================================================
    Color Editors -- color_edit3/4 with every display flag toggled live, plus a swatch strip.
==============================================================================================*/

static void
ex_widgets_color( void )
{
    if ( ex_begin( "Color Editors", 420, 400, GUI_WIN_NONE ) )
    {
        gui()->stack();

        static u32 flags = 0;   /* gui_color_edit_flags_t bits */
        gui()->text( "Display flags (applied to both editors):" );
        ex_flag_checkbox( "NO_ALPHA (hide/ignore alpha)", &flags, GUI_COLOR_EDIT_NO_ALPHA );
        ex_flag_checkbox( "DISPLAY_HSV",                  &flags, GUI_COLOR_EDIT_DISPLAY_HSV );
        ex_flag_checkbox( "FLOAT (0..1 not 0..255)",      &flags, GUI_COLOR_EDIT_FLOAT );

        gui()->separator_text( "color_edit3 / color_edit4" );
        static f32 c3[ 3 ] = { 0.4f, 0.7f, 0.1f };
        static f32 c4[ 4 ] = { 0.2f, 0.5f, 0.9f, 0.8f };
        gui()->color_edit3( "rgb",  c3, (gui_color_edit_flags_t)flags );
        gui()->color_edit4( "rgba", c4, (gui_color_edit_flags_t)flags );

        /* Swatch strip: the edited colors drawn raw, plus a blend gradient between them. */
        gui()->separator_text( "Live swatches" );
        u32 a = GUI_COLOR( (u8)( c3[ 0 ] * 255.0f ), (u8)( c3[ 1 ] * 255.0f ),
                           (u8)( c3[ 2 ] * 255.0f ), 0xFF );
        u32 b = GUI_COLOR( (u8)( c4[ 0 ] * 255.0f ), (u8)( c4[ 1 ] * 255.0f ),
                           (u8)( c4[ 2 ] * 255.0f ), (u8)( c4[ 3 ] * 255.0f ) );
        gui_rect_t r = gui()->canvas( 48.0f );
        gui_rect_t left  = gui_rect_cut_left ( &r, r.w * 0.25f );
        gui_rect_t right = gui_rect_cut_right( &r, r.w * 0.25f );
        gui()->draw_rect( left.x,  left.y,  left.w,  left.h,  a );
        gui()->draw_checker( right, 6.0f, 0xFF808080u, 0xFF404040u );   /* alpha shows through */
        gui()->draw_rect( right.x, right.y, right.w, right.h, b );
        gui()->draw_gradient( r, a, b, true );
        gui()->textf( "rgb #%02X%02X%02X   rgba %.2f/%.2f/%.2f/%.2f",
                      (u8)( c3[ 0 ] * 255.0f ), (u8)( c3[ 1 ] * 255.0f ), (u8)( c3[ 2 ] * 255.0f ),
                      c4[ 0 ], c4[ 1 ], c4[ 2 ], c4[ 3 ] );
    }
    gui()->window_end();
}

/*==============================================================================================
    Selection & Lists -- selectable rows, combo box, list box, one-liners and begin/end forms.
==============================================================================================*/

static void
ex_widgets_selection( void )
{
    if ( ex_begin( "Selection & Lists", 420, 640, GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* selectable -- single vs multi selection is the caller's policy. */
        gui()->separator_text( "selectable (single / multi)" );
        static bool multi = false;
        static bool no_typeahead = false;
        gui()->checkbox( "Multi-select", &multi );
        gui()->checkbox( "GUI_ITEM_NO_TYPEAHEAD (typing won't jump rows)", &no_typeahead );

        static i32  sel_one       = -1;
        static bool sel_many[ 8 ] = { false };
        if ( gui()->child_begin( "sel_rows", 0, gui()->sz_rows_h( 8 ), GUI_WIN_NONE ) )
        {
            gui()->stack();
            gui()->push_item_flag( GUI_ITEM_NO_TYPEAHEAD, no_typeahead );
            for ( i32 i = 0; i < 8; i++ )
            {
                gui()->push_id_int( i );
                char row[ 32 ];
                snprintf( row, sizeof( row ), "%s item %02d", multi ? "multi" : "single", i );
                if ( multi )
                {
                    gui()->selectable( row, &sel_many[ i ] );
                }
                else
                {
                    bool on = ( sel_one == i );
                    if ( gui()->selectable( row, &on ) )
                        sel_one = on ? i : -1;
                }
                gui()->pop_id();
            }
            gui()->pop_item_flag();
        }
        gui()->child_end();

        /* msel -- the multi-select protocol: plain click replaces, Ctrl toggles, Shift ranges
           from the anchor, Ctrl+Shift adds, Shift+arrow extends, Ctrl+A selects all.  Storage
           is THIS demo's bool array; the scope resolves each frame to one range action and
           msel_apply plays it. */
        gui()->separator_text( "msel (click / ctrl / shift / ctrl+A)" );
        static bool msel[ 12 ] = { false };
        const i32   n_msel     = (i32)( sizeof( msel ) / sizeof( msel[ 0 ] ) );

        gui()->msel_begin( "msel_demo", n_msel );
        if ( gui()->child_begin( "msel_rows", 0, gui()->sz_rows_h( 6 ), GUI_WIN_NONE ) )
        {
            gui()->stack();
            for ( i32 i = 0; i < n_msel; i++ )
            {
                char row[ 32 ];
                snprintf( row, sizeof( row ), "asset_%02d.png", i );
                gui()->msel_item( row, i, msel[ i ] );
            }
        }
        gui()->child_end();
        gui()->msel_apply( gui()->msel_end(), msel, n_msel );

        i32 n_on = 0;
        for ( i32 i = 0; i < n_msel; i++ )
            n_on += msel[ i ] ? 1 : 0;
        char status[ 48 ];
        snprintf( status, sizeof( status ), "%d of %d selected", n_on, n_msel );
        gui()->label_text( "msel", status );
        if ( gui()->small_button( "Clear selection" ) )
            gui()->msel_apply( ( gui_msel_t ){ GUI_MSEL_CLEAR, 0, 0 }, msel, n_msel );

        /* combo -- one-liner over a string array. */
        gui()->separator_text( "combo (one-liner)" );
        static const char* items[] = { "Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta" };
        const i32          n_items = (i32)( sizeof( items ) / sizeof( items[ 0 ] ) );
        static i32         combo_idx = 0;
        gui()->combo( "combo", &combo_idx, items, n_items );

        /* combo_begin -- full row control + the HEIGHT_* dropdown cap. */
        gui()->separator_text( "combo_begin + height caps" );
        static i32 height_sel = 1;
        gui()->radio_button( "Small",   &height_sel, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Regular", &height_sel, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Large",   &height_sel, 2 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Largest", &height_sel, 3 );
        static const gui_combo_flags_t height_flags[] = {
            GUI_COMBO_HEIGHT_SMALL, GUI_COMBO_HEIGHT_REGULAR,
            GUI_COMBO_HEIGHT_LARGE, GUI_COMBO_HEIGHT_LARGEST,
        };

        static i32 many_idx = 0;
        char preview[ 24 ];
        snprintf( preview, sizeof( preview ), "Item %02d", many_idx );
        if ( gui()->combo_begin( "combo 2", preview, height_flags[ height_sel ] ) )
        {
            for ( i32 i = 0; i < 24; i++ )
            {
                gui()->push_id_int( i );
                char row[ 24 ];
                snprintf( row, sizeof( row ), "Item %02d", i );
                bool on = ( many_idx == i );
                if ( gui()->selectable( row, &on ) )
                    many_idx = i;
                gui()->pop_id();
            }
            gui()->combo_end();
        }

        /* listbox -- one-liner, height in items. */
        gui()->separator_text( "listbox (one-liner)" );
        static const char* fruit[] = { "Apple", "Banana", "Cherry", "Kiwi", "Mango",
                                       "Orange", "Pineapple", "Strawberry" };
        const i32          n_fruit = (i32)( sizeof( fruit ) / sizeof( fruit[ 0 ] ) );
        static i32         fruit_idx = 1;
        gui()->listbox( "fruit", &fruit_idx, fruit, n_fruit, 4 );

        /* listbox_begin -- caller-emitted rows: here, live-filtered by a search box. */
        gui()->separator_text( "listbox_begin (filtered rows)" );
        static char filter[ 24 ] = "";
        gui()->input_text_with_hint( "Filter", "substring...", filter, sizeof( filter ) );
        static i32 row_idx = -1;
        if ( gui()->listbox_begin( "rows", 0.0f, 0.0f ) )
        {
            for ( i32 i = 0; i < n_fruit; i++ )
            {
                if ( filter[ 0 ] && !strstr( fruit[ i ], filter ) )
                    continue;
                gui()->push_id_int( i );
                bool on = ( row_idx == i );
                if ( gui()->selectable( fruit[ i ], &on ) )
                    row_idx = on ? i : -1;
                gui()->pop_id();
            }
            gui()->listbox_end();
        }

        gui()->textf( "combo=%s  combo2=%d  fruit=%s  filtered=%d",
                      items[ combo_idx ], many_idx, fruit[ fruit_idx ], row_idx );
    }
    gui()->window_end();
}

/*==============================================================================================
    Tab Bar -- an in-window tabbed content switcher: only the selected tab's widgets emit, below
    a strip of clickable chips.  (Docking, which tabs whole windows, has its own bed: sb_gui_dock.)
==============================================================================================*/

static void
ex_widgets_tabs( void )
{
    if ( ex_begin( "Tab Bar", 480, 520, GUI_WIN_HSCROLL ) )
    {
        gui()->stack();
        gui()->text( "Only the selected tab's body is emitted, below the chip strip." );
        gui()->separator();

        /* Basic tab bar -- one bar id, several sections; the active selection persists per bar. */
        gui()->separator_text( "tab_bar_begin / tab_item_begin" );
        if ( gui()->tab_bar_begin( "demo_tabs", GUI_TAB_BAR_NONE ) )
        {
            if ( gui()->tab_item_begin( "General", NULL, GUI_TAB_ITEM_NONE ) )
            {
                static bool vsync = true;
                static i32  quality = 2;
                gui()->text( "General settings live on this tab." );
                gui()->checkbox( "Vsync", &vsync );
                gui()->slider_int( "Quality", &quality, 0, 4 );
                gui()->tab_item_end();
            }
            if ( gui()->tab_item_begin( "Audio", NULL, GUI_TAB_ITEM_NONE ) )
            {
                static f32 master = 0.8f, music = 0.5f;
                gui()->slider_float( "Master", &master, 0.0f, 1.0f );
                gui()->slider_float( "Music",  &music,  0.0f, 1.0f );
                gui()->tab_item_end();
            }
            if ( gui()->tab_item_begin( "About", NULL, GUI_TAB_ITEM_NONE ) )
            {
                gui()->text_wrapped( "Same-named widgets on different tabs never collide: "
                                     "the active tab opens its own id scope." );
                gui()->tab_item_end();
            }
            gui()->tab_bar_end();
        }

        /* Closeable tabs -- pass a bool* to get a close (x) on the chip; a "Reopen all" button
           restores the ones the user closed. */
        gui()->separator_text( "closeable tabs (p_open)" );
        static bool open_a = true, open_b = true, open_c = true;
        if ( gui()->button( "Reopen all" ) )
            open_a = open_b = open_c = true;

        if ( gui()->tab_bar_begin( "doc_tabs", GUI_TAB_BAR_NONE ) )
        {
            if ( open_a && gui()->tab_item_begin( "Document A", &open_a, GUI_TAB_ITEM_NONE ) )
            {
                gui()->text( "Contents of A." );
                gui()->tab_item_end();
            }
            if ( open_b && gui()->tab_item_begin( "Document B", &open_b, GUI_TAB_ITEM_NONE ) )
            {
                gui()->text( "Contents of B." );
                gui()->tab_item_end();
            }
            if ( open_c && gui()->tab_item_begin( "Document C", &open_c, GUI_TAB_ITEM_NONE ) )
            {
                gui()->text( "Contents of C." );
                gui()->tab_item_end();
            }
            gui()->tab_bar_end();
        }
        gui()->textf( "open: A=%d B=%d C=%d", open_a, open_b, open_c );
    }
    gui()->window_end();
}

/*==============================================================================================
    Multiline Text -- input_text_multiline: 2D caret, per-line scroll, selection, undo.
==============================================================================================*/

static void
ex_widgets_multiline( void )
{
    if ( ex_begin( "Multiline Text", 560, 560, GUI_WIN_HSCROLL ))
    {
        gui()->stack();
        gui()->text( "Enter inserts a newline; Escape reverts and drops focus." );
        gui()->text( "Arrows/PageUp/PageDn move in 2D; Home/End line-local." );
        gui()->separator();

        /* Default height (eight lines), labeled.  Change count shows the per-frame edit flag. */
        static char notes[ 2048 ] =
            "The quick brown fox\n"
            "jumps over the lazy dog.\n"
            "\n"
            "Third paragraph: select across lines with the mouse or Shift+arrows,\n"
            "double-click drags select by word, Ctrl+Z / Ctrl+Y walk the undo ring.\n"
            "This line is deliberately long enough to overflow the box so the horizontal\n"
            "caret chase and the glyph clip window are visible in action.";
        static i32 edits = 0;
        if ( gui()->input_text_multiline( "Notes", notes, sizeof( notes ), 0.0f ) )
            edits++;
        gui()->textf( "edits: %d   bytes: %d", edits, (i32)strlen( notes ) );

        /* Custom height with a hidden label -- the box takes the full track width.  Enough
           content to overflow vertically, so the gutter scrollbar and wheel scroll engage. */
        gui()->separator_text( "custom height (##hidden label)" );
        static f32  box_h = 120.0f;
        static char log_text[ 4096 ] =
            "line 01\nline 02\nline 03\nline 04\nline 05\nline 06\nline 07\nline 08\n"
            "line 09\nline 10\nline 11\nline 12\nline 13\nline 14\nline 15\nline 16";
        gui()->slider_float( "Box height", &box_h, 60.0f, 300.0f );
        gui()->input_text_multiline( "##log", log_text, sizeof( log_text ), box_h );
    }
    gui()->window_end();
}

/*============================================================================================*/
