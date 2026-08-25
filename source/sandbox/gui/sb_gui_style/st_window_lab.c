/*==============================================================================================

    sandbox/gui/sb_gui_style/st_window_lab.c -- Window Lab: window chrome under the live style.

    The Look Gallery sweeps widgets; this window sweeps the WINDOW itself.  A control panel of
    gui_win_flags_t toggles drives a companion subject window that is re-begun every frame with
    the chosen mask, so any flag combination -- no title bar, forced scrollbars, autosize,
    bottom-anchored content -- is judged against the current theme without recompiling.
    Immediate mode makes this free: the mask is just window_begin's last argument.

    Flags with no meaning for a free panel are left out: NATIVE (needs a borderless OS window),
    MODAL (would fence the lab panel itself off), the child/region-only bits, DEBUG_BAND and
    DOCK_MAXIMIZE.

==============================================================================================*/
// clang-format off

/* One toggleable flag row.  A non-NULL section starts a new group under its own separator. */
typedef struct
{
    const char* section;   // group header, or NULL to continue the current group
    const char* name;      // checkbox label -- the GUI_WIN_* enum suffix
    u32         bit;       // the gui_win_flags_t bit
    const char* desc;      // tooltip: what the flag does

} lab_flag_t;

static const lab_flag_t s_lab_flag_tbl[] =
{
    { "Chrome",       "NOTITLEBAR",        GUI_WIN_NOTITLEBAR,        "no title bar: body fills the top; no collapse, no titlebar drag" },
    { NULL,           "NOCOLLAPSE",        GUI_WIN_NOCOLLAPSE,        "no collapse arrow; the window stays expanded" },
    { NULL,           "MENUBAR",           GUI_WIN_MENUBAR,           "reserve a non-scrolling menu-bar strip below the title bar" },
    { "Move / size",  "NOMOVE",            GUI_WIN_NOMOVE,            "disable user drag moving the window" },
    { NULL,           "NORESIZE",          GUI_WIN_NORESIZE,          "disable user resizing from the border edges" },
    { NULL,           "NO_BOUNDARY_CLAMP", GUI_WIN_NO_BOUNDARY_CLAMP, "skip the keep-on-surface clamps; the window may leave the display" },
    { NULL,           "ALWAYS_AUTOSIZE",   GUI_WIN_ALWAYS_AUTOSIZE,   "hug content every frame: no user resize, no scrollbars" },
    { NULL,           "CAN_AUTOSIZE",      GUI_WIN_CAN_AUTOSIZE,      "corner size-grip; double-click it to fit content" },
    { "Scrolling",    "NOSCROLL",          GUI_WIN_NOSCROLL,          "no scroll bars (wheel input still scrolls)" },
    { NULL,           "NOMOUSESCROLL",     GUI_WIN_NOMOUSESCROLL,     "ignore the mouse wheel" },
    { NULL,           "HSCROLL",           GUI_WIN_HSCROLL,           "enable a dynamic horizontal bar (off by default)" },
    { NULL,           "ALWAYS_VSCROLL",    GUI_WIN_ALWAYS_VSCROLL,    "vertical bar always shown, content or not" },
    { NULL,           "ALWAYS_HSCROLL",    GUI_WIN_ALWAYS_HSCROLL,    "horizontal bar always shown, content or not" },
    { "Title bar",    "NO_MINIMIZE",       GUI_WIN_NO_MINIMIZE,       "drop the minimize button" },
    { NULL,           "NO_MAXIMIZE",       GUI_WIN_NO_MAXIMIZE,       "drop the maximize / restore button" },
    { NULL,           "NO_DETACH",         GUI_WIN_NO_DETACH,         "no pop-out: hide the detach button, block tear-off drag" },
    { NULL,           "CLOSEABLE",         GUI_WIN_CLOSEABLE,         "close (X) button; the Show checkbox re-opens it" },
    { NULL,           "NO_TAB_TARGET",     GUI_WIN_NO_TAB_TARGET,     "never hosts tabs dropped onto its title" },
    { "Behavior",     "NO_INPUT",          GUI_WIN_NO_INPUT,          "click-through: the cursor passes through the window" },
    { NULL,           "TEXT_SELECT",       GUI_WIN_TEXT_SELECT,       "text runs are selectable / copyable (drag over them, Ctrl+C)" },
    { NULL,           "ANCHOR_BOTTOM",     GUI_WIN_ANCHOR_BOTTOM,     "content hugs the bottom; scroll pins to the newest row" },
    { NULL,           "DRAG_TARGET",       GUI_WIN_DRAG_TARGET,       "whole body lights up as a landing zone while a drag is in flight" },
};

#define LAB_FLAG_COUNT ( (i32)( sizeof( s_lab_flag_tbl ) / sizeof( s_lab_flag_tbl[ 0 ] ) ) )

#define LAB_SUBJECT_TITLE "Window Subject"

static u32  s_lab_mask = GUI_WIN_NONE;   // the mask the subject window is begun with
static bool s_lab_show = true;           // subject visibility (also re-opens after a CLOSEABLE X)
static i32  s_lab_rows = 12;             // numbered content rows -- enough to overflow vertically
static bool s_lab_wide = false;          // emit one long unwrapped line, to exercise HSCROLL

/* The subject: an ordinary window whose only job is to wear the mask.  Content is sized by the
   panel's knobs so both scroll axes can be pushed into overflow on demand. */
static void
lab_subject_window( void )
{
    gui()->window_set_next_size( 420.0f, 320.0f, GUI_COND_ONCE );
    if ( gui()->window_begin( LAB_SUBJECT_TITLE, ( gui_win_flags_t )s_lab_mask ) )
    {
        if ( ( s_lab_mask & GUI_WIN_MENUBAR ) && gui()->menu_bar_begin() )
        {
            if ( gui()->menu_begin( "File" ) )
            {
                gui()->menu_item( "New",  NULL, NULL );
                gui()->menu_item( "Open", NULL, NULL );
                gui()->menu_end();
            }
            if ( gui()->menu_begin( "Edit" ) )
            {
                gui()->menu_item( "Undo", NULL, NULL );
                gui()->menu_end();
            }
            gui()->menu_bar_end();
        }

        gui()->stack();

        /* Natural-size widgets only: a cell that fills its track reports no width to the content
           measure (line_place_cell, flow/gui_layout_core.c), so under ALWAYS_AUTOSIZE a fill row
           would let the window collapse to its text rows and ellipsize the labels. */
        static bool cb   = true;
        static f32  fval = 0.42f;
        gui()->button( "Button" );
        gui()->same_line( -1.0f );
        gui()->checkbox( "Checkbox", &cb );
        gui()->slider_float( "##val", &fval, 0.0f, 1.0f );

        if ( s_lab_wide )
            gui()->text( "A deliberately long unwrapped line of text that overflows the window's "
                         "content width, so the horizontal scroll flags have something to chew on." );

        for ( i32 i = 0; i < s_lab_rows; ++i )
            gui()->textf( "Content row %d", i );
    }
    gui()->window_end();

    /* An X-close (CLOSEABLE) lands here as is_open false; sync the Show checkbox to it. */
    if ( !gui()->window_is_open( LAB_SUBJECT_TITLE ) )
        s_lab_show = false;
}

static void
st_window_lab_window( void )
{
    if ( !st_begin( "Window Lab", 300.0f, 620.0f ) )
    {
        gui()->window_end();
        return;
    }

    gui()->stack();

    if ( gui()->checkbox( "Show subject window", &s_lab_show ) && s_lab_show )
        gui()->window_set_open( LAB_SUBJECT_TITLE, true );   /* clear a CLOSEABLE X-close latch */

    /* Presets -- the named composites, plus the all-defaults baseline. */
    gui()->row_cols( 0.0f, (f32[]){ 1.0f, 1.0f, 1.0f, GUI_END } );
    if ( gui()->button( "Default" ) )       s_lab_mask = GUI_WIN_NONE;
    gui()->next_item_fit( 1.0f );
    if ( gui()->button( "No Decoration" ) ) s_lab_mask = GUI_WIN_NODECORATION;
    gui()->set_item_tooltip( "GUI_WIN_NODECORATION -- bare content panel" );
    gui()->next_item_fit( 1.0f );
    if ( gui()->button( "Overlay" ) )       s_lab_mask = GUI_WIN_OVERLAY;
    gui()->set_item_tooltip( "GUI_WIN_OVERLAY -- passive autosized HUD (NO_INPUT: control it from here)" );
    gui()->stack();

    for ( i32 i = 0; i < LAB_FLAG_COUNT; ++i )
    {
        const lab_flag_t* fl = &s_lab_flag_tbl[ i ];
        if ( fl->section )
            gui()->separator_text( fl->section );

        bool on = ( s_lab_mask & fl->bit ) != 0;
        if ( gui()->checkbox( fl->name, &on ) )
            s_lab_mask ^= fl->bit;
        gui()->set_item_tooltip( fl->desc );
    }

    gui()->separator_text( "Subject content" );
    gui()->slider_int( "Rows", &s_lab_rows, 0, 40, NULL );
    gui()->checkbox( "Wide line (overflows width)", &s_lab_wide );

    /* A payload to fly: drag this over the subject to see DRAG_TARGET light its body. */
    gui()->button( "Drag source -- fly me over the subject" );
    if ( gui()->drag_source_begin( GUI_DRAG_NONE ) )
    {
        static const i32 dummy = 0;
        gui()->drag_payload_set( "LAB_PAYLOAD", &dummy, sizeof dummy );
        gui()->text( "lab payload" );
        gui()->drag_source_end();
    }

    gui()->textf( "flags = 0x%08X", s_lab_mask );

    gui()->window_end();

    if ( s_lab_show )
        lab_subject_window();
}

// clang-format on
