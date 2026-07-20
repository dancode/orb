/*==============================================================================================

    runtime_service/gui/gui_ui_mem.c -- Frontend (UI unit) memory accounting.

    The UI-unit counterpart of backend/gui_backend_mem.c: sizeof-sums every fixed static the
    gui.c unity TU defines into one bucket (cpu_frontend_bytes), read by gui_mem_stats
    (core/gui_ctx.c, which forward-declares gui_ui_memory).  Unlike the backend there are no
    big arenas here -- the frontend's real state lives in the malloc'd context blocks, already
    counted as CPU heap -- so this bucket is expected to register SMALL.  That is the point:
    the accounting contract is that the grand total is the true resident footprint, and a
    bucket that is known to be small beats one that is unknown.

    MUST be the LAST include in gui.c (before the gui_api.c vtable block): every line below is
    a sizeof over another file's static, and unity visibility only flows downward.  Adding a
    static aggregate to the UI unit?  Add it here.

    Not counted: scalar statics (bools, counters, stack depths, hook pointers) -- sub-cache-line
    noise -- and string literals (pooled by the linker).

==============================================================================================*/
// clang-format off

u32
gui_ui_memory( void )
{
    u32 b = 0;

    /* core/ -- ambient context records, io snapshot, identity + bracketing stacks. */
    b += (u32)( sizeof( s_interaction ) + sizeof( s_build ) + sizeof( s_scope )
              + sizeof( s_item_flag_stack ) + sizeof( s_layout_stack )
              + sizeof( s_ctx_pool ) + sizeof( s_id_stack ) );
    b += (u32)( sizeof( s_io ) + sizeof( s_click_elapsed )
              + sizeof( s_click_x ) + sizeof( s_click_y ) );

    /* core/ -- style + theme: base/active style, theme table (.rdata), stacks + pair tables. */
    b += (u32)( sizeof( s_style_base ) + sizeof( s_style ) + sizeof( k_themes )
              + sizeof( s_slot ) + sizeof( s_col_stack ) + sizeof( s_var_stack )
              + sizeof( s_next ) + sizeof( s_item ) );

    /* compose/ -- layout state, split, sublayout scratch. */
    b += (u32)( sizeof( s_layout_state_stack ) + sizeof( s_split_stack )
              + sizeof( s_sublayout_sink ) );

    /* interact/ -- drag payload service + text-selection controller. */
    b += (u32)( sizeof( s_drag ) + sizeof( s_select ) );

    /* widgets/ -- the single-line and multiline undo buffers (the frontend's largest statics),
       numeric edit scratch, tab-bar stack. */
    b += (u32)( sizeof( s_undo ) + sizeof( s_medit_undo )
              + sizeof( s_num_edit_buf ) + sizeof( s_tabbars ) );

    /* table/ -- the ambient table record. */
    b += (u32)( sizeof( s_tab ) + sizeof( s_tab_scroll_dummy ) );

    /* dock/ + popup/ -- gesture latches, menu-bar scratch, tooltip save slot. */
    b += (u32)( sizeof( s_dock_drag ) + sizeof( s_dock_tab_drag ) + sizeof( s_dock_float_req ) );
    b += (u32)( sizeof( s_menubar_sink ) + sizeof( s_menubar_saved_clip )
              + sizeof( s_tooltip_save ) );

    /* frame/ + root -- lifecycle stacks, boot/present state, forwarded caps. */
    b += (u32)( sizeof( s_ctx_save_stack ) + sizeof( s_font_stack )
              + sizeof( s_boot ) + sizeof( s_present ) + sizeof( s_fwd_caps ) );

    /* debug/ -- the perf/state HUD accumulators (always compiled; runtime-gated) and the
       dashboard's palette when the feature is in. */
    b += (u32)sizeof( s_perf );
#ifdef GUI_PIPELINE_DASHBOARD
    b += (u32)sizeof( s_dash_palette );
#endif

    return b;
}

// clang-format on
/*============================================================================================*/
