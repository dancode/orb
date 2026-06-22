/*==============================================================================================

    sandbox/vulkan/sb_vulkan_imgui.c -- imgui feature demos.

    One function per feature group, each opening its own window.  Kept small and focused so the
    window on screen reads as a worked example of exactly one part of the imgui API; the host
    switches between them with a key.  See sb_vulkan_imgui.h for the contract.

    All persistent widget values are file-scope statics local to each demo function -- the demos
    are pure UI, no shared state, so a demo can be read top to bottom in isolation.

==============================================================================================*/

#include <stdio.h>
#include <string.h>    /* strcmp -- column sort comparison in demo_table */
#include <math.h>      /* cosf / sinf -- the diagonal fan + polygon in demo_lines */

#include "sb_vulkan_imgui.h"
#include "runtime_service/imgui/imgui_host.h"

// clang-format off 

/*==============================================================================================
    1. Widgets -- the basic interactive controls.

    text / button / checkbox / slider_float / input_text, each on its own auto-height row in a
    stack() (the single-column vertical list).  Every widget returns true on the frame it is
    activated or its value changes; here we just feed that back into a little state so it shows.
==============================================================================================*/

static void
demo_widgets( void )
{
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 360, 420, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Widgets", IMGUI_WIN_NONE ))
    {
        imgui()->stack();                                   /* declare the mode: a vertical list */
        imgui()->text( "Basic interactive widgets:" );
        imgui()->separator();

        static int clicks = 0;
        if ( imgui()->button( "Click me" ) )
            clicks++;
        imgui()->textf( "button pressed %d time(s)", clicks );

        imgui()->spacing( 0 );

        static bool checked = true;
        imgui()->checkbox( "Enable feature", &checked );
        imgui()->textf( "feature is %s", checked ? "ON" : "off" );

        imgui()->spacing( 0 );

        static f32 amount = 3.0f;
        imgui()->slider_float( "Amount", &amount, 0.0f, 10.0f );
        imgui()->textf( "amount = %.2f", amount );

        imgui()->spacing( 0 );

        static char name[ 32 ] = "orb";
        imgui()->input_text( "Name", name, sizeof( name ) );
        imgui()->textf( "hello, %s", name );
    }
    imgui()->end_window();
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
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 380, 460, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Text & Sections", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "Plain text line." );
        imgui()->textf( "Formatted: pi ~= %.4f, frame %d", 3.14159f, 42 );

        imgui()->separator_text( "A bullet list" );
        imgui()->bullet_text( "first item" );
        imgui()->bullet_text( "second item" );
        imgui()->bullet_text( "third item" );

        imgui()->separator();

        /* collapsing_header returns its open state; guard the body with it. */
        if ( imgui()->collapsing_header( "Details (click to fold)" ) )
        {
            imgui()->text( "These lines only draw while the" );
            imgui()->text( "header above is expanded." );
            imgui()->textf( "value: %d", 1234 );
        }

        if ( imgui()->collapsing_header( "More details" ) )
        {
            imgui()->bullet_text( "another folded section" );
            imgui()->bullet_text( "independent open state" );
        }
    }
    imgui()->end_window();
}

/*==============================================================================================
    3. Layout rows -- shaping the repeating row template.

    A region declares its mode with a header: stack() is the single flex column, while row_cols /
    row2..4 / row_track install a multi-column flow template.  The template persists and repeats
    for every following widget until set again.  Sizes use one overloaded f32: > 1 pixels, 1.0
    fill (equal share of the leftover), (0,1) a fraction, 0 natural.
==============================================================================================*/

static void
demo_layout_rows( void )
{
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 440, 420, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Layout Rows", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();                         /* heading lines sit in a plain stack */
        imgui()->text( "row_cols( 0, 3 ) -- three equal columns:" );
        imgui()->row_cols( 0, 3 );
        imgui()->button( "A" );
        imgui()->button( "B" );
        imgui()->button( "C" );
        imgui()->row( 0 );                        /* back to a single column */

        imgui()->text( "row2( 0.3, 0.7 ) -- weighted 30/70:" );
        imgui()->row2( 0.3f, 0.7f );
        imgui()->button( "30%" );
        imgui()->button( "70%" );
        imgui()->row( 0 );

        imgui()->text( "row_track -- 120px + fill + 80px:" );
        imgui()->row_track( 0, ( f32[] ){ 120, 1, 80, IMGUI_END } );
        imgui()->button( "fixed 120" );
        imgui()->button( "fill" );
        imgui()->button( "80" );
        imgui()->row( 0 );

        imgui()->text( "row( 48 ) -- one tall 48px row:" );
        imgui()->row( 48 );
        imgui()->button( "tall button" );
        imgui()->row( 0 );
    }
    imgui()->end_window();
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

    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 400, 420, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Field Forms", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        /* Each section reuses the "Name"/"Enabled" labels, so scope them with push_id to keep
           the widget ids distinct (ids are seeded by label within a region). */
        imgui()->text( "field_label_left( 90 ) -- labels in a 90px gutter:" );
        imgui()->push_id( "left" );
        imgui()->field_label_left( 90.0f );
        imgui()->input_text  ( "Name",    f_name, sizeof( f_name ) );
        imgui()->slider_float( "Speed",   &f_speed, 0.0f, 10.0f );
        imgui()->checkbox    ( "Enabled", &f_enabled );
        imgui()->field_label_left( 0.0f );        /* clear the split */
        imgui()->pop_id();

        imgui()->spacing( 0 );
        imgui()->text( "field_label_right( 90 ) -- labels on the right:" );
        imgui()->push_id( "right" );
        imgui()->field_label_right( 90.0f );
        imgui()->input_text  ( "Name",    f_name, sizeof( f_name ) );
        imgui()->checkbox    ( "Enabled", &f_enabled );
        imgui()->field_label_right( 0.0f );
        imgui()->pop_id();

        imgui()->spacing( 0 );
        imgui()->text( "field_split( LEFT, 0.4, 0.6 ) -- fractional:" );
        imgui()->field_split( IMGUI_LABEL_LEFT, 0.4f, 0.6f );
        imgui()->slider_float( "Volume", &f_volume, 0.0f, 10.0f );
        imgui()->field_label_left( 0.0f );
    }
    imgui()->end_window();
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
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 420, 440, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Grid", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "grid_cells( 3, 3 ) -- 3x3, row-major:" );

        /* The grid fills the remaining content box, so put it in a fixed-height child
           and leave a note below it. */
        if ( imgui()->begin_child( "grid_box", 0, 300, IMGUI_WIN_NOSCROLL ) )
        {
            imgui()->grid_cells( 3, 3 );
            for ( int i = 0; i < 9; i++ )
            {
                if ( i == 4 )
                {
                    imgui()->skip();              /* leave the center cell empty */
                    continue;
                }
                char label[ 8 ];
                snprintf( label, sizeof( label ), "%d", i );
                imgui()->button( label );
            }
        }
        imgui()->end_child();

        imgui()->text( "center cell skipped with skip()" );
    }
    imgui()->end_window();
}

/*==============================================================================================
    6. Child region / list box -- an independently scrolled, clipped sub-box.

    begin_child carves a box of height h that scrolls and clips on its own; fill it with
    selectable rows to make a list box.  selectable toggles *on and returns true on the clicked
    frame, so the caller can manage single-selection by index.  push_id_int keeps row ids distinct.
==============================================================================================*/

static void
demo_child_list( void )
{
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 360, 440, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Child / List Box", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "List box (scrolls independently):" );

        static int sel = -1;
        if ( imgui()->begin_child( "rows", 0, 240, IMGUI_WIN_NONE ) )
        {
            imgui()->stack();                     /* the child is its own region -- declare its mode */
            for ( int i = 0; i < 40; i++ )
            {
                imgui()->push_id_int( i );        /* distinct id even though labels repeat */
                char row[ 32 ];
                snprintf( row, sizeof( row ), "row item %02d", i );
                bool on = ( sel == i );
                if ( imgui()->selectable( row, &on ) )
                    sel = on ? i : -1;            /* single-selection */
                imgui()->pop_id();
            }
        }
        imgui()->end_child();

        imgui()->textf( "selected: %d (this line is below the box)", sel );

        /* Vertically resizeable child: drag the bottom border to change its height.  The size is
           user-owned and persisted, so the rows below it move as the box grows / shrinks. */
        imgui()->help_marker( "Drag the bottom border to resize this box vertically." );
        if ( imgui()->begin_child( "resizeable", 0, 120, IMGUI_WIN_CHILD_RESIZE_Y ) )
        {
            imgui()->stack();
            for ( int i = 0; i < 12; i++ )
                imgui()->textf( "resizeable line %02d", i );
        }
        imgui()->end_child();

        imgui()->text( "this line sits below the resizeable box" );

        /* Auto-resize with a height cap: the box hugs its content (AutoResizeY, h <= 0) but
           set_next_window_size_constraints caps it at max_lines rows -- beyond that it stops
           growing and scrolls.  The two sliders drive the emitted line count and the cap so the
           transition from hugging to scrolling is visible. */
        imgui()->separator_text( "Auto-resize with constraints" );

        static i32 draw_lines = 3;
        static i32 max_lines  = 10;
        imgui()->drag_int( "Lines Count",     &draw_lines, 0.2f, 0,  30, "%d" );
        imgui()->drag_int( "Max (in lines)",  &max_lines,  0.2f, 1,  20, "%d" );

        f32 line = imgui()->line_h() + imgui()->h_min();   /* one row plus its standard margin */
        imgui()->set_next_window_size_constraints( 0.0f, line, 0.0f, line * (f32)max_lines );
        if ( imgui()->begin_child( "constrained", 0, 0, IMGUI_WIN_NONE ) )
        {
            imgui()->stack();
            for ( i32 n = 0; n < draw_lines; n++ )
                imgui()->textf( "Line %04d", n );
        }
        imgui()->end_child();
    }
    imgui()->end_window();
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
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 380, 460, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Combo / List Box", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();

        static const char* items[] = { "AAAA", "BBBB", "CCCC", "DDDD", "EEEE", "FFFF",
                                       "GGGG", "HHHH", "IIIIIII", "JJJJ", "KKKKKKK" };
        const i32          n_items  = (i32)( sizeof( items ) / sizeof( items[ 0 ] ) );

        /* One-liner combo over the array -- the everyday case. */
        imgui()->separator_text( "Combo (one-liner)" );
        static i32 combo_idx = 0;
        imgui()->combo( "combo", &combo_idx, items, n_items );
        imgui()->same_line( -1.0f );
        imgui()->help_marker( "combo() over an array of strings; the dropdown drops below the box." );

        /* Dropdown height cap (imgui_combo_flags_t HEIGHT_*): mutually exclusive, so pick with a
           radio group.  Applied to the BeginCombo dropdown below. */
        imgui()->separator_text( "BeginCombo (full control)" );
        static i32 height_sel = 1;   /* 0 small, 1 regular, 2 large, 3 largest */
        imgui()->text( "Popup height:" );
        imgui()->radio_button( "Small",   &height_sel, 0 ); imgui()->same_line( -1.0f );
        imgui()->radio_button( "Regular", &height_sel, 1 ); imgui()->same_line( -1.0f );
        imgui()->radio_button( "Large",   &height_sel, 2 ); imgui()->same_line( -1.0f );
        imgui()->radio_button( "Largest", &height_sel, 3 );

        const imgui_combo_flags_t height_flags[] = {
            IMGUI_COMBO_HEIGHT_SMALL, IMGUI_COMBO_HEIGHT_REGULAR,
            IMGUI_COMBO_HEIGHT_LARGE, IMGUI_COMBO_HEIGHT_LARGEST,
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
        if ( imgui()->begin_combo( "combo 2", preview, height_flags[ height_sel ] ) )
        {
            for ( i32 i = 0; i < n_many; i++ )
            {
                imgui()->push_id_int( i );
                bool is_sel = ( sel_idx == i );
                if ( imgui()->selectable( many[ i ], &is_sel ) )
                    sel_idx = i;
                imgui()->pop_id();
            }
            imgui()->end_combo();
        }

        /* One-liner list box, height in items. */
        imgui()->separator_text( "List box (one-liner)" );
        static const char* fruit[] = { "Apple", "Banana", "Cherry", "Kiwi", "Mango",
                                       "Orange", "Pineapple", "Strawberry", "Watermelon" };
        const i32          n_fruit = (i32)( sizeof( fruit ) / sizeof( fruit[ 0 ] ) );
        static i32         fruit_idx = 1;
        imgui()->listbox( "listbox", &fruit_idx, fruit, n_fruit, 4 );

        /* Generic begin/end list box -- emit the rows yourself (default size). */
        imgui()->separator_text( "BeginListBox (full control)" );
        static i32 row_idx = -1;
        if ( imgui()->begin_listbox( "rows", 0.0f, 0.0f ) )
        {
            for ( i32 i = 0; i < 24; i++ )
            {
                imgui()->push_id_int( i );
                char row[ 32 ];
                snprintf( row, sizeof( row ), "list row %02d", i );
                bool on = ( row_idx == i );
                if ( imgui()->selectable( row, &on ) )
                    row_idx = on ? i : -1;
                imgui()->pop_id();
            }
            imgui()->end_listbox();
        }

        imgui()->textf( "combo=%s  combo2=%s  fruit=%s  row=%d",
                        items[ combo_idx ], many[ sel_idx ], fruit[ fruit_idx ], row_idx );
    }
    imgui()->end_window();
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
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 380, 400, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Align & Spacing", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "align() across two columns:" );

        imgui()->row2( 0.5f, 0.5f );
        imgui()->align( IMGUI_ALIGN_LEFT );
        imgui()->text( "left" );
        imgui()->align( IMGUI_ALIGN_RIGHT );
        imgui()->text( "right" );
        imgui()->align( IMGUI_ALIGN_LEFT );       /* restore */
        imgui()->row( 0 );

        imgui()->separator();

        imgui()->text( "same_line keeps widgets on one line:" );
        imgui()->button( "OK" );
        imgui()->same_line( 8.0f );
        imgui()->button( "Cancel" );
        imgui()->same_line( 8.0f );
        imgui()->button( "Apply" );

        imgui()->separator_text( "spacing()" );
        imgui()->text( "above a 32px gap" );
        imgui()->spacing( 32.0f );
        imgui()->text( "below the gap" );
    }
    imgui()->end_window();
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
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 400, 360, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Sub-layout", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "row_cols( 0, 2 ): col 0 is a sub-layout" );
        imgui()->separator();

        imgui()->row_cols( 0, 2 );

        /* Column 0: open a sub-layout and stack three buttons inside the single cell. */
        imgui()->push_layout();
            imgui()->stack();                     /* the sub-layout is a region too -- declare its mode */
            imgui()->text( "stacked:" );
            imgui()->button( "one" );
            imgui()->button( "two" );
            imgui()->button( "three" );
        imgui()->pop_layout();

        /* Column 1: a single widget filling the other cell. */
        imgui()->text( "single cell" );

        imgui()->row( 0 );
    }
    imgui()->end_window();
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
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 440, 320, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Pack / Bar", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "bar() -- buttons at natural width, then a fill search box:" );

        imgui()->bar();
        imgui()->button( "New" );
        imgui()->button( "Open" );
        imgui()->button( "Save" );
        static char find[ 32 ] = "";
        imgui()->pack_size( 1.0f );                   /* the next item fills the rest of the line */
        imgui()->input_text( "##find", find, sizeof( find ) );

        imgui()->stack();
        imgui()->spacing( 0 );
        imgui()->text( "pack_nextline() wraps the run -- three per line:" );

        imgui()->bar();
        for ( int i = 0; i < 9; i++ )
        {
            imgui()->push_id_int( i );
            char b[ 12 ];
            snprintf( b, sizeof( b ), "btn %d", i );
            imgui()->button( b );
            if ( i % 3 == 2 ) imgui()->pack_nextline();
            imgui()->pop_id();
        }
    }
    imgui()->end_window();
}

/*==============================================================================================
    9. Windows -- multiple panels, flags, and z-order.

    Three overlapping windows show the per-window flags (a default window, a no-title-bar panel,
    a fixed/no-resize panel) and z-order: click any window to bring it to the front, drag to move.
==============================================================================================*/

static void
demo_windows( void )
{
    imgui()->set_next_window_pos ( 80, 80, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 280, 200, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Default Window", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "Default flags." );
        imgui()->text( "Title bar, collapse, resize." );
        imgui()->text( "Click another window to raise it." );
    }
    imgui()->end_window();

    imgui()->set_next_window_pos ( 220, 180, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 280, 180, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "No Title Bar", IMGUI_WIN_NOTITLEBAR ) )
    {
        imgui()->stack();
        imgui()->text( "IMGUI_WIN_NOTITLEBAR" );
        static bool t = false;
        imgui()->checkbox( "a toggle", &t );
    }
    imgui()->end_window();

    imgui()->set_next_window_pos ( 360, 280, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 260, 160, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Fixed", IMGUI_WIN_NORESIZE | IMGUI_WIN_NOMOVE ) )
    {
        imgui()->stack();
        imgui()->text( "IMGUI_WIN_NORESIZE | NOMOVE" );
        imgui()->text( "cannot be moved or resized." );
    }
    imgui()->end_window();
}

/*==============================================================================================
    10. Auto-size -- windows and children that hug their content.

    IMGUI_WIN_ALWAYS_AUTOSIZE makes a window recompute its size from its content every frame (no
    user resize, no scrollbars); add/remove rows and the window grows and shrinks to fit.
    IMGUI_WIN_CAN_AUTOSIZE keeps a normal resizeable window but adds a corner size-grip you
    double-click to snap it to its content.  A begin_child with h <= 0 auto-sizes its height the
    same way, and content_avail() reports the space left in the current region.
==============================================================================================*/

static void
demo_autosize( void )
{
    /* (a) A window that always hugs its content -- the row count drives its height. */
    imgui()->set_next_window_pos( 60, 60, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Always Auto-size", IMGUI_WIN_ALWAYS_AUTOSIZE ) )
    {
        static int rows = 3;

        imgui()->stack();
        imgui()->text( "ALWAYS_AUTOSIZE: window fits content." );
        imgui()->row2( 0.5f, 0.5f );
        if ( imgui()->button( "Add row" ) )    rows++;
        if ( imgui()->button( "Remove row" ) ) rows = rows > 0 ? rows - 1 : 0;
        imgui()->row( 0 );

        for ( int i = 0; i < rows; i++ )
            imgui()->textf( "content row %d", i );
    }
    imgui()->end_window();

    /* (b) A normal window with a corner grip; double-click it to fit. */
    imgui()->set_next_window_pos ( 360, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 300, 320, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Double-click grip", IMGUI_WIN_CAN_AUTOSIZE ) )
    {
        imgui()->stack();
        imgui()->text( "CAN_AUTOSIZE: drag the triangle" );
        imgui()->text( "grip in the corner to resize," );
        imgui()->text( "or double-click it to snap back" );
        imgui()->text( "to fit this content." );
    }
    imgui()->end_window();

    /* (c) Auto-height child (h <= 0) + content_avail(). */
    imgui()->set_next_window_pos ( 360, 420, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 320, 260, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Auto child", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "begin_child with h <= 0 hugs content:" );
        if ( imgui()->begin_child( "auto", 0, 0, IMGUI_WIN_NOSCROLL ) )
        {
            imgui()->stack();                     /* the child region declares its own mode */
            imgui()->text( "I am exactly as tall" );
            imgui()->text( "as my three lines." );
            imgui()->bullet_text( "no fixed height" );
        }
        imgui()->end_child();

        imgui_vec2_t avail = imgui()->content_avail();
        imgui()->textf( "content_avail below: %.0f x %.0f", avail.x, avail.y );
    }
    imgui()->end_window();
}

/*==============================================================================================
    11. Menus -- menu bars, menu entries, menu items, submenus, and context menus.

    begin_menu_bar fills the strip a window reserves with IMGUI_WIN_MENUBAR; begin_menu opens a
    submenu popup (horizontal in the bar, a row with an arrow inside a menu -- it nests); menu_item
    is a leaf command (optional "shortcut" text on the right, optional bool* for a checkable item).
    A menu_item click dismisses the whole menu chain.  Disabled items reuse the item-flag stack.
    The same menu_item / begin_menu work inside any popup, so a right-click context menu is built
    the same way, and begin_main_menu_bar pins a bar across the top of the display.
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
    if ( imgui()->menu_item( "New",  NULL,     NULL ) ) s_menu_last = "File / New";
    if ( imgui()->menu_item( "Open", "Ctrl+O", NULL ) ) s_menu_last = "File / Open";

    /* A submenu: begin_menu inside a menu renders a row with a right arrow and opens to the side. */
    if ( imgui()->begin_menu( "Open Recent" ) )
    {
        if ( imgui()->menu_item( "scene.orb",  NULL, NULL ) ) s_menu_last = "Recent / scene.orb";
        if ( imgui()->menu_item( "level_1.orb", NULL, NULL ) ) s_menu_last = "Recent / level_1.orb";
        if ( imgui()->begin_menu( "More.." ) )            /* nested submenu */
        {
            if ( imgui()->menu_item( "old.orb",   NULL, NULL ) ) s_menu_last = "Recent / old.orb";
            if ( imgui()->menu_item( "older.orb", NULL, NULL ) ) s_menu_last = "Recent / older.orb";
            imgui()->end_menu();
        }
        imgui()->end_menu();
    }

    if ( imgui()->menu_item( "Save",    "Ctrl+S", NULL ) ) s_menu_last = "File / Save";
    if ( imgui()->menu_item( "Save As..", NULL,   NULL ) ) s_menu_last = "File / Save As";
    imgui()->separator();
    if ( imgui()->menu_item( "Quit", "Alt+F4", NULL ) ) s_menu_last = "File / Quit";
}

static void
demo_menus( void )
{
    /* (a) Optional full-width bar pinned to the top of the display.  Toggled from the body below;
       drawn first (immediate mode), so the toggle takes effect the next frame. */
    if ( s_menu_mainbar && imgui()->begin_main_menu_bar() )
    {
        if ( imgui()->begin_menu( "App" ) )
        {
            if ( imgui()->menu_item( "About", NULL,     NULL ) ) s_menu_last = "App / About";
            if ( imgui()->menu_item( "Quit",  "Alt+F4", NULL ) ) s_menu_last = "App / Quit";
            imgui()->end_menu();
        }
        if ( imgui()->begin_menu( "Help" ) )
        {
            if ( imgui()->menu_item( "Documentation", "F1", NULL ) ) s_menu_last = "Help / Docs";
            imgui()->end_menu();
        }
        imgui()->end_main_menu_bar();
    }

    /* (b) A window with its own menu bar (IMGUI_WIN_MENUBAR reserves the strip). */
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 420, 320, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Menus", IMGUI_WIN_MENUBAR ) )
    {
        if ( imgui()->begin_menu_bar() )
        {
            if ( imgui()->begin_menu( "File" ) )
            {
                demo_menu_file();
                imgui()->end_menu();
            }
            if ( imgui()->begin_menu( "Edit" ) )
            {
                if ( imgui()->menu_item( "Undo", "Ctrl+Z", NULL ) ) s_menu_last = "Edit / Undo";

                /* A disabled item -- reuse the item-flag stack (no per-widget enabled param). */
                imgui()->push_item_flag( IMGUI_ITEM_DISABLED, true );
                imgui()->menu_item( "Redo", "Ctrl+Y", NULL );
                imgui()->pop_item_flag();

                imgui()->separator();
                if ( imgui()->menu_item( "Cut",   "Ctrl+X", NULL ) ) s_menu_last = "Edit / Cut";
                if ( imgui()->menu_item( "Copy",  "Ctrl+C", NULL ) ) s_menu_last = "Edit / Copy";
                if ( imgui()->menu_item( "Paste", "Ctrl+V", NULL ) ) s_menu_last = "Edit / Paste";
                imgui()->end_menu();
            }
            if ( imgui()->begin_menu( "View" ) )
            {
                /* Checkable items: pass a bool* -- the tick reflects + toggles the flag. */
                imgui()->menu_item( "Show grid",  NULL, &s_menu_grid );
                imgui()->menu_item( "Show stats", NULL, &s_menu_stats );
                imgui()->end_menu();
            }
            imgui()->end_menu_bar();
        }

        imgui()->stack();
        imgui()->text( "Use the menu bar above." );
        imgui()->textf( "Last command: %s", s_menu_last );
        imgui()->textf( "grid: %s   stats: %s",
                        s_menu_grid ? "ON" : "off", s_menu_stats ? "ON" : "off" );
        imgui()->separator();
        imgui()->checkbox( "Show main menu bar (top of screen)", &s_menu_mainbar );
        imgui()->spacing( 0 );
        imgui()->text( "Right-click empty space for a context menu." );

        /* (c) A context menu: begin_popup_context_window opens on a right-click in empty window
           space, and is filled with the same menu_item / begin_menu calls. */
        if ( imgui()->begin_popup_context_window( "ctx" ) )
        {
            imgui()->stack();                 /* a popup body declares its layout mode */
            if ( imgui()->begin_menu( "File" ) )
            {
                demo_menu_file();
                imgui()->end_menu();
            }
            imgui()->separator();
            if ( imgui()->menu_item( "Reset grid", NULL, NULL ) ) { s_menu_grid = true;  s_menu_last = "ctx / Reset grid"; }
            imgui()->menu_item( "Show stats", NULL, &s_menu_stats );
            imgui()->end_popup();
        }
    }
    imgui()->end_window();
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
#define LINE_INK    IMGUI_COLOR( 0xE6, 0xE6, 0xE6, 0xFF )   /* near-white stroke    */
#define LINE_CYAN   IMGUI_COLOR( 0x4F, 0xC3, 0xF7, 0xFF )   /* cool accent          */
#define LINE_AMBR   IMGUI_COLOR( 0xFF, 0xB0, 0x40, 0xFF )   /* warm accent (opaque) */
#define LINE_AMBR_T IMGUI_COLOR( 0xFF, 0xB0, 0x40, 0xC8 )   /* warm, translucent    */
#define LINE_PATH   IMGUI_COLOR( 0xFF, 0xFF, 0xFF, 0xFF )   /* the ideal path / guide */
#define LINE_BG     IMGUI_COLOR( 0x1E, 0x1E, 0x1E, 0xFF )   /* canvas backdrop      */

static const f32 PI = 3.14159265f;

/* Stroke a closed point ring: optionally the ideal path as a 1px white guide, then the thick
   selected stroke translucent on top -- so inside / center / outside reads against the guide. */
static void
line_shape( const imgui_vec2_t* pts, u32 n, f32 thickness, imgui_stroke_align_t align, bool guide )
{
    if ( guide )
        imgui()->draw_polyline( pts, n, 1.0f, IMGUI_STROKE_CENTER, true, LINE_PATH );
    imgui()->draw_polyline( pts, n, thickness, align, true, LINE_AMBR_T );
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
    static const imgui_stroke_align_t align_mode[ 4 ] = {
        IMGUI_STROKE_CENTER_BIASED, IMGUI_STROKE_CENTER, IMGUI_STROKE_INSIDE, IMGUI_STROKE_OUTSIDE };

    imgui()->set_next_window_pos ( 80, 40, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 480, 660, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Lines & Paths", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "draw_line / draw_polyline / path_stroke into a canvas()." );

        /* ---- controls ---- */
        imgui()->separator_text( "Parameters" );
        imgui()->slider_int( "Thickness", &thickness_px, 0, 16 );   /* 0 hides the stroke -> guide only */
        f32 thickness = (f32)thickness_px;                          /* used as f32 by the draw calls */
        imgui()->textf( "alignment: %s", align_names[ align_idx ] );

        imgui()->bar();                                  /* a horizontal run of radio buttons */
        imgui()->radio_button( "Biased",  &align_idx, 0 );
        imgui()->radio_button( "Center",  &align_idx, 1 );
        imgui()->radio_button( "Inside",  &align_idx, 2 );
        imgui()->radio_button( "Outside", &align_idx, 3 );
        imgui()->stack();                                /* back to the vertical list */

        imgui()->checkbox( "Show guide lines", &show_guides );

        /* A custom-drawn, interactive control built entirely from the primitives: dummy() reserves a
           cell, is_mouse_hovering_rect() drives the hover tint, draw_text_in() centers the caption,
           and invisible_button() makes the same rect clickable -- cycling the alignment on a click. */
        {
            imgui_rect_t sw  = imgui()->dummy( 0.0f, 22.0f );        /* full-width strip, 22px tall */
            bool         hot = imgui()->is_mouse_hovering_rect( sw );
            imgui()->draw_rect( sw.x, sw.y, sw.w, sw.h, hot ? LINE_CYAN : LINE_BG );
            imgui()->draw_text_in( sw, IMGUI_ALIGN_CENTER, LINE_INK, "click: cycle alignment" );
            if ( imgui()->invisible_button( "cycle##align", sw ) )
                align_idx = ( align_idx + 1 ) % 4;
        }

        /* ---- (a) crisp axis-aligned ladder ---- */
        imgui()->separator_text( "Axis-aligned: crisp 1..6 px (pixel-snapped)" );
        {
            imgui_rect_t r = imgui()->canvas( 168.0f );
            imgui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );

            /* Carve the canvas declaratively: pad it, then per rung cut a row off the top and a
               label column off that row's right.  The line fills what is left, the tag aligns in the
               column -- no absolute offsets, so nothing can spill the border. */
            imgui_rect_t area = imgui_rect_pad( r, 12.0f );
            f32          lblw = imgui()->text_w( "16 px" ) + 12.0f;
            for ( int t = 1; t <= 6; ++t )
            {
                imgui_rect_t row = imgui_rect_cut_top( &area, 24.0f );
                imgui_rect_t lbl = imgui_rect_cut_right( &row, lblw );
                f32          cy  = row.y + row.h * 0.5f;
                char         tag[ 8 ];
                snprintf( tag, sizeof( tag ), "%d px", t );
                imgui()->draw_line( row.x, cy, row.x + row.w, cy, (f32)t, LINE_INK );
                imgui()->draw_text_in( lbl, IMGUI_ALIGN_LEFT | IMGUI_ALIGN_VCENTER, LINE_INK, tag );
            }
        }

        /* ---- (b) antialiased diagonal fan (uses Thickness) ---- */
        imgui()->separator_text( "Antialiased diagonals (Thickness)" );
        {
            imgui_rect_t r = imgui()->canvas( 140.0f );
            imgui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );
            f32 cx  = r.x + 24.0f;
            f32 cy  = r.y + r.h - 20.0f;
            f32 len = ( r.h - 36.0f );
            for ( int i = 0; i <= 8; ++i )
            {
                f32 a = ( PI * 0.5f ) * (f32)i / 8.0f;     /* sweep 0..90 deg */
                imgui()->draw_line( cx, cy, cx + cosf( a ) * len, cy - sinf( a ) * len,
                                    thickness, LINE_CYAN );
            }
        }

        /* ---- (c) the four alignments against the ideal path (uses Thickness + the radio) ---- */
        imgui()->separator_text( "Stroke alignment vs the ideal path" );
        {
            imgui_rect_t r = imgui()->canvas( 162.0f );
            imgui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );

            imgui_rect_t area = imgui_rect_pad( r, 12.0f );

            /* Caption: reserve a top strip (so the layout is stable whether or not it draws) and
               right-align the text in it -- the spill is impossible, the rect owns the right edge. */
            imgui_rect_t cap = imgui_rect_cut_top( &area, imgui()->line_h() );
            if ( show_guides )
                imgui()->draw_text_in( cap, IMGUI_ALIGN_RIGHT | IMGUI_ALIGN_VCENTER,
                                       LINE_PATH, "white = ideal path" );

            /* Cut a label column sized to the widest name; `area` is left as the segment region, so a
               long label like CENTER_BIASED can never run under the lines. */
            f32 label_w = 0.0f;
            for ( int i = 0; i < 4; ++i )
            {
                f32 w = imgui()->text_w( align_names[ i ] );
                if ( w > label_w ) label_w = w;
            }
            imgui_rect_t labels = imgui_rect_cut_left( &area, label_w + 12.0f );
            for ( int i = 0; i < 4; ++i )
            {
                f32          ly       = area.y + 16.0f + (f32)i * 30.0f;
                imgui_vec2_t seg[ 2 ] = { { area.x, ly }, { area.x + area.w, ly } };
                imgui()->draw_polyline( seg, 2, thickness, align_mode[ i ], false, LINE_AMBR_T );
                if ( show_guides )
                    imgui()->draw_line( area.x, ly, area.x + area.w, ly, 1.0f, LINE_PATH );

                imgui_rect_t lbl = { labels.x, ly - imgui()->line_h() * 0.5f, labels.w, imgui()->line_h() };
                imgui()->draw_text_in( lbl, IMGUI_ALIGN_LEFT | IMGUI_ALIGN_VCENTER,
                                       i == align_idx ? LINE_CYAN : LINE_INK, align_names[ i ] );
            }
        }

        /* ---- (d) closed shapes: square + circle (uses Thickness + alignment radio) ---- */
        imgui()->separator_text( "Closed shapes: draw_polyline (square / circle)" );
        {
            imgui_rect_t r = imgui()->canvas( 200.0f );
            imgui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );
            f32 cy = r.y + r.h * 0.5f - 6.0f;

            /* square -- four corners, the same "shape technique" as any polygon */
            f32 sx = r.x + r.w * 0.27f, hs = 44.0f;
            imgui_vec2_t sq[ 4 ] = {
                { sx - hs, cy - hs }, { sx + hs, cy - hs },
                { sx + hs, cy + hs }, { sx - hs, cy + hs } };
            line_shape( sq, 4, thickness, align_mode[ align_idx ], show_guides );
            imgui()->draw_text( sx - 22.0f, cy + hs + 14.0f, LINE_INK, "square" );

            /* circle -- a many-sided closed ring */
            f32 ox = r.x + r.w * 0.70f, rr = 48.0f;
            imgui_vec2_t cir[ 48 ];
            for ( int i = 0; i < 48; ++i )
            {
                f32 a = ( 2.0f * PI ) * (f32)i / 48.0f;
                cir[ i ] = ( imgui_vec2_t ){ ox + cosf( a ) * rr, cy + sinf( a ) * rr };
            }
            line_shape( cir, 48, thickness, align_mode[ align_idx ], show_guides );
            imgui()->draw_text( ox - 20.0f, cy + rr + 14.0f, LINE_INK, "circle" );
        }

        /* ---- (e) mitered polyline corners, with an adjustable spread (uses Thickness) ---- */
        imgui()->separator_text( "Polyline: mitered corners" );
        imgui()->slider_float( "Corner spread", &spread, 0.0f, 1.0f );
        {
            imgui_rect_t r = imgui()->canvas( 130.0f );
            imgui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );
            f32 cy   = r.y + r.h * 0.5f;
            f32 amp  = ( r.h * 0.5f - 16.0f ) * spread;      /* spread closes / opens the V's */
            f32 x    = r.x + 24.0f;
            f32 step = ( r.w - 48.0f ) / 6.0f;
            imgui_vec2_t zig[ 7 ];
            for ( int i = 0; i < 7; ++i )
                zig[ i ] = ( imgui_vec2_t ){ x + step * (f32)i, cy + ( ( i & 1 ) ? -amp : amp ) };
            imgui()->draw_polyline( zig, 7, thickness, IMGUI_STROKE_CENTER, false, LINE_CYAN );
        }

        /* ---- (f) closed path via the retained builder: polygon / star ---- */
        imgui()->separator_text( "Closed path: path_stroke (polygon / star)" );
        imgui()->checkbox( "Star", &star_mode ); imgui()->same_line( 12.0f );
        imgui()->drag_int( "Points", &poly_pts, 0.1f, 3, 10, "%d" );
        {
            imgui_rect_t r = imgui()->canvas( 200.0f );
            imgui()->draw_rect( r.x, r.y, r.w, r.h, LINE_BG );
            f32 pcx = r.x + r.w * 0.5f;
            f32 pcy = r.y + r.h * 0.5f;
            f32 pr  = ( r.h * 0.5f ) - 22.0f;

            imgui()->path_clear();
            if ( star_mode )
            {
                /* alternate outer / inner radius for a star */
                int n = poly_pts * 2;
                for ( int i = 0; i < n; ++i )
                {
                    f32 a  = -PI * 0.5f + ( 2.0f * PI ) * (f32)i / (f32)n;
                    f32 rr = ( i & 1 ) ? pr * 0.45f : pr;
                    imgui()->path_line_to( pcx + cosf( a ) * rr, pcy + sinf( a ) * rr );
                }
            }
            else
            {
                for ( int i = 0; i < poly_pts; ++i )
                {
                    f32 a = -PI * 0.5f + ( 2.0f * PI ) * (f32)i / (f32)poly_pts;
                    imgui()->path_line_to( pcx + cosf( a ) * pr, pcy + sinf( a ) * pr );
                }
            }
            imgui()->path_stroke( thickness, IMGUI_STROKE_CENTER, true, LINE_AMBR );
        }
    }
    imgui()->end_window();
}

/*==============================================================================================
    15. Tables -- multi-column layout with per-cell clipping and a clickable header row.

    table_begin opens a table of ncols columns.  table_setup_column names and sizes each column
    before the first row: STRETCH columns fill the remaining space equally, FIXED columns take
    an explicit pixel width.  table_headers_row draws the non-scrolling header strip and, when
    IMGUI_TABLE_SORTABLE is set, registers sort clicks.  Each data row starts with table_next_row;
    table_next_column clips the draw list and hit-test rect to the current cell.
==============================================================================================*/

static void
demo_table( void )
{
    imgui()->set_next_window_pos ( 60, 60, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 480, 480, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Tables", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();

        /* --- sortable three-column table -------------------------------------------------- */
        imgui()->separator_text( "table_headers_row / IMGUI_TABLE_SORTABLE" );

        static const struct { const char* name; const char* kind; float value; }
        k_items[] = {
            { "pos_x",   "float",  1.234f   },
            { "pos_y",   "float",  -5.678f  },
            { "health",  "int",    100.0f   },
            { "shield",  "int",    42.0f    },
            { "speed",   "float",  9.81f    },
            { "alive",   "bool",   1.0f     },
            { "score",   "int",    31337.0f },
        };
        const int k_item_count = (int)( sizeof( k_items ) / sizeof( k_items[ 0 ] ) );

        /* Display order, re-sorted in place whenever a header is clicked. */
        static int s_order[ 7 ];
        static bool s_order_init = false;
        if ( !s_order_init )
        {
            for ( int i = 0; i < k_item_count; ++i ) s_order[ i ] = i;
            s_order_init = true;
        }

        /* stretch Name, fixed Type (64 px), fixed Value (128 px); sortable + striped + framed */
        if ( imgui()->table_begin( "props", 3,
                                   IMGUI_TABLE_SORTABLE | IMGUI_TABLE_ROW_STRIPES
                                       | IMGUI_TABLE_BORDERS_V | IMGUI_TABLE_BORDERS_OUTER,
                                   0.0f ) )
        {
            imgui()->table_setup_column( "Name",  IMGUI_TABLE_COL_STRETCH,   0     );
            imgui()->table_setup_column( "Type",  IMGUI_TABLE_COL_FIXED,     64.0f );
            imgui()->table_setup_column( "Value", IMGUI_TABLE_COL_FIXED,    128.0f );
            imgui()->table_headers_row();

            /* On a header click, re-sort the index array by the chosen column/direction. */
            imgui_table_sort_specs_t specs;
            if ( imgui()->table_get_sort_specs( &specs ) )
            {
                for ( int a = 0; a < k_item_count - 1; ++a )      /* simple insertion sort */
                    for ( int b = a + 1; b < k_item_count; ++b )
                    {
                        const int ia = s_order[ a ], ib = s_order[ b ];
                        int cmp = 0;
                        if ( specs.col == 0 )
                            cmp = strcmp( k_items[ ia ].name, k_items[ ib ].name );
                        else if ( specs.col == 1 )
                            cmp = strcmp( k_items[ ia ].kind, k_items[ ib ].kind );
                        else
                            cmp = ( k_items[ ia ].value > k_items[ ib ].value ) -
                                  ( k_items[ ia ].value < k_items[ ib ].value );
                        if ( specs.descending ) cmp = -cmp;
                        if ( cmp > 0 ) { s_order[ a ] = ib; s_order[ b ] = ia; }
                    }
            }

            for ( int r = 0; r < k_item_count; ++r )
            {
                const int i = s_order[ r ];
                imgui()->table_next_row( 0 );
                if ( imgui()->table_next_column() )
                {
                    imgui()->stack();
                    imgui()->text( k_items[ i ].name );
                }
                if ( imgui()->table_next_column() )
                {
                    imgui()->stack();
                    imgui()->text_disabled( k_items[ i ].kind );
                }
                if ( imgui()->table_next_column() )
                {
                    imgui()->stack();
                    /* Tint the cell red when the value is negative (CELL bg override). */
                    if ( k_items[ i ].value < 0.0f )
                        imgui()->table_set_bg_color( IMGUI_TABLE_BG_CELL,
                                                     IMGUI_COLOR( 0xC0, 0x30, 0x30, 0x60 ) );
                    char buf[ 24 ];
                    snprintf( buf, sizeof( buf ), "%.3g", k_items[ i ].value );
                    imgui()->text( buf );
                }
            }
            imgui()->table_end();
        }

        /* --- table containing interactive widgets ----------------------------------------- */
        imgui()->spacing( 0 );
        imgui()->separator_text( "interactive cells (no header)" );

        static float s_vals[ 4 ] = { 0.25f, 0.5f, 0.75f, 1.0f };
        static const char* k_labels[] = { "Alpha", "Beta", "Gamma", "Delta" };

        if ( imgui()->table_begin( "sliders", 2, IMGUI_TABLE_BORDERS_H, 0.0f ) )
        {
            imgui()->table_setup_column( "Label",  IMGUI_TABLE_COL_FIXED,  60.0f );
            imgui()->table_setup_column( "Slider", IMGUI_TABLE_COL_STRETCH, 0    );
            /* No table_headers_row -- body opens automatically on the first table_next_row.
               IMGUI_TABLE_BORDERS_H draws a divider between each data row. */

            for ( int i = 0; i < 4; ++i )
            {
                imgui()->table_next_row( 0 );
                if ( imgui()->table_next_column() )
                {
                    imgui()->stack();
                    imgui()->text( k_labels[ i ] );
                }
                if ( imgui()->table_next_column() )
                {
                    imgui()->stack();
                    imgui()->push_id_int( i );
                    imgui()->slider_float( "##v", &s_vals[ i ], 0.0f, 1.0f );
                    imgui()->pop_id();
                }
            }
            imgui()->table_end();
        }
    }
    imgui()->end_window();
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
        imgui()->dock_save( 0, s_layout, sizeof( s_layout ) );
        s_have_layout = true;
        s_save_layout = false;
    }
    if ( s_restore_layout )
    {
        if ( s_have_layout ) imgui()->dock_load( 0, s_layout );
        s_restore_layout = false;
    }

    /* Lay out + interact the dockspace every frame (must precede the docked windows' begin_window). */
    imgui_dock_id_t root = imgui()->dockspace_over_viewport( 0, IMGUI_DOCKSPACE_NONE );

    /* Build the tree once: left rail, right inspector, a bottom strip, central viewport. */
    static bool built = false;
    if ( !built && root != IMGUI_DOCK_NONE )
    {
        imgui_dock_id_t left   = imgui()->dock_split( root, IMGUI_DIR_LEFT,  0.22f, &root );
        imgui_dock_id_t right  = imgui()->dock_split( root, IMGUI_DIR_RIGHT, 0.28f, &root );
        imgui_dock_id_t bottom = imgui()->dock_split( root, IMGUI_DIR_DOWN,  0.30f, &root );
        imgui()->dock_window( "Scene Tree", left   );
        imgui()->dock_window( "Inspector",  right  );
        imgui()->dock_window( "Console",    bottom );
        imgui()->dock_window( "Assets",     bottom );   /* tab alongside Console */
        imgui()->dock_window( "Viewport",   root   );   /* central remainder */
        built = true;
    }

    if ( imgui()->begin_window( "Scene Tree", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        if ( imgui()->tree_node( "World" ) )
        {
            imgui()->text( "Camera" );
            imgui()->text( "Sun Light" );
            if ( imgui()->tree_node( "Props" ) )
            {
                imgui()->text( "Crate" );
                imgui()->text( "Barrel" );
                imgui()->tree_pop();
            }
            imgui()->tree_pop();
        }
    }
    imgui()->end_window();

    if ( imgui()->begin_window( "Inspector", IMGUI_WIN_NONE ) )
    {
        static char name[ 32 ] = "Crate";
        static f32  pos[ 3 ]   = { 0.0f, 1.0f, 0.0f };
        static bool visible    = true;
        imgui()->stack();                     /* declare the layout mode before any widget */
        imgui()->field_label_left( 80.0f );
        imgui()->input_text  ( "Name",     name, sizeof( name ) );
        imgui()->input_float3( "Position", pos, NULL );
        imgui()->checkbox    ( "Visible",  &visible );
    }
    imgui()->end_window();

    if ( imgui()->begin_window( "Console", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "[info] engine started" );
        imgui()->text( "[info] dock layout built" );
        imgui()->text( "> _" );
    }
    imgui()->end_window();

    if ( imgui()->begin_window( "Assets", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->bullet_text( "models/crate.obj" );
        imgui()->bullet_text( "textures/wood.png" );
        imgui()->bullet_text( "shaders/lit.glsl" );
    }
    imgui()->end_window();

    if ( imgui()->begin_window( "Viewport", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "Central viewport panel." );
        imgui()->separator();
        imgui()->text( "Drag the gutters between regions to resize." );
        imgui()->text( "Click the Console / Assets tabs to switch." );
        imgui()->text( "Drag a tab OUT to pop it into a floater." );
        imgui()->text( "Drag the Palette window onto a pane to dock." );
        imgui()->separator();
        imgui()->text( "Rearrange, Save, rearrange more, then Restore:" );
        /* Two full-width stacked rows: a stack() button fills its column, so same_line would push the
           second button off the pane's right edge. */
        if ( imgui()->button( "Save Layout" ) )    s_save_layout    = true;
        if ( imgui()->button( "Restore Layout" ) ) s_restore_layout = true;
    }
    imgui()->end_window();

    /* A FREE (undocked) window to exercise the Phase-2 drag-to-dock gesture: drag its title bar over
       any pane to see the 5-way overlay, then drop on center (tab in) or a side (split). */
    imgui()->set_next_window_pos ( 980, 120, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 220, 150, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Palette", IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "I'm a floating window." );
        imgui()->separator();
        imgui()->text( "Drag my title bar over a" );
        imgui()->text( "pane to dock me (center =" );
        imgui()->text( "tab, sides = split)." );
    }
    imgui()->end_window();
}

/*==============================================================================================
    Demo table -- the menu the host steps through.
==============================================================================================*/

const sb_imgui_demo_t sb_imgui_demos[] =
{
    { "Widgets",      "text / button / checkbox / slider / input_text", demo_widgets     },
    { "Text",         "textf / bullet / separator / collapsing_header", demo_text        },
    { "Layout Rows",  "row / row_cols / row2..4 / row_track",           demo_layout_rows },
    { "Field Forms",  "field_label_left/right / field_split",           demo_fields      },
    { "Grid",         "grid_cells / skip",                              demo_grid        },
    { "Child / List", "begin_child / selectable / push_id",             demo_child_list  },
    { "Combo / List", "combo / listbox / begin_combo / begin_listbox",  demo_combo_list  },
    { "Align",        "align / same_line / spacing / separator",        demo_align       },
    { "Sub-layout",   "push_layout / pop_layout",                       demo_sublayout   },
    { "Pack / Bar",   "bar / pack_size / pack_nextline",                demo_pack        },
    { "Windows",      "multiple windows / flags / z-order",             demo_windows     },
    { "Auto-size",    "ALWAYS_AUTOSIZE / CAN_AUTOSIZE / auto child",    demo_autosize    },
    { "Menus",        "menu bar / begin_menu / menu_item / context",    demo_menus       },
    { "Lines / Paths","draw_line / draw_polyline / path_stroke",        demo_lines       },
    { "Tables",       "table_begin / setup_column / next_row / next_column", demo_table   },
    { "Docking",      "dockspace_over_viewport / dock_split / tabs",     demo_docking     },
    { NULL,           NULL,                                             NULL             },
};

int
sb_imgui_demo_count( void )
{
    int n = 0;
    while ( sb_imgui_demos[ n ].name )
        n++;
    return n;
}

/*==============================================================================================
    Picker overlay -- list every demo with the active one marked, plus the key hints.
==============================================================================================*/

int
sb_imgui_demo_picker( int active )
{
    const int count    = sb_imgui_demo_count();
    int       selected = active;                  /* unchanged unless a row is clicked */

    imgui()->set_next_window_pos ( 940, 20, IMGUI_COND_ONCE );
    imgui()->set_next_window_size( 320, 360, IMGUI_COND_ONCE );
    if ( imgui()->begin_window( "Demos", IMGUI_WIN_NOCOLLAPSE ) )
    {
        imgui()->stack();
        imgui()->text( "Keys: 1-9 select  +/- step  ESC quit" );
        imgui()->separator();

        for ( int i = 0; i < count; i++ )
        {
            imgui()->push_id_int( i );
            bool on = ( i == active );
            char label[ 64 ];
            snprintf( label, sizeof( label ), "%d. %s", i + 1, sb_imgui_demos[ i ].name );
            /* selectable returns true on the clicked frame -- report that row back to the
               host, which owns the real active index. */
            if ( imgui()->selectable( label, &on ) )
                selected = i;
            imgui()->pop_id();
        }

        imgui()->separator();
        if ( active >= 0 && active < count )
        {
            imgui()->textf( "%s", sb_imgui_demos[ active ].name );
            imgui()->text( sb_imgui_demos[ active ].desc );
        }
    }
    imgui()->end_window();

    return selected;
}

/*============================================================================================*/
// clang-format on
