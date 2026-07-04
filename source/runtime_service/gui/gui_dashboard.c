/*==============================================================================================

    runtime_service/gui/gui_dashboard.c -- Pipeline dashboard window shell (UI half).

    The window half of the pipeline diagnostic dashboard (see backend/gui_dash_overlay.c for
    the content renderer and the full feature description).  This file emits ONLY chrome
    through the normal pipeline: a regular window_begin window (movable / dockable / tear-off
    into its own viewport, like any panel), the panel headers, the freeze toggle, one canvas()
    reservation per panel, and the hover tooltips.  The heavy diagnostic geometry -- memory-map
    bars, batch rows, labels -- is expanded by the backend half at flush time into its own
    vertex/index buffers, scissored to the canvas rects registered here, so the dashboard never
    contributes the data it is measuring.

    The window itself still occupies a geometry slot like any other (its chrome is real UI); it
    is exempted from the stats it reports and from the any_changed idle-skip signal via
    g_gui_dash_window_id (cache_win_exempt, gui_build_cache.c) -- the perf-overlay rule -- and
    its own slot is drawn with a distinct marker in the memory map: the observer is part of the
    plumbing, marked as such, not hidden.

    Emit gui()->pipeline_dashboard( &open ) once per frame inside a ctx scope, like
    perf_overlay.  Included by gui.c after the popup tier (tooltips) and before gui_frame.c.
    Compiled out unless GUI_PIPELINE_DASHBOARD (gui_backend.h); the vtable slot stays a no-op
    stub then so func_api_size is hot-reload stable.

==============================================================================================*/
// clang-format off

/* The dashboard window's id -- read by the backend cache exemption even when the feature is
   compiled out (stays 0 then, and the exemption never matches). */
gui_id_t g_gui_dash_window_id = 0;

#ifdef GUI_PIPELINE_DASHBOARD

#define DASH_SHELL_TITLE "Pipeline Dashboard"   /* id_hash of this = g_gui_dash_window_id */

/* One panel: header text + a canvas reservation registered with the backend, plus arming the
   private hover tooltip.  The canvas rect is re-registered every emit frame; on idle frames the
   backend keeps the last rects (nothing can move without input, and input dirties the frame).

   The tooltip is NOT a normal gui tooltip: dash content flushes after the whole UI, so a
   tooltip window would be painted over -- and its per-mouse-move re-tessellation kept churning
   the slot arena the dashboard displays (the memory map visibly flickered).  Instead the shell
   only arms the cursor position; the backend probes its hit rects at flush time and draws the
   text block through its own buffers, last, on top of everything it painted. */
static void
dash_shell_panel( u32 panel, const char* title, f32 h )
{
    gui_separator_text( title );

    gui_rect_t r = gui_canvas( h );
    gui_dash_canvas( panel, r, s_build.cur_viewport );

    /* Gated on this window owning the hover so a window floating above does not probe through. */
    if ( s_interaction.hover_win == g_gui_dash_window_id
         && gui_is_mouse_hovering_rect( r ) )
        gui_dash_tooltip( s_io.mouse_x, s_io.mouse_y );
}

void
gui_pipeline_dashboard( bool* open )
{
    /* Clear this emit's canvas registrations first, open or not -- a closed dashboard must
       leave no live canvases behind or the backend keeps flushing the stale rects. */
    gui_dash_ui_begin();

    if ( !open || !*open )
        return;

    g_gui_dash_window_id = id_hash( DASH_SHELL_TITLE );

    /* The host said open: reopen the pool entry if the X button hid it on an earlier run. */
    gui_window_set_open( DASH_SHELL_TITLE, true );

    gui_window_set_next_size( 580.0f, 780.0f, GUI_COND_ONCE );
    if ( gui_window_begin( DASH_SHELL_TITLE, GUI_WIN_CLOSEABLE ) )
    {
        gui_stack();

        bool frozen = gui_dash_frozen();
        if ( gui_checkbox( "Freeze capture", &frozen ) )
            gui_dash_set_freeze( frozen );

        /* Panel heights derive from the live line height so text rows never clip mid-glyph:
           the row metrics inside each expander (backend) use the same font accessors. */
        f32 lh = gui_line_h();

        dash_shell_panel( GUI_DASH_PANEL_VBMAP,    "Vertex arena (slot memory map)", lh + 58.0f );
        dash_shell_panel( GUI_DASH_PANEL_IBMAP,    "Index arena (slot memory map)",  lh + 58.0f );
        dash_shell_panel( GUI_DASH_PANEL_FIF,      "Frames in flight / uploads",     96.0f );
        dash_shell_panel( GUI_DASH_PANEL_BATCH,    "Draw batches (dispatch order)",  8.0f * ( lh + 3.0f ) + 6.0f );
        dash_shell_panel( GUI_DASH_PANEL_EMIT,     "Emit + build buffers",           8.0f * ( lh + 2.0f ) + 6.0f );
        dash_shell_panel( GUI_DASH_PANEL_VOLATILE, "Volatile sub-slots",             4.0f * ( lh + 2.0f ) + lh );
        dash_shell_panel( GUI_DASH_PANEL_STATS,    "Frame stats",                    3.0f * lh + 10.0f );
    }
    gui_window_end();

    /* The X button closed it this frame: report back so the host toggle stays in sync. */
    if ( !gui_window_is_open( DASH_SHELL_TITLE ) )
        *open = false;
}

#else  /* !GUI_PIPELINE_DASHBOARD */

/* No-op stub: the vtable slot exists in every build so func_api_size is identical across a
   hot-reload (the debug-slot ABI rule, gui_api.h). */
void
gui_pipeline_dashboard( bool* open )
{
    (void)open;
}

#endif /* GUI_PIPELINE_DASHBOARD */

// clang-format on
/*============================================================================================*/
