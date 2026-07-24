/*==============================================================================================

    runtime_service/gui/gui_ui_mem.c -- Frontend (frame unit) memory accounting.

    The frame-unit counterpart of render/gui_render_mem.c: sizeof-sums every fixed static the
    gui_frame.c unity TU defines into one bucket (cpu_frontend_bytes), plus the carved units'
    footprints through their *_unit_mem_bytes seams.  Unlike the backend there are no big
    arenas here -- the real state lives in the malloc'd context blocks, already counted as
    CPU heap -- so this bucket is expected to register SMALL.  That is the point: the
    accounting contract is that the grand total is the true resident footprint, and a bucket
    that is known to be small beats one that is unknown.

    Also home to gui_mem_stats / gui_print_mem_stats: the full-footprint aggregation reads BOTH
    servers (gui_backend_memory + the core pool), which makes it orchestrator work, not
    interact-server work.

    MUST be the LAST include in the gui_frame.c unit root: every line below is a sizeof over
    another file's static, and unity visibility only flows downward.  Adding a static aggregate
    to this unit?  Add it here.

    Not counted: scalar statics (bools, counters, stack depths, hook pointers) -- sub-cache-line
    noise -- and string literals (pooled by the linker).

==============================================================================================*/
// clang-format off

u32
gui_ui_memory( void )
{
    u32 b = 0;

    /* core/ -- THE INTERACT SERVER is its own unit (gui_core.c) and accounts for its
       own statics (ambient records, io snapshot, id/flag stacks, context pool) via its seam. */
    b += gui_core_unit_mem_bytes();

    /* style/ -- THE STYLE UNIT is its own unit (gui_style.c) and accounts for its
       own statics (base/active style, theme table, stacks + pair tables) via its seam. */
    b += gui_style_unit_mem_bytes();

    /* flow/ -- THE FLOW UNIT is its own unit (gui_flow.c) and accounts for its own
       statics (layout state pool, split stack, sublayout sink) via its seam. */
    b += gui_flow_unit_mem_bytes();

    /* interact/ -- THE INTERACT UNIT is its own unit (gui_interact.c) and accounts
       for its own statics (the drag payload slot) via its seam; the text-selection controller
       moved to chrome (chrome/window/gui_select.c) and is counted there. */
    b += gui_interact_unit_mem_bytes();

    /* component/ -- THE COMPONENT UNIT (widget logic, staging) accounts for its own statics
       (none yet) via its seam (gui_component.c). */
    b += gui_component_unit_mem_bytes();

    /* stock/ -- THE STOCK UNIT (reference widget set) accounts for its own statics (the
       installed element style + the slot map) via its seam (gui_stock.c). */
    b += gui_stock_unit_mem_bytes();

    /* widgets/ + table/ + dock/ + popup/ -- the chrome unit accounts for its own statics
       (gui_chrome.c seam). */
    b += gui_chrome_unit_mem_bytes();

    /* text/ -- THE TEXT LEAF (gui_font.c) accounts for the loaded-font registry (glyph metric
       tables) via its seam; it moved down from the draw unit so both servers can measure text. */
    b += gui_font_unit_mem_bytes();

    /* frame/ + root -- lifecycle stacks, boot/present state. */
    b += (u32)( sizeof( s_ctx_save_stack ) + sizeof( s_font_stack )
              + sizeof( s_boot ) + sizeof( s_present ) );

    /* core/gui_ctx.c -- the one global viewport table (s_vp_pool).  A fixed static now, not part
       of any context's per-context heap block, so it is counted here rather than under
       cpu_context_bytes below. */
    b += (u32)sizeof( s_vp_pool );

    /* debug/ -- the perf/state HUD accumulators (always compiled; runtime-gated) live in THIS
       unit; the dashboard / stepper statics moved to the debug unit (gui_debug.c), which
       reports its own fixed footprint through the seam. */
    b += (u32)sizeof( s_perf );
    b += gui_debug_unit_mem_bytes();

    return b;
}

/*==============================================================================================
    Memory Stats

    A full accounting of the gui system's resident footprint, split by where it lives (GPU device
    memory / fixed CPU .bss / per-context CPU heap).  The backend fills its own buckets through
    gui_backend_memory (GPU buffers + the fixed backend buffers); this unit owns the aggregation,
    counting the live GPU surfaces to scale the geometry buffers and summing the per-context
    malloc blocks (core's pool, reached through the gui_ctx.h externs) for the heap total.
    See gui_mem_stats_t (gui.h) for the bucket meanings.
==============================================================================================*/

gui_mem_stats_t
gui_mem_stats( void )
{
    /* Count live GPU surfaces in the one global viewport table (a viewport is live once it owns
       geometry buffers) so the backend can scale the per-surface VB/IB by the true surface count --
       the old report assumed a single surface and undercounted every floater / secondary window. */
    u32 live_viewports = 0;
    for ( u32 v = 0; v < GUI_MAX_VIEWPORTS; ++v )
        if ( rhi_handle_valid( s_vp_pool[ v ].vb ) )
            ++live_viewports;

    /* Backend fills GPU + CPU .bss; this unit adds the frontend statics, the CPU-heap context
       blocks, and the totals. */
    gui_mem_stats_t s = gui_backend_memory( live_viewports );

    s.cpu_frontend_bytes = gui_ui_memory();
    s.cpu_static_total  += s.cpu_frontend_bytes;

    /* CPU heap: one malloc block per live context (recorded at allocation as _alloc_size). */
    for ( u32 i = 0; i < s_ctx_pool_count; ++i )
    {
        gui_context_t* ctx = s_ctx_pool[ i ];
        if ( !ctx ) continue;
        s.cpu_context_bytes += ctx->_alloc_size;
        ++s.context_count;
    }
    s.cpu_dynamic_total = s.cpu_context_bytes;

    s.total_bytes = s.gpu_total + s.cpu_static_total + s.cpu_dynamic_total;
    return s;
}

/* Dump the full breakdown to stdout as a sectioned table: GPU / CPU static / CPU heap, each with
   its own subtotal, then the grand total.  Bytes and KiB side by side so small (font glyph tables)
   and large (geometry buffers) buckets are both legible at a glance. */
void
gui_print_mem_stats( void )
{
    gui_mem_stats_t s = gui_mem_stats();
    const f32 kb = 1024.0f;

    #define GUI_MEM_ROW( label, bytes ) \
        printf( "  %-22s %10u B  (%8.1f KB)\n", (label), (u32)(bytes), (u32)(bytes) / kb )

    printf( "[gui] memory usage -- full breakdown:\n" );

    printf( "  -- GPU device (%u live surface%s) ------------------------\n",
            s.viewport_count, s.viewport_count == 1u ? "" : "s" );
    GUI_MEM_ROW( "vertex buffers",   s.gpu_vertex_bytes  );
    GUI_MEM_ROW( "index buffers",    s.gpu_index_bytes   );
    GUI_MEM_ROW( "font atlas texture", s.gpu_texture_bytes );
    if ( s.gpu_debug_bytes )
        GUI_MEM_ROW( "debug overlay buffers", s.gpu_debug_bytes );
    GUI_MEM_ROW( "  GPU subtotal",   s.gpu_total         );

    printf( "  -- CPU static (fixed backend buffers) ------------------\n" );
    GUI_MEM_ROW( "draw command list",  s.cpu_drawlist_bytes );
    GUI_MEM_ROW( "tessellation stage", s.cpu_tess_bytes     );
    GUI_MEM_ROW( "retained cache",     s.cpu_cache_bytes    );
    GUI_MEM_ROW( "font registry",      s.cpu_font_bytes     );
    GUI_MEM_ROW( "atlas + icons",      s.cpu_res_bytes      );
    GUI_MEM_ROW( "render + shaders",   s.cpu_render_bytes   );
    GUI_MEM_ROW( "text-select capture", s.cpu_select_bytes  );
    if ( s.cpu_debug_bytes )
        GUI_MEM_ROW( "debug tooling",  s.cpu_debug_bytes    );
    GUI_MEM_ROW( "frontend statics",   s.cpu_frontend_bytes );
    GUI_MEM_ROW( "  CPU static subtotal", s.cpu_static_total );

    printf( "  -- CPU heap (%u context%s) -----------------------------\n",
            s.context_count, s.context_count == 1u ? "" : "s" );
    GUI_MEM_ROW( "context blocks",        s.cpu_context_bytes );
    GUI_MEM_ROW( "  CPU heap subtotal",   s.cpu_dynamic_total );

    printf( "  --------------------------------------------------------\n" );
    printf( "  %-22s %10u B  (%8.1f KB)  (%.1f MB)\n",
            "TOTAL", s.total_bytes, s.total_bytes / kb, s.total_bytes / ( kb * kb ) );

    #undef GUI_MEM_ROW
}

// clang-format on
/*============================================================================================*/
