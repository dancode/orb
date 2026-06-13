/*==============================================================================================

    sandbox/vulkan/sb_vulkan_imgui.c -- imgui feature demos.

    One function per feature group, each opening its own window.  Kept small and focused so the
    window on screen reads as a worked example of exactly one part of the imgui API; the host
    switches between them with a key.  See sb_vulkan_imgui.h for the contract.

    All persistent widget values are file-scope statics local to each demo function -- the demos
    are pure UI, no shared state, so a demo can be read top to bottom in isolation.

==============================================================================================*/

#include <stdio.h>

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
    if ( imgui()->begin_window( "Widgets", 60, 60, 360, 420, IMGUI_WIN_NONE ) )
    {
        imgui()->stack();                       /* declare the mode: a vertical list */
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
    if ( imgui()->begin_window( "Text & Sections", 60, 60, 380, 460, IMGUI_WIN_NONE ) )
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
    if ( imgui()->begin_window( "Layout Rows", 60, 60, 440, 420, IMGUI_WIN_NONE ) )
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

    if ( imgui()->begin_window( "Field Forms", 60, 60, 400, 420, IMGUI_WIN_NONE ) )
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
    if ( imgui()->begin_window( "Grid", 60, 60, 420, 440, IMGUI_WIN_NONE ) )
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
    if ( imgui()->begin_window( "Child / List Box", 60, 60, 360, 440, IMGUI_WIN_NONE ) )
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
    if ( imgui()->begin_window( "Align & Spacing", 60, 60, 380, 400, IMGUI_WIN_NONE ) )
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
    if ( imgui()->begin_window( "Sub-layout", 60, 60, 400, 360, IMGUI_WIN_NONE ) )
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
    if ( imgui()->begin_window( "Pack / Bar", 60, 60, 440, 320, IMGUI_WIN_NONE ) )
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
    if ( imgui()->begin_window( "Default Window", 80, 80, 280, 200, IMGUI_WIN_NONE ) )
    {
        imgui()->stack();
        imgui()->text( "Default flags." );
        imgui()->text( "Title bar, collapse, resize." );
        imgui()->text( "Click another window to raise it." );
    }
    imgui()->end_window();

    if ( imgui()->begin_window( "No Title Bar", 220, 180, 280, 180, IMGUI_WIN_NOTITLEBAR ) )
    {
        imgui()->stack();
        imgui()->text( "IMGUI_WIN_NOTITLEBAR" );
        static bool t = false;
        imgui()->checkbox( "a toggle", &t );
    }
    imgui()->end_window();

    if ( imgui()->begin_window( "Fixed", 360, 280, 260, 160,
                                IMGUI_WIN_NORESIZE | IMGUI_WIN_NOMOVE ) )
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
    if ( imgui()->begin_window( "Always Auto-size", 60, 60, 0, 0, IMGUI_WIN_ALWAYS_AUTOSIZE ) )
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
    if ( imgui()->begin_window( "Double-click grip", 360, 60, 300, 320,
                                IMGUI_WIN_CAN_AUTOSIZE ) )
    {
        imgui()->stack();
        imgui()->text( "CAN_AUTOSIZE: drag the triangle" );
        imgui()->text( "grip in the corner to resize," );
        imgui()->text( "or double-click it to snap back" );
        imgui()->text( "to fit this content." );
    }
    imgui()->end_window();

    /* (c) Auto-height child (h <= 0) + content_avail(). */
    if ( imgui()->begin_window( "Auto child", 360, 420, 320, 260, IMGUI_WIN_NONE ) )
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
    { "Align",        "align / same_line / spacing / separator",        demo_align       },
    { "Sub-layout",   "push_layout / pop_layout",                       demo_sublayout   },
    { "Pack / Bar",   "bar / pack_size / pack_nextline",                demo_pack        },
    { "Windows",      "multiple windows / flags / z-order",             demo_windows     },
    { "Auto-size",    "ALWAYS_AUTOSIZE / CAN_AUTOSIZE / auto child",    demo_autosize    },
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

    if ( imgui()->begin_window( "Demos", 940, 20, 320, 360, IMGUI_WIN_NOCOLLAPSE ) )
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
