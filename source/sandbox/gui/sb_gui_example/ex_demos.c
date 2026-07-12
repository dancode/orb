/*==============================================================================================

    sandbox/gui/sb_gui_example/ex_demos.c -- gui feature demos (root unity TU).

    The registry + main menu bar for the exhaustive gui feature suite.  The demo bodies live in
    per-category files included below (ex_widgets.c, ex_layout.c, ...) so each reads as a worked
    example of one part of the gui API; this file owns the shared helpers, the category table,
    and the per-frame driver (ex_frame).  See ex_demos.h for the contract.

    All persistent widget values are function-local statics -- the demos are pure UI, no shared
    state, so a demo can be read top to bottom in isolation.

==============================================================================================*/

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "ex_demos.h"
#include "runtime_service/gui/gui_host.h"
#include "engine/sys/sys_host.h"          /* sys_exe_dir (fonts demo), sys_tick_seconds */

// clang-format off

/*==============================================================================================
    Shared helpers -- used by every demo file included below.
==============================================================================================*/

/* Open a demo's primary window: seed its first-appearance geometry, then begin it CLOSEABLE
   (the titlebar X hides it; the registry re-syncs the menu check).  Demo windows keep the
   default edge-resize behavior on both axes -- only the Window Playground opts out, by passing
   its own interactive flag set through `extra`. */
static bool
ex_begin( const char* title, f32 x, f32 y, f32 w, f32 h, gui_win_flags_t extra )
{
    gui()->window_set_next_pos ( x, y, GUI_COND_ONCE );
    gui()->window_set_next_size( w, h, GUI_COND_ONCE );
    return gui()->window_begin( title, GUI_WIN_CLOSEABLE | extra );
}

/* One checkbox bound to one bit of a flag word -- the building block of the interactive flag
   suites (window flags, table flags, color-edit flags). */
static bool
ex_flag_checkbox( const char* label, u32* flags, u32 bit )
{
    bool on      = ( *flags & bit ) != 0;
    bool changed = gui()->checkbox( label, &on );
    if ( changed )
        *flags = on ? ( *flags | bit ) : ( *flags & ~bit );
    return changed;
}

/*==============================================================================================
    Demo bodies -- one file per category, unity-included so the build config stays one unit.
==============================================================================================*/

#include "ex_widgets.c"
#include "ex_layout.c"
#include "ex_windows.c"
#include "ex_style.c"
#include "ex_draw.c"
#include "ex_interact.c"
#include "ex_data.c"

/*==============================================================================================
    Registry -- grouped by category; the menu bar iterates this table in order.
==============================================================================================*/

static ex_demo_t s_demos[] =
{
    /* category    menu item          window title        description                                          fn                      open */
    { "Widgets",  "Basic Widgets",    "Basic Widgets",    "button / checkbox / radio / progress / repeat",     ex_widgets_basic,       true  },
    { "Widgets",  "Text & Trees",     "Text & Trees",     "text runs / wrapping / headers / tree nodes",       ex_widgets_text,        false },
    { "Widgets",  "Text Inputs",      "Text Inputs",      "input_text / hints / change callback / focus",      ex_widgets_input_text,  false },
    { "Widgets",  "Numeric Inputs",   "Numeric Inputs",   "input_int/float/double / steps / vector rows",      ex_widgets_numeric,     false },
    { "Widgets",  "Sliders & Drags",  "Sliders & Drags",  "slider_float/int/step / drag_int/floatN",           ex_widgets_sliders,     false },
    { "Widgets",  "Color Editors",    "Color Editors",    "color_edit3/4 + display flags",                     ex_widgets_color,       false },
    { "Widgets",  "Selection & Lists","Selection & Lists","selectable / combo / listbox + begin/end forms",    ex_widgets_selection,   false },

    { "Layout",   "Rows & Columns",   "Rows & Columns",   "stack / row / cols / row2..4 / layout desc",        ex_layout_rows,         false },
    { "Layout",   "Field Forms",      "Field Forms",      "form / field_split / field_label_left/right",       ex_layout_fields,       false },
    { "Layout",   "Grid",             "Grid",             "grid_cells / skip -- bounded matrix",               ex_layout_grid,         false },
    { "Layout",   "Align & Spacing",  "Align & Spacing",  "align / same_line / new_line / next_item_fit",      ex_layout_align,        false },
    { "Layout",   "Sub-layout",       "Sub-layout",       "push_layout / pop_layout in one cell",              ex_layout_sublayout,    false },
    { "Layout",   "Pack & Bars",      "Pack & Bars",      "bar / strip / pack_size / pack_nextline",           ex_layout_pack,         false },
    { "Layout",   "Child Regions",    "Child Regions",    "child_begin / resize grips / size constraints",     ex_layout_children,     false },
    { "Layout",   "Sizing Helpers",   "Sizing Helpers",   "sz_* family / content_avail / empty",               ex_layout_sizing,       false },
    { "Layout",   "Split & Carve",    "Split & Carve",    "split_begin / split / carve / anchor / overlay",    ex_layout_carve,        false },

    { "Windows",  "Window Playground","Window Playground","every window flag, toggled live",                   ex_windows_playground,  false },
    { "Windows",  "Multiple Windows", "Default Window",   "overlap / z-order / closeable / control",           ex_windows_multi,       false },
    { "Windows",  "Auto-size",        "Always Auto-size", "ALWAYS_AUTOSIZE / CAN_AUTOSIZE / auto child",       ex_windows_autosize,    false },
    { "Windows",  "Popups & Modals",  "Popups & Modals",  "popup_open/begin / modal / context menus",          ex_windows_popups,      false },
    { "Windows",  "Tooltips",         "Tooltips",         "set_item_tooltip / tooltip_begin / help_marker",    ex_windows_tooltips,    false },
    { "Windows",  "Menu Bars",        "Menu Bars",        "window menu bar / submenus / checkable items",      ex_windows_menus,       false },

    { "Style",    "Themes",           "Themes",           "theme_list / theme_set / theme_reset",              ex_style_themes,        false },
    { "Style",    "Style Stacks",     "Style Stacks",     "push_style_color/var / next_* / scale ramp",        ex_style_stacks,        false },
    { "Style",    "Widget Shape Tags","Widget Shape Tags","check/bullet/arrow/separator/knob style vars",      ex_style_shape_tags,    false },
    { "Style",    "Fonts",            "Fonts",            "font_load / push_font / text_size / atlas view",    ex_style_fonts,         false },

    { "Draw",     "Lines & Paths",    "Lines & Paths",    "draw_line / draw_polyline / path_stroke",           ex_draw_lines,          false },
    { "Draw",     "Shape Primitives", "Shape Primitives", "the parametric draw_* shape palette",               ex_draw_shapes,         false },
    { "Draw",     "Icons & Images",   "Icons & Images",   "register_icon / image / draw_icon_in",              ex_draw_icons,          false },
    { "Draw",     "Custom Widgets",   "Custom Widgets",   "canvas + item() + rect algebra + draw_text_in",     ex_draw_custom,         false },
    { "Draw",     "Volatile Widgets", "Volatile Widgets", "volatile_cb -- animation on idle frames",           ex_draw_volatile,       false },

    { "Interact", "Drag & Drop",      "Drag & Drop",      "drag_source/target / payloads / peek",              ex_interact_dragdrop,   false },
    { "Interact", "Item Queries",     "Item Queries",     "the is_item_* family + get_item_rect",              ex_interact_queries,    false },
    { "Interact", "Keyboard & Focus", "Keyboard & Focus", "focus / caret / edit key hook / key readers",       ex_interact_keyboard,   false },
    { "Interact", "Mouse & Cursor",   "Mouse & Cursor",   "mouse readers / hardware cursor / capture",         ex_interact_mouse,      false },

    { "Data",     "Tables",           "Tables",           "table flags / sortable headers / widgets in cells", ex_data_tables,         false },
    { "Data",     "Debug & Stats",    "Debug & Stats",    "overlay layers / render modes / stats levers",      ex_data_debug,          false },
};

#define EX_DEMO_COUNT ( (i32)( sizeof( s_demos ) / sizeof( s_demos[ 0 ] ) ) )

/* Show / hide one demo, keeping gui's internal CLOSEABLE latch in sync (a window the user
   X-closed stays latched shut inside gui until window_set_open re-opens it). */
static void
ex_set_open( ex_demo_t* d, bool open )
{
    d->open = open;
    if ( open )
        gui()->window_set_open( d->title, true );
}

/*==============================================================================================
    Main menu bar -- one menu per category, one checkable item per demo window.
==============================================================================================*/

static void
ex_menu_bar( void )
{
    if ( !gui()->main_menu_bar_begin() )
        return;

    /* Explorer menu: bulk open/close, plus the pointer to the docking test bed. */
    if ( gui()->menu_begin( "Explorer" ) )
    {
        if ( gui()->menu_item( "Open all",  NULL, NULL ) )
            for ( i32 i = 0; i < EX_DEMO_COUNT; i++ ) ex_set_open( &s_demos[ i ], true );
        if ( gui()->menu_item( "Close all", NULL, NULL ) )
            for ( i32 i = 0; i < EX_DEMO_COUNT; i++ ) ex_set_open( &s_demos[ i ], false );
        gui()->separator();
        gui()->push_item_flag( GUI_ITEM_DISABLED, true );
        gui()->menu_item( "Docking lives in sb_gui_dock", NULL, NULL );
        gui()->pop_item_flag();
        gui()->menu_end();
    }

    /* Category menus -- the table is grouped, so a category change closes the previous menu. */
    const char* cat      = NULL;
    bool        cat_open = false;
    for ( i32 i = 0; i < EX_DEMO_COUNT; i++ )
    {
        ex_demo_t* d = &s_demos[ i ];
        if ( cat == NULL || strcmp( cat, d->category ) != 0 )
        {
            if ( cat_open )
                gui()->menu_end();
            cat      = d->category;
            cat_open = gui()->menu_begin( cat );
        }
        if ( cat_open && gui()->menu_item( d->name, NULL, &d->open ) )
            ex_set_open( d, d->open );          /* re-arm the CLOSEABLE latch on open */
    }
    if ( cat_open )
        gui()->menu_end();

    gui()->main_menu_bar_end();
}

/*==============================================================================================
    Frame driver -- menu bar + every open demo, syncing the X button back into the registry.
==============================================================================================*/

void
ex_frame( void )
{
    ex_menu_bar();

    for ( i32 i = 0; i < EX_DEMO_COUNT; i++ )
    {
        ex_demo_t* d = &s_demos[ i ];
        if ( !d->open )
            continue;

        d->fn();

        /* The titlebar X closed the primary window this frame -- reflect it in the menu. */
        if ( !gui()->window_is_open( d->title ) )
            d->open = false;
    }
}

/*============================================================================================*/
// clang-format on
