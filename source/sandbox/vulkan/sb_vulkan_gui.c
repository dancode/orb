/*==============================================================================================

    sandbox/vulkan/sb_vulkan_gui.c -- gui feature demos.

    One function per feature group, each opening its own window.  Kept small and focused so the
    window on screen reads as a worked example of exactly one part of the gui API; the host
    switches between them with a key.  See sb_vulkan_gui.h for the contract.

    All persistent widget values are file-scope statics local to each demo function -- the demos
    are pure UI, no shared state, so a demo can be read top to bottom in isolation.

==============================================================================================*/

#include <stdio.h>
#include <math.h>      /* cosf / sinf -- the diagonal fan + polygon in demo_lines */

#include "sb_vulkan_gui.h"
#include "runtime_service/gui/gui_host.h"

// clang-format off

/*==============================================================================================
    Procedural icon rasterizers -- exercise the runtime icon atlas.

    The icon atlas takes raw R8 coverage (row-major, w*h bytes, 0..255); pixel sourcing is the
    caller's.  The engine's image/asset pipeline is not online yet, so these draw three simple
    icons (folder / check / gear) straight into a buffer for registration.  Crude on purpose --
    the point is to feed the atlas real bytes, not to be pretty.
==============================================================================================*/

/* Distance from point (px,py) to segment a-b -- used to stroke the checkmark with soft edges. */
static f32
sb_seg_dist( f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by )
{
    f32 dx = bx - ax, dy = by - ay;
    f32 len2 = dx * dx + dy * dy;
    f32 t = len2 > 0.0f ? ( ( px - ax ) * dx + ( py - ay ) * dy ) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : ( t > 1.0f ? 1.0f : t );
    f32 ex = px - ( ax + t * dx );
    f32 ey = py - ( ay + t * dy );
    return sqrtf( ex * ex + ey * ey );
}

/* Coverage for "inside by margin m", with a ~1px soft edge. */
static u8
sb_cov( f32 m )
{
    f32 a = m + 0.5f;
    if ( a <= 0.0f ) return 0;
    if ( a >= 1.0f ) return 255;
    return (u8)( a * 255.0f );
}

static void
sb_make_folder( u8* p, int n )
{
    for ( int y = 0; y < n; ++y )
        for ( int x = 0; x < n; ++x )
        {
            bool body = ( x >= 3 && x <= 28 && y >= 11 && y <= 26 );   /* folder body */
            bool tab  = ( x >= 3 && x <= 14 && y >=  7 && y <= 11 );   /* raised tab  */
            p[ y * n + x ] = ( body || tab ) ? 255 : 0;
        }
}

static void
sb_make_check( u8* p, int n )
{
    for ( int y = 0; y < n; ++y )
        for ( int x = 0; x < n; ++x )
        {
            f32 px = (f32)x + 0.5f, py = (f32)y + 0.5f;
            f32 d1 = sb_seg_dist( px, py,  7.0f, 17.0f, 13.0f, 23.0f );  /* short down-stroke */
            f32 d2 = sb_seg_dist( px, py, 13.0f, 23.0f, 26.0f,  8.0f );  /* long up-stroke    */
            f32 d  = d1 < d2 ? d1 : d2;
            p[ y * n + x ] = sb_cov( 2.5f - d );                        /* ~2.5px half-width */
        }
}

static void
sb_make_gear( u8* p, int n )
{
    f32 cx = (f32)n * 0.5f, cy = (f32)n * 0.5f;
    for ( int y = 0; y < n; ++y )
        for ( int x = 0; x < n; ++x )
        {
            f32 dx = (f32)x + 0.5f - cx, dy = (f32)y + 0.5f - cy;
            f32 rad = sqrtf( dx * dx + dy * dy );
            f32 ang = atan2f( dy, dx );

            bool body = rad <= 11.0f;                                   /* hub disc */
            bool tooth = false;
            if ( rad <= 15.0f )                                         /* 8 teeth along spokes */
            {
                f32 sect = ang / ( 3.14159265f / 4.0f );
                f32 frac = sect - floorf( sect + 0.5f );
                tooth = fabsf( frac ) < 0.30f;
            }
            bool hole = rad <= 5.0f;                                    /* center bore */
            p[ y * n + x ] = ( ( body || tooth ) && !hole ) ? 255 : 0;
        }
}

/*==============================================================================================
    1. Widgets -- the basic interactive controls.

    text / button / checkbox / slider_float / input_text, each on its own auto-height row in a
    stack() (the single-column vertical list).  Every widget returns true on the frame it is
    activated or its value changes; here we just feed that back into a little state so it shows.
==============================================================================================*/

static void
demo_widgets( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 360, 420, GUI_COND_ONCE );
    if ( gui()->window_begin( "Widgets", GUI_WIN_NONE ))
    {
        gui()->stack();                                   /* declare the mode: a vertical list */
        gui()->text( "Basic interactive widgets:" );
        gui()->separator();

        static int clicks = 0;
        if ( gui()->button( "Click me" ) )
            clicks++;
        gui()->textf( "button pressed %d time(s)", clicks );

        gui()->spacing( 0 );

        static bool checked = true;
        gui()->checkbox( "Enable feature", &checked );
        gui()->textf( "feature is %s", checked ? "ON" : "off" );

        gui()->spacing( 0 );

        static f32 amount = 3.0f;
        gui()->slider_float( "Amount", &amount, 0.0f, 10.0f );
        gui()->textf( "amount = %.2f", amount );

        gui()->spacing( 0 );

        static char name[ 32 ] = "orb";
        gui()->input_text( "Name", name, sizeof( name ) );
        gui()->textf( "hello, %s", name );
    }
    gui()->window_end();
}

/*==============================================================================================
    2. Text & sections -- read-only widgets and the section-folding helpers.

    text / textf for runs, bullet_text for lists, separator / separator_text for rules, and
    collapsing_header to fold a block of content (the return value guards the body -- a closed
    header skips everything inside the if).
==============================================================================================*/

static void
demo_text( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 380, 460, GUI_COND_ONCE );
    if ( gui()->window_begin( "Text & Sections", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Plain text line." );
        gui()->textf( "Formatted: pi ~= %.4f, frame %d", 3.14159f, 42 );

        gui()->separator_text( "A bullet list" );
        gui()->bullet_text( "first item" );
        gui()->bullet_text( "second item" );
        gui()->bullet_text( "third item" );

        gui()->separator();

        /* collapsing_header returns its open state; guard the body with it. */
        if ( gui()->collapsing_header( "Details (click to fold)" ) )
        {
            gui()->text( "These lines only draw while the" );
            gui()->text( "header above is expanded." );
            gui()->textf( "value: %d", 1234 );
        }

        if ( gui()->collapsing_header( "More details" ) )
        {
            gui()->bullet_text( "another folded section" );
            gui()->bullet_text( "independent open state" );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    3. Layout rows -- shaping the repeating row template.

    A region declares its mode with a header: stack() is the single flex column, while row_cols / row_cols_n /
    row2..4 install a multi-column flow template.  The template persists and repeats
    for every following widget until set again.  Sizes use one overloaded f32: > 1 pixels, 1.0
    fill (equal share of the leftover), (0,1) a fraction, 0 natural.
==============================================================================================*/

static void
demo_layout_rows( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 440, 420, GUI_COND_ONCE );
    if ( gui()->window_begin( "Layout Rows", GUI_WIN_NONE ) )
    {
        gui()->stack();                         /* heading lines sit in a plain stack */
        gui()->text( "row_cols_n( 0, 3 ) -- three equal columns:" );
        gui()->row_cols_n( 0, 3 );
        gui()->button( "A" );
        gui()->button( "B" );
        gui()->button( "C" );
        gui()->row( 0 );                        /* back to a single column */

        gui()->text( "row2( 0.3, 0.7 ) -- weighted 30/70:" );
        gui()->row2( 0.3f, 0.7f );
        gui()->button( "30%" );
        gui()->button( "70%" );
        gui()->row( 0 );

        gui()->text( "row_cols -- 120px + fill + 80px:" );
        gui()->row_cols( 0, ( f32[] ){ 120, 1, 80, GUI_END } );
        gui()->button( "fixed 120" );
        gui()->button( "fill" );
        gui()->button( "80" );
        gui()->row( 0 );

        gui()->text( "row( 48 ) -- one tall 48px row:" );
        gui()->row( 48 );
        gui()->button( "tall button" );
        gui()->row( 0 );
    }
    gui()->window_end();
}

/*==============================================================================================
    4. Field forms -- aligned "Label  [control]" rows from a single widget call.

    field_split (and its field_label_left / field_label_right sugar) splits a labeled widget's
    cell into a label track + a control track, so input_text / slider_float / checkbox lay out as
    a form without pairing a separate text() with each control.  The split persists until cleared
    with width 0.
==============================================================================================*/

static void
demo_fields( void )
{
    static char f_name[ 32 ] = "player";
    static f32  f_speed      = 5.0f;
    static f32  f_volume     = 7.0f;
    static bool f_enabled    = true;

    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 400, 420, GUI_COND_ONCE );
    if ( gui()->window_begin( "Field Forms", GUI_WIN_NONE ) )
    {
        gui()->stack();
        /* Each section reuses the "Name"/"Enabled" labels, so scope them with push_id to keep
           the widget ids distinct (ids are seeded by label within a region). */
        gui()->text( "field_label_left( 90 ) -- labels in a 90px gutter:" );
        gui()->push_id( "left" );
        gui()->field_label_left( 90.0f );
        gui()->input_text  ( "Name",    f_name, sizeof( f_name ) );
        gui()->slider_float( "Speed",   &f_speed, 0.0f, 10.0f );
        gui()->checkbox    ( "Enabled", &f_enabled );
        gui()->field_label_left( 0.0f );        /* clear the split */
        gui()->pop_id();

        gui()->spacing( 0 );
        gui()->text( "field_label_right( 90 ) -- labels on the right:" );
        gui()->push_id( "right" );
        gui()->field_label_right( 90.0f );
        gui()->input_text  ( "Name",    f_name, sizeof( f_name ) );
        gui()->checkbox    ( "Enabled", &f_enabled );
        gui()->field_label_right( 0.0f );
        gui()->pop_id();

        gui()->spacing( 0 );
        gui()->text( "field_split( LEFT, 0.4, 0.6 ) -- fractional:" );
        gui()->field_split( GUI_LABEL_LEFT, 0.4f, 0.6f );
        gui()->slider_float( "Volume", &f_volume, 0.0f, 10.0f );
        gui()->field_label_left( 0.0f );
    }
    gui()->window_end();
}

/*==============================================================================================
    5. Grid -- a fixed cols x rows matrix in a bounded box.

    grid_cells( nc, nr ) partitions the region content (from the pen to the bottom) into a fixed
    matrix; widgets fill cells row-major and nothing scrolls.  skip() leaves one cell blank -- the
    natural way to step over a slot.
==============================================================================================*/

static void
demo_grid( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 420, 440, GUI_COND_ONCE );
    if ( gui()->window_begin( "Grid", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "grid_cells( 3, 3 ) -- 3x3, row-major:" );

        /* The grid fills the remaining content box, so put it in a fixed-height child
           and leave a note below it. */
        if ( gui()->child_begin( "grid_box", 0, 300, GUI_WIN_NOSCROLL ) )
        {
            gui()->grid_cells( 3, 3 );
            for ( int i = 0; i < 9; i++ )
            {
                if ( i == 4 )
                {
                    gui()->skip();              /* leave the center cell empty */
                    continue;
                }
                char label[ 8 ];
                snprintf( label, sizeof( label ), "%d", i );
                gui()->button( label );
            }
        }
        gui()->child_end();

        gui()->text( "center cell skipped with skip()" );
    }
    gui()->window_end();
}

/*==============================================================================================
    6. Child region / list box -- an independently scrolled, clipped sub-box.

    child_begin carves a box of height h that scrolls and clips on its own; fill it with
    selectable rows to make a list box.  selectable toggles *on and returns true on the clicked
    frame, so the caller can manage single-selection by index.  push_id_int keeps row ids distinct.
==============================================================================================*/

static void
demo_child_list( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 360, 440, GUI_COND_ONCE );
    if ( gui()->window_begin( "Child / List Box", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "List box (scrolls independently):" );

        static int sel = -1;
        if ( gui()->child_begin( "rows", 0, 240, GUI_WIN_NONE ) )
        {
            gui()->stack();                     /* the child is its own region -- declare its mode */
            for ( int i = 0; i < 40; i++ )
            {
                gui()->push_id_int( i );        /* distinct id even though labels repeat */
                char row[ 32 ];
                snprintf( row, sizeof( row ), "row item %02d", i );
                bool on = ( sel == i );
                if ( gui()->selectable( row, &on ) )
                    sel = on ? i : -1;            /* single-selection */
                gui()->pop_id();
            }
        }
        gui()->child_end();

        gui()->textf( "selected: %d (this line is below the box)", sel );

        /* Vertically resizeable child: drag the bottom border to change its height.  The size is
           user-owned and persisted, so the rows below it move as the box grows / shrinks. */
        gui()->help_marker( "Drag the bottom border to resize this box vertically." );
        if ( gui()->child_begin( "resizeable", 0, 120, GUI_WIN_CHILD_RESIZE_Y ) )
        {
            gui()->stack();
            for ( int i = 0; i < 12; i++ )
                gui()->textf( "resizeable line %02d", i );
        }
        gui()->child_end();

        gui()->text( "this line sits below the resizeable box" );

        /* Auto-resize with a height cap: the box hugs its content (AutoResizeY, h <= 0) but
           window_set_next_size_constraints caps it at max_lines rows -- beyond that it stops
           growing and scrolls.  The two sliders drive the emitted line count and the cap so the
           transition from hugging to scrolling is visible. */
        gui()->separator_text( "Auto-resize with constraints" );

        static i32 draw_lines = 3;
        static i32 max_lines  = 10;
        gui()->drag_int( "Lines Count",     &draw_lines, 0.2f, 0,  30, "%d" );
        gui()->drag_int( "Max (in lines)",  &max_lines,  0.2f, 1,  20, "%d" );

        f32 line = gui()->line_h() + gui()->h_min();   /* one row plus its standard margin */
        gui()->window_set_next_size_constraints( 0.0f, line, 0.0f, line * (f32)max_lines );
        if ( gui()->child_begin( "constrained", 0, 0, GUI_WIN_NONE ) )
        {
            gui()->stack();
            for ( i32 n = 0; n < draw_lines; n++ )
                gui()->textf( "Line %04d", n );
        }
        gui()->child_end();
    }
    gui()->window_end();
}

/*==============================================================================================
    6b. Combo box & list box -- single-selection from a set.

    combo() drops a popup of rows below a framed preview box; listbox() shows a scrolling box of
    rows.  Both come as a one-liner over a string array and a generic begin/end form that gives full
    control over the rows (a filter, custom row widgets, a default-focus highlight).
==============================================================================================*/

static void
demo_combo_list( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 380, 460, GUI_COND_ONCE );
    if ( gui()->window_begin( "Combo / List Box", GUI_WIN_NONE ) )
    {
        gui()->stack();

        static const char* items[] = { "AAAA", "BBBB", "CCCC", "DDDD", "EEEE", "FFFF",
                                       "GGGG", "HHHH", "IIIIIII", "JJJJ", "KKKKKKK" };
        const i32          n_items  = (i32)( sizeof( items ) / sizeof( items[ 0 ] ) );

        /* One-liner combo over the array -- the everyday case. */
        gui()->separator_text( "Combo (one-liner)" );
        static i32 combo_idx = 0;
        gui()->combo( "combo", &combo_idx, items, n_items );
        gui()->same_line( -1.0f );
        gui()->help_marker( "combo() over an array of strings; the dropdown drops below the box." );

        /* Dropdown height cap (gui_combo_flags_t HEIGHT_*): mutually exclusive, so pick with a
           radio group.  Applied to the BeginCombo dropdown below. */
        gui()->separator_text( "BeginCombo (full control)" );
        static i32 height_sel = 1;   /* 0 small, 1 regular, 2 large, 3 largest */
        gui()->text( "Popup height:" );
        gui()->radio_button( "Small",   &height_sel, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Regular", &height_sel, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Large",   &height_sel, 2 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Largest", &height_sel, 3 );

        const gui_combo_flags_t height_flags[] = {
            GUI_COMBO_HEIGHT_SMALL, GUI_COMBO_HEIGHT_REGULAR,
            GUI_COMBO_HEIGHT_LARGE, GUI_COMBO_HEIGHT_LARGEST,
        };

        /* Generic begin/end combo over a longer set so the height cap is visible (it scrolls past
           the cap).  A clicked row dismisses the combo automatically. */
        static const char* many[] = {
            "Item 00", "Item 01", "Item 02", "Item 03", "Item 04", "Item 05", "Item 06", "Item 07",
            "Item 08", "Item 09", "Item 10", "Item 11", "Item 12", "Item 13", "Item 14", "Item 15",
            "Item 16", "Item 17", "Item 18", "Item 19", "Item 20", "Item 21", "Item 22", "Item 23",
        };
        const i32 n_many = (i32)( sizeof( many ) / sizeof( many[ 0 ] ) );
        static i32  sel_idx = 0;
        const char* preview = many[ sel_idx ];
        if ( gui()->combo_begin( "combo 2", preview, height_flags[ height_sel ] ) )
        {
            for ( i32 i = 0; i < n_many; i++ )
            {
                gui()->push_id_int( i );
                bool is_sel = ( sel_idx == i );
                if ( gui()->selectable( many[ i ], &is_sel ) )
                    sel_idx = i;
                gui()->pop_id();
            }
            gui()->combo_end();
        }

        /* One-liner list box, height in items. */
        gui()->separator_text( "List box (one-liner)" );
        static const char* fruit[] = { "Apple", "Banana", "Cherry", "Kiwi", "Mango",
                                       "Orange", "Pineapple", "Strawberry", "Watermelon" };
        const i32          n_fruit = (i32)( sizeof( fruit ) / sizeof( fruit[ 0 ] ) );
        static i32         fruit_idx = 1;
        gui()->listbox( "listbox", &fruit_idx, fruit, n_fruit, 4 );

        /* Generic begin/end list box -- emit the rows yourself (default size). */
        gui()->separator_text( "BeginListBox (full control)" );
        static i32 row_idx = -1;
        if ( gui()->listbox_begin( "rows", 0.0f, 0.0f ) )
        {
            for ( i32 i = 0; i < 24; i++ )
            {
                gui()->push_id_int( i );
                char row[ 32 ];
                snprintf( row, sizeof( row ), "list row %02d", i );
                bool on = ( row_idx == i );
                if ( gui()->selectable( row, &on ) )
                    row_idx = on ? i : -1;
                gui()->pop_id();
            }
            gui()->listbox_end();
        }

        gui()->textf( "combo=%s  combo2=%s  fruit=%s  row=%d",
                        items[ combo_idx ], many[ sel_idx ], fruit[ fruit_idx ], row_idx );
    }
    gui()->window_end();
}

/*==============================================================================================
    7. Alignment, same_line & spacers -- composition within a row.

    align() sets where natural-sized content sits in its cell (persists like the row template).
    same_line keeps the next widget on the line just emitted.  skip / spacing / separator are the
    cell-consuming spacers that emit nothing interactive.
==============================================================================================*/

static void
demo_align( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 380, 400, GUI_COND_ONCE );
    if ( gui()->window_begin( "Align & Spacing", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "align() across two columns:" );

        gui()->row2( 0.5f, 0.5f );
        gui()->align( GUI_ALIGN_LEFT );
        gui()->text( "left" );
        gui()->align( GUI_ALIGN_RIGHT );
        gui()->text( "right" );
        gui()->align( GUI_ALIGN_LEFT );       /* restore */
        gui()->row( 0 );

        gui()->separator();

        gui()->text( "same_line keeps widgets on one line:" );
        gui()->button( "OK" );
        gui()->same_line( 8.0f );
        gui()->button( "Cancel" );
        gui()->same_line( 8.0f );
        gui()->button( "Apply" );

        gui()->separator_text( "spacing()" );
        gui()->text( "above a 32px gap" );
        gui()->spacing( 32.0f );
        gui()->text( "below the gap" );
    }
    gui()->window_end();
}

/*==============================================================================================
    8. Sub-layout -- a transient nested layout inside one cell.

    push_layout consumes one cell and opens a little layout filling it (shape it with row /
    widgets inside); pop_layout closes it and the parent resumes at the next cell.  Here column 0
    of a 2-column row holds a stack of buttons while column 1 stays a single label.
==============================================================================================*/

static void
demo_sublayout( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 400, 360, GUI_COND_ONCE );
    if ( gui()->window_begin( "Sub-layout", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "row_cols_n( 0, 2 ): col 0 is a sub-layout" );
        gui()->separator();

        gui()->row_cols_n( 0, 2 );

        /* Column 0: open a sub-layout and stack three buttons inside the single cell. */
        gui()->push_layout();
            gui()->stack();                     /* the sub-layout is a region too -- declare its mode */
            gui()->text( "stacked:" );
            gui()->button( "one" );
            gui()->button( "two" );
            gui()->button( "three" );
        gui()->pop_layout();

        /* Column 1: a single widget filling the other cell. */
        gui()->text( "single cell" );

        gui()->row( 0 );
    }
    gui()->window_end();
}

/*==============================================================================================
    8b. Pack / bar -- the print run: natural-size widgets placed one after another.

    bar() packs items left to right at their natural width (a toolbar); pack_size() sets the next
    item's measure (1 = fill the rest of the line), and pack_nextline() wraps to a new line.  Here
    the widget sizes itself, unlike columns / grid where the cell sizes the widget.
==============================================================================================*/

static void
demo_pack( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 440, 320, GUI_COND_ONCE );
    if ( gui()->window_begin( "Pack / Bar", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "bar() -- buttons at natural width, then a fill search box:" );

        gui()->bar();
        gui()->button( "New" );
        gui()->button( "Open" );
        gui()->button( "Save" );
        static char find[ 32 ] = "";
        gui()->pack_size( 1.0f );                   /* the next item fills the rest of the line */
        gui()->input_text( "##find", find, sizeof( find ) );

        gui()->stack();
        gui()->spacing( 0 );
        gui()->text( "pack_nextline() wraps the run -- three per line:" );

        gui()->bar();
        for ( int i = 0; i < 9; i++ )
        {
            gui()->push_id_int( i );
            char b[ 12 ];
            snprintf( b, sizeof( b ), "btn %d", i );
            gui()->button( b );
            if ( i % 3 == 2 ) gui()->pack_nextline();
            gui()->pop_id();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    9. Windows -- multiple panels, flags, and z-order.

    Three overlapping windows show the per-window flags (a default window, a no-title-bar panel,
    a fixed/no-resize panel) and z-order: click any window to bring it to the front, drag to move.
==============================================================================================*/

static void
demo_windows( void )
{
    gui()->window_set_next_pos ( 80, 80, GUI_COND_ONCE );
    gui()->window_set_next_size( 280, 200, GUI_COND_ONCE );
    if ( gui()->window_begin( "Default Window", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Default flags." );
        gui()->text( "Title bar, collapse, resize." );
        gui()->text( "Click another window to raise it." );
    }
    gui()->window_end();

    gui()->window_set_next_pos ( 220, 180, GUI_COND_ONCE );
    gui()->window_set_next_size( 280, 180, GUI_COND_ONCE );
    if ( gui()->window_begin( "No Title Bar", GUI_WIN_NOTITLEBAR ) )
    {
        gui()->stack();
        gui()->text( "GUI_WIN_NOTITLEBAR" );
        static bool t = false;
        gui()->checkbox( "a toggle", &t );
    }
    gui()->window_end();

    gui()->window_set_next_pos ( 360, 280, GUI_COND_ONCE );
    gui()->window_set_next_size( 260, 160, GUI_COND_ONCE );
    if ( gui()->window_begin( "Fixed", GUI_WIN_NORESIZE | GUI_WIN_NOMOVE ) )
    {
        gui()->stack();
        gui()->text( "GUI_WIN_NORESIZE | NOMOVE" );
        gui()->text( "cannot be moved or resized." );
    }
    gui()->window_end();

    /* A closeable window: the X at the title bar's right edge hides it.  Re-opening is the host's
       job -- the control window below offers a button that calls window_set_open. */
    gui()->window_set_next_pos ( 500, 120, GUI_COND_ONCE );
    gui()->window_set_next_size( 260, 150, GUI_COND_ONCE );
    if ( gui()->window_begin( "Closeable", GUI_WIN_CLOSEABLE ) )
    {
        gui()->stack();
        gui()->text( "GUI_WIN_CLOSEABLE" );
        gui()->text( "Click the X to close me," );
        gui()->text( "then re-open from the control" );
        gui()->text( "window below.  Detach me first" );
        gui()->text( "and close: I re-open as a" );
        gui()->text( "floater in the same spot." );
    }
    gui()->window_end();

    /* Control window: shows the closeable window's state and re-opens it on demand.  The button is
       disabled while the window is already open, so the only way to open it is after the X closed it. */
    gui()->window_set_next_pos ( 500, 290, GUI_COND_ONCE );
    gui()->window_set_next_size( 260, 110, GUI_COND_ONCE );
    if ( gui()->window_begin( "Window Control", GUI_WIN_NONE ) )
    {
        bool open = gui()->window_is_open( "Closeable" );

        gui()->stack();
        gui()->textf( "Closeable window is %s.", open ? "open" : "closed" );

        gui()->disabled_begin( open );
        if ( gui()->button( "Open Closeable window" ) )
            gui()->window_set_open( "Closeable", true );
        gui()->disabled_end();
    }
    gui()->window_end();
}

/*==============================================================================================
    10. Auto-size -- windows and children that hug their content.

    GUI_WIN_ALWAYS_AUTOSIZE makes a window recompute its size from its content every frame (no
    user resize, no scrollbars); add/remove rows and the window grows and shrinks to fit.
    GUI_WIN_CAN_AUTOSIZE keeps a normal resizeable window but adds a corner size-grip you
    double-click to snap it to its content.  A child_begin with h <= 0 auto-sizes its height the
    same way, and content_avail() reports the space left in the current region.
==============================================================================================*/

static void
demo_autosize( void )
{
    /* (a) A window that always hugs its content -- the row count drives its height. */
    gui()->window_set_next_pos( 60, 60, GUI_COND_ONCE );
    if ( gui()->window_begin( "Always Auto-size", GUI_WIN_ALWAYS_AUTOSIZE ) )
    {
        static int rows = 3;

        gui()->stack();
        gui()->text( "ALWAYS_AUTOSIZE: window fits content." );
        gui()->row2( 0.5f, 0.5f );
        if ( gui()->button( "Add row" ) )    rows++;
        if ( gui()->button( "Remove row" ) ) rows = rows > 0 ? rows - 1 : 0;
        gui()->row( 0 );

        for ( int i = 0; i < rows; i++ )
            gui()->textf( "content row %d", i );
    }
    gui()->window_end();

    /* (b) A normal window with a corner grip; double-click it to fit. */
    gui()->window_set_next_pos ( 360, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 300, 320, GUI_COND_ONCE );
    if ( gui()->window_begin( "Double-click grip", GUI_WIN_CAN_AUTOSIZE ) )
    {
        gui()->stack();
        gui()->text( "CAN_AUTOSIZE: drag the triangle" );
        gui()->text( "grip in the corner to resize," );
        gui()->text( "or double-click it to snap back" );
        gui()->text( "to fit this content." );
    }
    gui()->window_end();

    /* (c) Auto-height child (h <= 0) + content_avail(). */
    gui()->window_set_next_pos ( 360, 420, GUI_COND_ONCE );
    gui()->window_set_next_size( 320, 260, GUI_COND_ONCE );
    if ( gui()->window_begin( "Auto child", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "child_begin with h <= 0 hugs content:" );
        if ( gui()->child_begin( "auto", 0, 0, GUI_WIN_NOSCROLL ) )
        {
            gui()->stack();                     /* the child region declares its own mode */
            gui()->text( "I am exactly as tall" );
            gui()->text( "as my three lines." );
            gui()->bullet_text( "no fixed height" );
        }
        gui()->child_end();

        gui_vec2_t avail = gui()->content_avail();
        gui()->textf( "content_avail below: %.0f x %.0f", avail.x, avail.y );
    }
    gui()->window_end();
}

/*==============================================================================================
    11. Menus -- menu bars, menu entries, menu items, submenus, and context menus.

    menu_bar_begin fills the strip a window reserves with GUI_WIN_MENUBAR; menu_begin opens a
    submenu popup (horizontal in the bar, a row with an arrow inside a menu -- it nests); menu_item
    is a leaf command (optional "shortcut" text on the right, optional bool* for a checkable item).
    A menu_item click dismisses the whole menu chain.  Disabled items reuse the item-flag stack.
    The same menu_item / menu_begin work inside any popup, so a right-click context menu is built
    the same way, and main_menu_bar_begin pins a bar across the top of the display.
==============================================================================================*/

/* Shared across the bars + context menu so a chosen command is visible below. */
static const char* s_menu_last   = "(none)";
static bool        s_menu_grid    = true;
static bool        s_menu_stats   = false;
static bool        s_menu_mainbar = false;

/* The File menu body, factored out so the menu bar and the context menu can both show it
   (mirroring the Dear ImGui sample's ShowExampleMenuFile reuse). */
static void
demo_menu_file( void )
{
    if ( gui()->menu_item( "New",  NULL,     NULL ) ) s_menu_last = "File / New";
    if ( gui()->menu_item( "Open", "Ctrl+O", NULL ) ) s_menu_last = "File / Open";

    /* A submenu: menu_begin inside a menu renders a row with a right arrow and opens to the side. */
    if ( gui()->menu_begin( "Open Recent" ) )
    {
        if ( gui()->menu_item( "scene.orb",  NULL, NULL ) ) s_menu_last = "Recent / scene.orb";
        if ( gui()->menu_item( "level_1.orb", NULL, NULL ) ) s_menu_last = "Recent / level_1.orb";
        if ( gui()->menu_begin( "More.." ) )            /* nested submenu */
        {
            if ( gui()->menu_item( "old.orb",   NULL, NULL ) ) s_menu_last = "Recent / old.orb";
            if ( gui()->menu_item( "older.orb", NULL, NULL ) ) s_menu_last = "Recent / older.orb";
            gui()->menu_end();
        }
        gui()->menu_end();
    }

    if ( gui()->menu_item( "Save",    "Ctrl+S", NULL ) ) s_menu_last = "File / Save";
    if ( gui()->menu_item( "Save As..", NULL,   NULL ) ) s_menu_last = "File / Save As";
    gui()->separator();
    if ( gui()->menu_item( "Quit", "Alt+F4", NULL ) ) s_menu_last = "File / Quit";
}

static void
demo_menus( void )
{
    /* (a) Optional full-width bar pinned to the top of the display.  Toggled from the body below;
       drawn first (immediate mode), so the toggle takes effect the next frame. */
    if ( s_menu_mainbar && gui()->main_menu_bar_begin() )
    {
        if ( gui()->menu_begin( "App" ) )
        {
            if ( gui()->menu_item( "About", NULL,     NULL ) ) s_menu_last = "App / About";
            if ( gui()->menu_item( "Quit",  "Alt+F4", NULL ) ) s_menu_last = "App / Quit";
            gui()->menu_end();
        }
        if ( gui()->menu_begin( "Help" ) )
        {
            if ( gui()->menu_item( "Documentation", "F1", NULL ) ) s_menu_last = "Help / Docs";
            gui()->menu_end();
        }
        gui()->main_menu_bar_end();
    }

    /* (b) A window with its own menu bar (GUI_WIN_MENUBAR reserves the strip). */
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 420, 320, GUI_COND_ONCE );
    if ( gui()->window_begin( "Menus", GUI_WIN_MENUBAR ) )
    {
        if ( gui()->menu_bar_begin() )
        {
            if ( gui()->menu_begin( "File" ) )
            {
                demo_menu_file();
                gui()->menu_end();
            }
            if ( gui()->menu_begin( "Edit" ) )
            {
                if ( gui()->menu_item( "Undo", "Ctrl+Z", NULL ) ) s_menu_last = "Edit / Undo";

                /* A disabled item -- reuse the item-flag stack (no per-widget enabled param). */
                gui()->push_item_flag( GUI_ITEM_DISABLED, true );
                gui()->menu_item( "Redo", "Ctrl+Y", NULL );
                gui()->pop_item_flag();

                gui()->separator();
                if ( gui()->menu_item( "Cut",   "Ctrl+X", NULL ) ) s_menu_last = "Edit / Cut";
                if ( gui()->menu_item( "Copy",  "Ctrl+C", NULL ) ) s_menu_last = "Edit / Copy";
                if ( gui()->menu_item( "Paste", "Ctrl+V", NULL ) ) s_menu_last = "Edit / Paste";
                gui()->menu_end();
            }
            if ( gui()->menu_begin( "View" ) )
            {
                /* Checkable items: pass a bool* -- the tick reflects + toggles the flag. */
                gui()->menu_item( "Show grid",  NULL, &s_menu_grid );
                gui()->menu_item( "Show stats", NULL, &s_menu_stats );
                gui()->menu_end();
            }
            gui()->menu_bar_end();
        }

        gui()->stack();
        gui()->text( "Use the menu bar above." );
        gui()->textf( "Last command: %s", s_menu_last );
        gui()->textf( "grid: %s   stats: %s",
                        s_menu_grid ? "ON" : "off", s_menu_stats ? "ON" : "off" );
        gui()->separator();
        gui()->checkbox( "Show main menu bar (top of screen)", &s_menu_mainbar );
        gui()->spacing( 0 );
        gui()->text( "Right-click empty space for a context menu." );

        /* (c) A context menu: popup_context_window_begin opens on a right-click in empty window
           space, and is filled with the same menu_item / menu_begin calls. */
        if ( gui()->popup_context_window_begin( "ctx" ) )
        {
            gui()->stack();                 /* a popup body declares its layout mode */
            if ( gui()->menu_begin( "File" ) )
            {
                demo_menu_file();
                gui()->menu_end();
            }
            gui()->separator();
            if ( gui()->menu_item( "Reset grid", NULL, NULL ) ) { s_menu_grid = true;  s_menu_last = "ctx / Reset grid"; }
            gui()->menu_item( "Show stats", NULL, &s_menu_stats );
            gui()->popup_end();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    12. Lines & paths -- draw_line / draw_polyline / path_stroke.

    A drawing demo rather than a widget one: each example reserves a canvas() block in the normal
    vertical list and strokes directly into the screen rect it returns.  The Thickness slider and
    the alignment radio drive the examples live: pixel-crisp axis-aligned lines, antialiased
    diagonals, the four stroke alignments against the ideal path, closed shapes (square / circle),
    adjustable mitered corners, and a path-built polygon / star.
==============================================================================================*/

/* Palette shared by the line canvases. */
#define LINE_INK    GUI_COLOR( 0xE6, 0xE6, 0xE6, 0xFF )   /* near-white stroke    */
#define LINE_CYAN   GUI_COLOR( 0x4F, 0xC3, 0xF7, 0xFF )   /* cool accent          */
#define LINE_AMBR   GUI_COLOR( 0xFF, 0xB0, 0x40, 0xFF )   /* warm accent (opaque) */
#define LINE_AMBR_T GUI_COLOR( 0xFF, 0xB0, 0x40, 0xC8 )   /* warm, translucent    */
#define LINE_PATH   GUI_COLOR( 0xFF, 0xFF, 0xFF, 0xFF )   /* the ideal path / guide */
#define LINE_BG     GUI_COLOR( 0x1E, 0x1E, 0x1E, 0xFF )   /* canvas backdrop      */

static const f32 PI = 3.14159265f;

/* Stroke a closed point ring: optionally the ideal path as a 1px white guide, then the thick
   selected stroke translucent on top -- so inside / center / outside reads against the guide. */
static void
line_shape( const gui_vec2_t* pts, u32 n, f32 thickness, gui_stroke_align_t align, bool guide )
{
    if ( guide )
        gui()->draw_polyline( pts, n, 1.0f, GUI_STROKE_CENTER, true, LINE_PATH );
    gui()->draw_polyline( pts, n, thickness, align, true, LINE_AMBR_T );
}

static void
demo_lines( void )
{
    /* Interactive parameters driven by the widgets below. */
    static i32  thickness_px = 6;     /* shared stroke width (integer-snapped via slider_int) */
    static int  align_idx = 0;        /* index into the alignment table      */
    static f32  spread    = 1.0f;     /* zig-zag corner amplitude (0..1)     */
    static int  poly_pts  = 5;        /* sides of the path-built polygon     */
    static bool star_mode = true;     /* star vs convex polygon              */
    static bool show_guides = true;   /* draw the 1px white ideal-path guides */

    static const char*           align_names[ 4 ] = { "CENTER_BIASED", "CENTER", "INSIDE", "OUTSIDE" };
    static const gui_stroke_align_t align_mode[ 4 ] = {
        GUI_STROKE_CENTER_BIASED, GUI_STROKE_CENTER, GUI_STROKE_INSIDE, GUI_STROKE_OUTSIDE };

    gui()->window_set_next_pos ( 80, 40, GUI_COND_ONCE );
    gui()->window_set_next_size( 480, 660, GUI_COND_ONCE );
    if ( gui()->window_begin( "Lines & Paths", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "draw_line / draw_polyline / path_stroke into a canvas()." );

        /* ---- controls ---- */
        gui()->separator_text( "Parameters" );
        gui()->slider_int( "Thickness", &thickness_px, 0, 16 );   /* 0 hides the stroke -> guide only */
        f32 thickness = (f32)thickness_px;                          /* used as f32 by the draw calls */
        gui()->textf( "alignment: %s", align_names[ align_idx ] );

        gui()->bar();                                  /* a horizontal run of radio buttons */
        gui()->radio_button( "Biased",  &align_idx, 0 );
        gui()->radio_button( "Center",  &align_idx, 1 );
        gui()->radio_button( "Inside",  &align_idx, 2 );
        gui()->radio_button( "Outside", &align_idx, 3 );
        gui()->stack();                                /* back to the vertical list */

        gui()->checkbox( "Show guide lines", &show_guides );

        /* A custom-drawn, interactive control built entirely from the primitives: dummy() reserves a
           cell, is_mouse_hovering_rect() drives the hover tint, draw_text_in() centers the caption,
           and invisible_button() makes the same rect clickable -- cycling the alignment on a click. */
        {
            gui_rect_t sw  = gui()->dummy( 0.0f, 22.0f );        /* full-width strip, 22px tall */
            bool         hot = gui()->is_mouse_hovering_rect( sw );
            gui()->draw_rect( sw.x, sw.y, sw.w, sw.h, hot ? LINE_CYAN : LINE_BG );
            gui()->draw_text_in( sw, GUI_ALIGN_CENTER, LINE_INK, "click: cycle alignment" );
            if ( gui()->invisible_button( "cycle##align", sw ) )
                align_idx = ( align_idx + 1 ) % 4;
        }

        /* ---- (a) crisp axis-aligned ladder ---- */
        gui()->separator_text( "Axis-aligned: crisp 1..6 px (pixel-snapped)" );
        {
            gui_rect_t r = gui()->canvas( 168.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );

            /* Carve the canvas declaratively: pad it, then per rung cut a row off the top and a
               label column off that row's right.  The line fills what is left, the tag aligns in the
               column -- no absolute offsets, so nothing can spill the border. */
            gui_rect_t area = gui_rect_pad( r, 12.0f );
            f32          lblw = gui()->text_w( "16 px" ) + 12.0f;
            for ( int t = 1; t <= 6; ++t )
            {
                gui_rect_t row = gui_rect_cut_top( &area, 24.0f );
                gui_rect_t lbl = gui_rect_cut_right( &row, lblw );
                f32          cy  = row.y + row.h * 0.5f;
                char         tag[ 8 ];
                snprintf( tag, sizeof( tag ), "%d px", t );
                gui()->draw_line( row.x, cy, row.x + row.w, cy, (f32)t, LINE_INK );
                gui()->draw_text_in( lbl, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER, LINE_INK, tag );
            }
        }

        /* ---- (b) antialiased diagonal fan (uses Thickness) ---- */
        gui()->separator_text( "Antialiased diagonals (Thickness)" );
        {
            gui_rect_t r = gui()->canvas( 140.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );
            f32 cx  = r.x + 24.0f;
            f32 cy  = r.y + r.h - 20.0f;
            f32 len = ( r.h - 36.0f );
            for ( int i = 0; i <= 8; ++i )
            {
                f32 a = ( PI * 0.5f ) * (f32)i / 8.0f;     /* sweep 0..90 deg */
                gui()->draw_line( cx, cy, cx + cosf( a ) * len, cy - sinf( a ) * len,
                                    thickness, LINE_CYAN );
            }
        }

        /* ---- (c) the four alignments against the ideal path (uses Thickness + the radio) ---- */
        gui()->separator_text( "Stroke alignment vs the ideal path" );
        {
            gui_rect_t r = gui()->canvas( 162.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );

            gui_rect_t area = gui_rect_pad( r, 12.0f );

            /* Caption: reserve a top strip (so the layout is stable whether or not it draws) and
               right-align the text in it -- the spill is impossible, the rect owns the right edge. */
            gui_rect_t cap = gui_rect_cut_top( &area, gui()->line_h() );
            if ( show_guides )
                gui()->draw_text_in( cap, GUI_ALIGN_RIGHT | GUI_ALIGN_VCENTER,
                                       LINE_PATH, "white = ideal path" );

            /* Cut a label column sized to the widest name; `area` is left as the segment region, so a
               long label like CENTER_BIASED can never run under the lines. */
            f32 label_w = 0.0f;
            for ( int i = 0; i < 4; ++i )
            {
                f32 w = gui()->text_w( align_names[ i ] );
                if ( w > label_w ) label_w = w;
            }
            gui_rect_t labels = gui_rect_cut_left( &area, label_w + 12.0f );
            for ( int i = 0; i < 4; ++i )
            {
                f32          ly       = area.y + 16.0f + (f32)i * 30.0f;
                gui_vec2_t seg[ 2 ] = { { area.x, ly }, { area.x + area.w, ly } };
                gui()->draw_polyline( seg, 2, thickness, align_mode[ i ], false, LINE_AMBR_T );
                if ( show_guides )
                    gui()->draw_line( area.x, ly, area.x + area.w, ly, 1.0f, LINE_PATH );

                gui_rect_t lbl = { labels.x, ly - gui()->line_h() * 0.5f, labels.w, gui()->line_h() };
                gui()->draw_text_in( lbl, GUI_ALIGN_LEFT | GUI_ALIGN_VCENTER,
                                       i == align_idx ? LINE_CYAN : LINE_INK, align_names[ i ] );
            }
        }

        /* ---- (d) closed shapes: square + circle (uses Thickness + alignment radio) ---- */
        gui()->separator_text( "Closed shapes: draw_polyline (square / circle)" );
        {
            gui_rect_t r = gui()->canvas( 200.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );
            f32 cy = r.y + r.h * 0.5f - 6.0f;

            /* square -- four corners, the same "shape technique" as any polygon */
            f32 sx = r.x + r.w * 0.27f, hs = 44.0f;
            gui_vec2_t sq[ 4 ] = {
                { sx - hs, cy - hs }, { sx + hs, cy - hs },
                { sx + hs, cy + hs }, { sx - hs, cy + hs } };
            line_shape( sq, 4, thickness, align_mode[ align_idx ], show_guides );
            gui()->draw_text( sx - 22.0f, cy + hs + 14.0f, LINE_INK, "square" );

            /* circle -- a many-sided closed ring */
            f32 ox = r.x + r.w * 0.70f, rr = 48.0f;
            gui_vec2_t cir[ 48 ];
            for ( int i = 0; i < 48; ++i )
            {
                f32 a = ( 2.0f * PI ) * (f32)i / 48.0f;
                cir[ i ] = ( gui_vec2_t ){ ox + cosf( a ) * rr, cy + sinf( a ) * rr };
            }
            line_shape( cir, 48, thickness, align_mode[ align_idx ], show_guides );
            gui()->draw_text( ox - 20.0f, cy + rr + 14.0f, LINE_INK, "circle" );
        }

        /* ---- (e) mitered polyline corners, with an adjustable spread (uses Thickness) ---- */
        gui()->separator_text( "Polyline: mitered corners" );
        gui()->slider_float( "Corner spread", &spread, 0.0f, 1.0f );
        {
            gui_rect_t r = gui()->canvas( 130.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );
            f32 cy   = r.y + r.h * 0.5f;
            f32 amp  = ( r.h * 0.5f - 16.0f ) * spread;      /* spread closes / opens the V's */
            f32 x    = r.x + 24.0f;
            f32 step = ( r.w - 48.0f ) / 6.0f;
            gui_vec2_t zig[ 7 ];
            for ( int i = 0; i < 7; ++i )
                zig[ i ] = ( gui_vec2_t ){ x + step * (f32)i, cy + ( ( i & 1 ) ? -amp : amp ) };
            gui()->draw_polyline( zig, 7, thickness, GUI_STROKE_CENTER, false, LINE_CYAN );
        }

        /* ---- (f) closed path via the retained builder: polygon / star ---- */
        gui()->separator_text( "Closed path: path_stroke (polygon / star)" );
        gui()->checkbox( "Star", &star_mode ); gui()->same_line( 12.0f );
        gui()->drag_int( "Points", &poly_pts, 0.1f, 3, 10, "%d" );
        {
            gui_rect_t r = gui()->canvas( 200.0f );
            gui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );
            f32 pcx = r.x + r.w * 0.5f;
            f32 pcy = r.y + r.h * 0.5f;
            f32 pr  = ( r.h * 0.5f ) - 22.0f;

            gui()->path_clear();
            if ( star_mode )
            {
                /* alternate outer / inner radius for a star */
                int n = poly_pts * 2;
                for ( int i = 0; i < n; ++i )
                {
                    f32 a  = -PI * 0.5f + ( 2.0f * PI ) * (f32)i / (f32)n;
                    f32 rr = ( i & 1 ) ? pr * 0.45f : pr;
                    gui()->path_line_to( pcx + cosf( a ) * rr, pcy + sinf( a ) * rr );
                }
            }
            else
            {
                for ( int i = 0; i < poly_pts; ++i )
                {
                    f32 a = -PI * 0.5f + ( 2.0f * PI ) * (f32)i / (f32)poly_pts;
                    gui()->path_line_to( pcx + cosf( a ) * pr, pcy + sinf( a ) * pr );
                }
            }
            gui()->path_stroke( thickness, GUI_STROKE_CENTER, true, LINE_AMBR );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    15. Tables -- multi-column layout with per-cell clipping and a clickable header row.

    table_begin opens a table of ncols columns.  table_setup_column names and sizes each column
    before the first row: STRETCH columns fill the remaining space equally, FIXED columns take
    an explicit pixel width.  table_headers_row draws the non-scrolling header strip and, when
    GUI_TABLE_SORTABLE is set, registers sort clicks.  Each data row starts with table_next_row;
    table_next_column clips the draw list and hit-test rect to the current cell.
==============================================================================================*/

/* Row record for the sortable demo table.  File scope so demo_item_sort_value (the table's per-cell
   key accessor) can name the type when the table calls it back with the array as its user pointer. */
typedef struct { const char* name; const char* kind; float value; } demo_item_t;

/* table_sort_order value callback: hand the table the sort key for one cell.  Name and Type sort
   alphabetically (str); Value sorts numerically (num).  The table applies the direction itself. */
static void
demo_item_sort_value( i32 row, i32 col, gui_table_sort_value_t* out, void* user )
{
    const demo_item_t* items = (const demo_item_t*)user;
    if ( col == 0 )      out->str = items[ row ].name;
    else if ( col == 1 ) out->str = items[ row ].kind;
    else               { out->num = items[ row ].value; out->is_num = true; }
}

static void
demo_table( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 480, 480, GUI_COND_ONCE );
    if ( gui()->window_begin( "Tables", GUI_WIN_NONE ) )
    {
        gui()->stack();

        /* --- sortable three-column table -------------------------------------------------- */
        gui()->separator_text( "table_headers_row / GUI_TABLE_SORTABLE" );

        static const demo_item_t k_items[] = {
            { "pos_x",   "float",  1.234f   },
            { "pos_y",   "float",  -5.678f  },
            { "pos_z",   "float",  0.0f     },
            { "vel_x",   "float",  -0.5f    },
            { "vel_y",   "float",  2.0f     },
            { "health",  "int",    100.0f   },
            { "shield",  "int",    42.0f    },
            { "armor",   "int",    7.0f     },
            { "speed",   "float",  9.81f    },
            { "stamina", "float",  -3.5f    },
            { "alive",   "bool",   1.0f     },
            { "frozen",  "bool",   0.0f     },
            { "level",   "int",    12.0f    },
            { "score",   "int",    31337.0f },
        };
        const int k_item_count = (int)( sizeof( k_items ) / sizeof( k_items[ 0 ] ) );

        /* Display order, re-sorted in place whenever a header is clicked. */
        static int s_order[ 32 ];
        static bool s_order_init = false;
        if ( !s_order_init )
        {
            for ( int i = 0; i < k_item_count; ++i ) s_order[ i ] = i;
            s_order_init = true;
        }

        /* stretch Name, fixed Type (64 px), fixed Value (128 px); sortable + striped + framed,
           vertical scroll inside a fixed 160px body (header stays pinned).  RESIZABLE: drag the
           column boundaries (the highlight line appears under the cursor). */
        if ( gui()->table_begin( "props", 3,
                                   GUI_TABLE_SORTABLE | GUI_TABLE_ROW_STRIPES | GUI_TABLE_RESIZABLE |
                                   GUI_TABLE_BORDERS_V | GUI_TABLE_BORDERS_OUTER | GUI_TABLE_SCROLL_Y,
                                   160.0f ))
        {
            gui()->table_setup_column( "Name",  GUI_TABLE_COL_STRETCH,   0     );
            gui()->table_setup_column( "Type",  GUI_TABLE_COL_FIXED,     64.0f );
            gui()->table_setup_column( "Value", GUI_TABLE_COL_FIXED,    128.0f );
            gui()->table_headers_row();

            /* Built-in sort: hand the table a per-cell key accessor and it reorders s_order on a
               header click -- columns 0/1 sort alphabetically (str), column 2 numerically (num).
               No hand-written comparison loop or direction handling needed. */
            gui()->table_sort_order( s_order, k_item_count, demo_item_sort_value, NULL, (void*)k_items );

            for ( int r = 0; r < k_item_count; ++r )
            {
                const int i = s_order[ r ];
                gui()->table_next_row( 0 );
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    gui()->text( k_items[ i ].name );
                }
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    gui()->text_disabled( k_items[ i ].kind );
                }
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    /* Tint the cell red when the value is negative (CELL bg override). */
                    if ( k_items[ i ].value < 0.0f )
                        gui()->table_set_bg_color( GUI_TABLE_BG_CELL,
                                                     GUI_COLOR( 0xC0, 0x30, 0x30, 0x60 ) );
                    char buf[ 24 ];
                    snprintf( buf, sizeof( buf ), "%.3g", k_items[ i ].value );
                    gui()->text( buf );
                }
            }
            gui()->table_end();
        }

        /* --- table containing interactive widgets ----------------------------------------- */
        gui()->spacing( 0 );
        gui()->separator_text( "interactive cells (no header)" );

        static float s_vals[ 4 ] = { 0.25f, 0.5f, 0.75f, 1.0f };
        static const char* k_labels[] = { "Alpha", "Beta", "Gamma", "Delta" };

        if ( gui()->table_begin( "sliders", 2, GUI_TABLE_BORDERS_H, 0.0f ) )
        {
            gui()->table_setup_column( "Label",  GUI_TABLE_COL_FIXED,  60.0f );
            gui()->table_setup_column( "Slider", GUI_TABLE_COL_STRETCH, 0    );
            /* No table_headers_row -- body opens automatically on the first table_next_row.
               GUI_TABLE_BORDERS_H draws a divider between each data row. */

            for ( int i = 0; i < 4; ++i )
            {
                gui()->table_next_row( 0 );
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    gui()->text( k_labels[ i ] );
                }
                if ( gui()->table_next_column() )
                {
                    gui()->stack();
                    gui()->push_id_int( i );
                    gui()->slider_float( "##v", &s_vals[ i ], 0.0f, 1.0f );
                    gui()->pop_id();
                }
            }
            gui()->table_end();
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    16. Docking -- tile + tab windows into a dockspace that fills the main viewport.

    dockspace_over_viewport( 0, ... ) is called at the TOP of the build every frame: it lays the dock
    tree out over the surface and draws + interacts its splitters.  The layout itself is built once
    (programmatically) with dock_split / dock_window -- the DockBuilder idiom of splitting the shrinking
    remainder.  The docked windows below then render into their nodes: no per-window title bar, a shared
    tab strip instead (drag the splitters to resize; click the Console / Assets tabs to switch).  The
    "Demos" picker and any other free window still float on top of the dockspace.
==============================================================================================*/

static void
demo_docking( void )
{
    /* Layout persistence (Phase 3): the Viewport panel's buttons only set these flags; the actual
       save/restore runs HERE, at the top of the build before the dockspace and any docked window --
       a safe point to free + rebuild the tree (dock_load) without touching a node mid-render.  A real
       app would write s_layout to a file on save and load it at startup; here it round-trips in RAM. */
    static char s_layout[ 2048 ];
    static bool s_have_layout    = false;
    static bool s_save_layout    = false;
    static bool s_restore_layout = false;
    if ( s_save_layout )
    {
        gui()->dock_save( 0, s_layout, sizeof( s_layout ) );
        s_have_layout = true;
        s_save_layout = false;
    }
    if ( s_restore_layout )
    {
        if ( s_have_layout ) gui()->dock_load( 0, s_layout );
        s_restore_layout = false;
    }

    /* Lay out + interact the dockspace every frame (must precede the docked windows' window_begin). */
    gui_dock_id_t root = gui()->dockspace_over_viewport( 0, GUI_DOCKSPACE_NONE );

    /* Build the tree once: left rail, right inspector, a bottom strip, central viewport. */
    static bool built = false;
    if ( !built && root != GUI_DOCK_NONE )
    {
        gui_dock_id_t left   = gui()->dock_split( root, GUI_DIR_LEFT,  0.22f, &root );
        gui_dock_id_t right  = gui()->dock_split( root, GUI_DIR_RIGHT, 0.28f, &root );
        gui_dock_id_t bottom = gui()->dock_split( root, GUI_DIR_DOWN,  0.30f, &root );
        gui()->dock_window( "Scene Tree", left   );
        gui()->dock_window( "Inspector",  right  );
        gui()->dock_window( "Console",    bottom );
        gui()->dock_window( "Assets",     bottom );   /* tab alongside Console */
        gui()->dock_window( "Viewport",   root   );   /* central remainder */
        built = true;
    }

    if ( gui()->window_begin( "Scene Tree", GUI_WIN_NONE ) )
    {
        gui()->stack();
        if ( gui()->tree_node( "World" ) )
        {
            gui()->text( "Camera" );
            gui()->text( "Sun Light" );
            if ( gui()->tree_node( "Props" ) )
            {
                gui()->text( "Crate" );
                gui()->text( "Barrel" );
                gui()->tree_pop();
            }
            gui()->tree_pop();
        }
    }
    gui()->window_end();

    if ( gui()->window_begin( "Inspector", GUI_WIN_NONE ) )
    {
        static char name[ 32 ] = "Crate";
        static f32  pos[ 3 ]   = { 0.0f, 1.0f, 0.0f };
        static bool visible    = true;
        gui()->stack();                     /* declare the layout mode before any widget */
        gui()->field_label_left( 80.0f );
        gui()->input_text  ( "Name",     name, sizeof( name ) );
        gui()->input_float3( "Position", pos, NULL );
        gui()->checkbox    ( "Visible",  &visible );
    }
    gui()->window_end();

    if ( gui()->window_begin( "Console", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "[info] engine started" );
        gui()->text( "[info] dock layout built" );
        gui()->text( "> _" );
    }
    gui()->window_end();

    if ( gui()->window_begin( "Assets", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->bullet_text( "models/crate.obj" );
        gui()->bullet_text( "textures/wood.png" );
        gui()->bullet_text( "shaders/lit.glsl" );
    }
    gui()->window_end();

    if ( gui()->window_begin( "Viewport", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Central viewport panel." );
        gui()->separator();
        gui()->text( "Drag the gutters between regions to resize." );
        gui()->text( "Click the Console / Assets tabs to switch." );
        gui()->text( "Drag a tab OUT to pop it into a floater." );
        gui()->text( "Drag the Palette window onto a pane to dock." );
        gui()->separator();
        gui()->text( "Rearrange, Save, rearrange more, then Restore:" );
        /* Two full-width stacked rows: a stack() button fills its column, so same_line would push the
           second button off the pane's right edge. */
        if ( gui()->button( "Save Layout" ) )    s_save_layout    = true;
        if ( gui()->button( "Restore Layout" ) ) s_restore_layout = true;
    }
    gui()->window_end();

    /* A FREE (undocked) window to exercise the Phase-2 drag-to-dock gesture: drag its title bar over
       any pane to see the 5-way overlay, then drop on center (tab in) or a side (split). */
    gui()->window_set_next_pos ( 980, 120, GUI_COND_ONCE );
    gui()->window_set_next_size( 220, 150, GUI_COND_ONCE );
    if ( gui()->window_begin( "Palette", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "I'm a floating window." );
        gui()->separator();
        gui()->text( "Drag my title bar over a" );
        gui()->text( "pane to dock me (center =" );
        gui()->text( "tab, sides = split)." );
    }
    gui()->window_end();
}

/*==============================================================================================
    17. Icons -- the runtime icon atlas.

    Three icons are rasterized once into a buffer and registered (register_icon -> handle); after
    that they draw as tinted quads in the same flush as text.  image() reserves a layout slot and
    fits the icon into it; draw_icon_in places an icon into a rect the caller already holds, here
    a dummy() slot paired with a text label -- the "icon + caption" row an editor list uses.
==============================================================================================*/

static void
demo_icons( void )
{
    static gui_icon_id_t ic_folder = GUI_ICON_NONE;
    static gui_icon_id_t ic_check  = GUI_ICON_NONE;
    static gui_icon_id_t ic_gear   = GUI_ICON_NONE;

    /* Register once.  Safe to call any frame -- the GPU upload is deferred to the next frame_begin. */
    if ( ic_folder == GUI_ICON_NONE )
    {
        static u8 buf[ 32 * 32 ];
        sb_make_folder( buf, 32 ); ic_folder = gui()->register_icon( "folder", 32, 32, buf );
        sb_make_check ( buf, 32 ); ic_check  = gui()->register_icon( "check",  32, 32, buf );
        sb_make_gear  ( buf, 32 ); ic_gear   = gui()->register_icon( "gear",   32, 32, buf );
    }

    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 380, 360, GUI_COND_ONCE );
    if ( gui()->window_begin( "Icons", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Runtime icon atlas: register_icon ->" );
        gui()->text( "tinted quads, same flush as text." );
        gui()->separator();

        /* A row of the three icons, each tinted differently (colors are ABGR: 0xAABBGGRR). */
        gui()->row_cols_n( 0, 3 );
        gui()->image( ic_folder, 48, 48, 0xFF66CCFFu );   /* amber  */
        gui()->image( ic_check,  48, 48, 0xFF66DD66u );   /* green  */
        gui()->image( ic_gear,   48, 48, 0xFFDDDDDDu );   /* grey   */
        gui()->row( 0 );                                  /* back to a single column */

        gui()->separator();

        /* icon + caption rows -- draw_icon_in into a manual slot beside an aligned label. */
        static const struct { gui_icon_id_t* id; const char* label; } rows[] = {
            { &ic_folder, "Open Folder" },
            { &ic_gear,   "Settings"    },
            { &ic_check,  "Apply"       },
        };
        for ( int i = 0; i < 3; ++i )
        {
            gui_rect_t slot = gui()->dummy( 240, 28 );
            gui()->draw_icon_in( ( gui_rect_t ){ slot.x + 2, slot.y + 2, 24, 24 }, *rows[ i ].id, 0xFFFFFFFFu );
            gui()->draw_text_in( ( gui_rect_t ){ slot.x + 34, slot.y, slot.w - 34, slot.h },
                                   GUI_ALIGN_VCENTER, 0xFFFFFFFFu, rows[ i ].label );
        }
    }
    gui()->window_end();
}

/*==============================================================================================
    18. Symbols -- the Render* primitive family (normal pipeline, not the icon atlas).

    Two halves.  Top: the widget *style tags* -- per-emit enum switches pushed via push_style_var
    (GUI_VAR_CHECK_STYLE / _BULLET_STYLE / _ARROW_STYLE / _SEPARATOR_STYLE / _PROGRESS_STYLE /
    _SLIDER_KNOB), so the same call (checkbox, arrow_button, separator, progress_bar, slider) re-shapes
    to the selected style; pushing scopes it to just the sample block, set_*_style would set it
    globally.  Bottom: each raw render_* primitive drawn into a dummy() slot, the pieces editor /
    custom widgets reuse.
==============================================================================================*/

/* Center a sz x sz preview box inside the cell `c` (clamped to the cell) -- the placement for each
   parametric symbol preview in demo_symbols. */
static gui_rect_t
sym_box( gui_rect_t c, f32 sz )
{
    if ( sz > c.h ) sz = c.h;
    if ( sz > c.w ) sz = c.w;
    return ( gui_rect_t ){ c.x + ( c.w - sz ) * 0.5f, c.y + ( c.h - sz ) * 0.5f, sz, sz };
}

static void
demo_symbols( void )
{
    gui()->window_set_next_pos ( 60, 60, GUI_COND_ONCE );
    gui()->window_set_next_size( 430, 620, GUI_COND_ONCE );
    if ( gui()->window_begin( "Symbols", GUI_WIN_NONE ) )
    {
        gui()->stack();
        gui()->text( "Style tags force a shape on the widget emitting." );

        gui()->separator_text( "Style tags (scoped locally)" );

        static i32  check_idx  = 0;     /* 0 tick / 1 disc / 2 cross */
        static bool square_bull = false;
        static bool chevron     = false;
        static bool dashed      = false;
        static bool gradient    = false;
        static bool circle_knob = false;

        gui()->radio_button( "Tick",  &check_idx, 0 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Disc",  &check_idx, 1 ); gui()->same_line( -1.0f );
        gui()->radio_button( "Cross", &check_idx, 2 );
        gui()->checkbox( "Square bullets",    &square_bull );
        gui()->checkbox( "Chevron arrows",    &chevron );
        gui()->checkbox( "Dashed separators", &dashed );
        gui()->checkbox( "Gradient progress", &gradient );
        gui()->checkbox( "Circle slider knob", &circle_knob );

        /* push/pop scopes the tags to the sample widgets below; the set_*_style setters (check /
           bullet / arrow) or push of the same var elsewhere would apply them more broadly. */
        gui()->push_style_var( GUI_VAR_CHECK_STYLE,     (f32)check_idx );
        gui()->push_style_var( GUI_VAR_BULLET_STYLE,    square_bull ? 1.0f : 0.0f );
        gui()->push_style_var( GUI_VAR_ARROW_STYLE,     chevron     ? 1.0f : 0.0f );
        gui()->push_style_var( GUI_VAR_SEPARATOR_STYLE, dashed      ? 1.0f : 0.0f );
        gui()->push_style_var( GUI_VAR_PROGRESS_STYLE,  gradient    ? 1.0f : 0.0f );
        gui()->push_style_var( GUI_VAR_SLIDER_KNOB,     circle_knob ? 1.0f : 0.0f );

        gui()->text( "Sample widgets honoring the tags:" );

        static bool a = true, b = false, c = true;
        gui()->checkbox( "Enabled", &a ); gui()->same_line( 12.0f );
        gui()->checkbox( "Visible", &b ); gui()->same_line( 12.0f );
        gui()->checkbox( "Locked",  &c );

        gui()->push_item_flag( GUI_ITEM_BUTTON_REPEAT, true );
        gui()->arrow_button( "##l", GUI_DIR_LEFT  ); gui()->same_line( -1.0f );
        gui()->arrow_button( "##r", GUI_DIR_RIGHT ); gui()->same_line( 12.0f );
        gui()->arrow_button( "##u", GUI_DIR_UP    ); gui()->same_line( -1.0f );
        gui()->arrow_button( "##d", GUI_DIR_DOWN  );
        gui()->pop_item_flag();

        gui()->separator();                                  /* solid or dashed per the tag */

        static f32 sval = 0.5f;
        gui()->slider_float( "Level", &sval, 0.0f, 1.0f );   /* bar or circle knob per the tag */
        gui()->progress_bar( 0.66f, NULL );                  /* solid or gradient fill per the tag */
        gui()->bullet_text( "first bullet item" );

        gui()->pop_style_var( 6 );

        gui()->separator_text( "Parametric primitives (slider drives each)" );

        /* A stacked 50/50 list: each row previews one render_* primitive on the left (canvas cell)
           and a slider on the right driving one facet of it.  The preview reads last frame's slider
           value (immediate mode) -- a 1-frame latency that is imperceptible while dragging. */
        const u32 col = 0xFFE0E0E0u;
        const u32 acc = 0xFF60C0F0u;
        const u32 grn = 0xFF48E618u;             /* the strengthened check-mark green */
        const f32 H   = 26.0f;
        f32       t   = (f32)gui()->get_time();

        gui()->row2( 1.0f, 1.0f );
        {
            gui_rect_t r;

            /* Most facets are pixel sizes, stroke weights, counts, or whole-degree sweeps -- integer
               sliders, since fractional values only stair-step (the prims floor/snap anyway).  The two
               genuinely continuous facets (sub-1 curve bow, 0..1 progress fraction) stay float. */

            static i32 p_arrow = 18;     /* px */
            r = gui()->canvas( H ); gui()->draw_arrow( sym_box( r, (f32)p_arrow ), GUI_DIR_RIGHT, col );
            gui()->slider_int( "Arrow size (px)", &p_arrow, 8, 26 );

            static i32 p_check = 22;     /* px */
            r = gui()->canvas( H ); gui()->draw_check_mark( sym_box( r, (f32)p_check ), grn );
            gui()->slider_int( "Check size (px)", &p_check, 8, 26 );

            static i32 p_chev = 2;       /* stroke weight, px */
            r = gui()->canvas( H ); gui()->draw_chevron( sym_box( r, H ), GUI_DIR_RIGHT, (f32)p_chev, col );
            gui()->slider_int( "Chevron weight", &p_chev, 1, 5 );

            static i32 p_pm = 2;         /* stroke weight, px */
            r = gui()->canvas( H ); gui()->draw_plus_minus( sym_box( r, H ), true, (f32)p_pm, col );
            gui()->slider_int( "Plus weight", &p_pm, 1, 5 );

            static i32 p_sides = 6;
            r = gui()->canvas( H ); gui()->draw_ngon( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, (u32)p_sides, t*0.3f, true, 0.0f, acc );
            gui()->slider_int( "Polygon sides", &p_sides, 3, 12 );

            static i32 p_ring = 2;       /* stroke weight, px */
            r = gui()->canvas( H ); gui()->draw_circle( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, false, (f32)p_ring, acc );
            gui()->slider_int( "Ring weight", &p_ring, 1, 6 );

            static i32 p_arc = 240;      /* whole degrees -> gui_radians at the call */
            r = gui()->canvas( H ); gui()->draw_arc( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, 0.0f, gui_radians( (f32)p_arc ), 3.0f, acc );
            gui()->slider_int( "Arc sweep (deg)", &p_arc, 20, 360 );

            static i32 p_pie = 150;      /* whole degrees, swept from -90 (12 o'clock) */
            r = gui()->canvas( H ); gui()->draw_pie( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, gui_radians( -90.0f ), gui_radians( -90.0f + (f32)p_pie ), acc );
            gui()->slider_int( "Pie sweep (deg)", &p_pie, 20, 360 );

            static i32 p_round = 8;      /* corner radius, px */
            r = gui()->canvas( H ); gui()->draw_round_rect( sym_box( r, H ), (f32)p_round, (f32)p_round, 0.0f, 0.0f, true, 0.0f, 0xFF4A90D0u );
            gui()->slider_int( "Tab corner (px)", &p_round, 0, 13 );

            static f32 p_bow = 0.4f;     /* continuous curve shape -- stays float */
            r = gui()->canvas( H ); gui()->draw_bezier_quad( r.x+4, r.y+H*0.5f, r.x+r.w*0.5f, r.y+H*0.5f - H*p_bow, r.x+r.w-4, r.y+H*0.5f, 2.0f, acc );
            gui()->slider_float( "Curve bow", &p_bow, -0.45f, 0.45f );

            static i32 p_dash = 5;       /* dash length, px */
            r = gui()->canvas( H ); gui()->draw_dashed_line( r.x+4, r.y+H*0.5f, r.x+r.w-4, r.y+H*0.5f, (f32)p_dash, 3.0f, 2.0f, col );
            gui()->slider_int( "Dash length (px)", &p_dash, 2, 12 );

            static i32 p_cell = 6;       /* cell size, px */
            r = gui()->canvas( H ); gui()->draw_checker( sym_box( r, H ), (f32)p_cell, 0xFF808080u, 0xFF404040u );
            gui()->slider_int( "Checker cell (px)", &p_cell, 3, 14 );

            static i32 p_hatch = 5;      /* line spacing, px */
            r = gui()->canvas( H ); gui()->draw_hatch( sym_box( r, H ), (f32)p_hatch, 1.0f, 0xFF909090u );
            gui()->slider_int( "Hatch spacing (px)", &p_hatch, 3, 14 );

            static i32 p_spin = 3;       /* stroke weight, px */
            r = gui()->canvas( H ); gui()->draw_spinner( sym_box( r, H ), t, (f32)p_spin, acc );
            gui()->slider_int( "Spinner weight", &p_spin, 1, 6 );

            static f32 p_prog = 0.66f;   /* continuous 0..1 fraction -- stays float */
            r = gui()->canvas( H ); gui()->draw_progress_arc( r.x + r.w*0.5f, r.y + H*0.5f, H*0.4f, p_prog, 3.0f, acc );
            gui()->slider_float( "Progress frac", &p_prog, 0.0f, 1.0f );
        }
        gui()->row( 0 );

        gui()->separator_text( "Text effects" );
        gui_rect_t tr = gui()->dummy( 0.0f, 20.0f );
        gui()->draw_text_outline( tr.x + 4.0f, tr.y + 4.0f, "Outlined text", 0xFFFFFFFFu, 0xFF000000u );
    }
    gui()->window_end();
}

/*==============================================================================================
    Demo table -- the menu the host steps through.
==============================================================================================*/

const sb_gui_demo_t sb_gui_demos[] =
{
    { "Widgets",      "text / button / checkbox / slider / input_text", demo_widgets     },
    { "Text",         "textf / bullet / separator / collapsing_header", demo_text        },
    { "Layout Rows",  "row / row_cols / row_cols_n / row2..4",           demo_layout_rows },
    { "Field Forms",  "field_label_left/right / field_split",           demo_fields      },
    { "Grid",         "grid_cells / skip",                              demo_grid        },
    { "Child / List", "child_begin / selectable / push_id",             demo_child_list  },
    { "Combo / List", "combo / listbox / combo_begin / listbox_begin",  demo_combo_list  },
    { "Align",        "align / same_line / spacing / separator",        demo_align       },
    { "Sub-layout",   "push_layout / pop_layout",                       demo_sublayout   },
    { "Pack / Bar",   "bar / pack_size / pack_nextline",                demo_pack        },
    { "Windows",      "multiple windows / flags / z-order",             demo_windows     },
    { "Auto-size",    "ALWAYS_AUTOSIZE / CAN_AUTOSIZE / auto child",    demo_autosize    },
    { "Menus",        "menu bar / menu_begin / menu_item / context",    demo_menus       },
    { "Lines / Paths","draw_line / draw_polyline / path_stroke",        demo_lines       },
    { "Tables",       "table_begin / setup_column / next_row / next_column", demo_table   },
    { "Docking",      "dockspace_over_viewport / dock_split / tabs",     demo_docking     },
    { "Icons",        "register_icon / image / draw_icon_in",            demo_icons       },
    { "Symbols",      "style tags + render_* shape/curve/fill palette",  demo_symbols     },
    { NULL,           NULL,                                             NULL             },
};

int
sb_gui_demo_count( void )
{
    int n = 0;
    while ( sb_gui_demos[ n ].name )
        n++;
    return n;
}

/*==============================================================================================
    Picker overlay -- list every demo with the active one marked, plus the key hints.
==============================================================================================*/

int
sb_gui_demo_picker( int active )
{
    const int count    = sb_gui_demo_count();
    int       selected = active;                  /* unchanged unless a row is clicked */

    gui()->window_set_next_pos ( 940, 20, GUI_COND_ONCE );
    gui()->window_set_next_size( 320, 360, GUI_COND_ONCE );
    if ( gui()->window_begin( "Demos", GUI_WIN_NOCOLLAPSE ) )
    {
        gui()->stack();
        gui()->text( "Keys: 1-9 select  +/- step  ESC quit" );
        gui()->separator();

        for ( int i = 0; i < count; i++ )
        {
            gui()->push_id_int( i );
            bool on = ( i == active );
            char label[ 64 ];
            snprintf( label, sizeof( label ), "%d. %s", i + 1, sb_gui_demos[ i ].name );
            /* selectable returns true on the clicked frame -- report that row back to the
               host, which owns the real active index. */
            if ( gui()->selectable( label, &on ) )
                selected = i;
            gui()->pop_id();
        }

        gui()->separator();
        if ( active >= 0 && active < count )
        {
            gui()->textf( "%s", sb_gui_demos[ active ].name );
            gui()->text( sb_gui_demos[ active ].desc );
        }
    }
    gui()->window_end();

    return selected;
}

/*============================================================================================*/
// clang-format on
