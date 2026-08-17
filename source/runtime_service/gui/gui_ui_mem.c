/*==============================================================================================

    runtime_service/gui/gui_ui_mem.c -- Frontend (frame unit) memory accounting.

    Answers one question: how much memory is the GUI actually using right now? This file adds
    up the size of every fixed-size global the GUI keeps -- state pools, caches, tables --
    across every unit, plus each loaded font's resident pixel data, into one total. It is the
    frame-side counterpart to render/gui_render_mem.c, which does the same job for the render
    server's own memory.

    Most of a widget's per-instance state actually lives in malloc'd blocks that already get
    counted elsewhere as ordinary heap usage. What makes this particular bucket grow is mostly
    fonts: load a font with a wide character range and its resident bitmap shows up here.

    This file is also where gui_mem_stats / gui_print_mem_stats live -- the functions that add
    both servers' totals together into one final number a caller can print or log.

    It must be the LAST file included in the gui_frame.c unit: every line below measures the
    size of a static variable defined in a file included earlier, which only works if this file
    comes after all of them. Adding a new fixed-size global anywhere in the GUI? Add its sizeof
    here too.

    Not counted: small scalar statics (bools, counters, a stack depth, a hook pointer) -- not
    worth tracking individually -- and string literals, which the linker already pools.

==============================================================================================*/
// clang-format off

u32
gui_ui_memory( void )
{
    u32 b = 0;

    /* core/ -- THE INTERACT SERVER is its own unit (gui_core.c) and accounts for its
       own statics (ambient records, io snapshot, id/flag stacks, context pool) via its seam. */
    b += core_unit_mem_bytes();

    /* style/ -- THE STYLE UNIT is its own unit (gui_style.c) and accounts for its
       own statics (base/active style, theme table, stacks + pair tables) via its seam. */
    b += style_unit_mem_bytes();

    /* flow/ -- THE FLOW UNIT is its own unit (gui_flow.c) and accounts for its own
       statics (layout state pool, split stack, sublayout sink) via its seam. */
    b += flow_unit_mem_bytes();

    /* interact/ -- THE INTERACT UNIT is its own unit (gui_interact.c) and accounts
       for its own statics (the drag payload slot) via its seam; the text-selection controller
       moved to chrome (chrome/window/gui_select.c) and is counted there. */
    b += interact_unit_mem_bytes();

    /* component/ -- THE COMPONENT UNIT (widget logic, staging) accounts for its own statics
       (none yet) via its seam (gui_component.c). */
    b += component_unit_mem_bytes();

    /* stock/ -- THE STOCK UNIT (reference widget set) accounts for its own statics (the
       renders are stateless) via its seam (gui_stock.c). */
    b += stock_unit_mem_bytes();

    /* widgets/ + table/ + dock/ + popup/ -- the chrome unit accounts for its own statics
       (gui_chrome.c seam). */
    b += chrome_unit_mem_bytes();

    /* text/ -- THE TEXT LEAF (gui_font.c) accounts for the loaded-font registry via its seam:
       the slot table plus each font's resident bitmap and ext glyph records (heap that scales
       with what is loaded).  It moved down from the draw unit so both servers can measure text. */
    b += font_unit_mem_bytes();

    /* frame/ + root -- lifecycle stacks, DPI response state, boot/present state. */
    b += (u32)( sizeof( s_ctx_save_stack ) + sizeof( s_font_stack ) + sizeof( s_dpi )
              + sizeof( s_boot ) + sizeof( s_present ) );

    /* core/gui_ctx.c -- the one global viewport table (s_vp_pool).  A fixed static now, not part
       of any context's per-context heap block, so it is counted here rather than under
       cpu_context_bytes below. */
    b += (u32)sizeof( s_vp_pool );

    /* debug/ -- the perf/state HUD accumulators (always compiled; runtime-gated) live in THIS
       unit; the dashboard / stepper statics moved to the debug unit (gui_debug.c), which
       reports its own fixed footprint through the seam. */
    b += (u32)sizeof( s_perf );
    b += debug_unit_mem_bytes();

    return b;
}

/*==============================================================================================
    Memory Stats

    A full accounting of the gui system's resident footprint, split by where it lives (GPU device
    memory / fixed CPU .bss / per-context CPU heap).  The backend fills its own buckets through
    backend_memory (GPU buffers + the fixed backend buffers); this unit owns the aggregation,
    counting the live GPU surfaces to scale the geometry buffers and summing the per-context
    malloc blocks (core's pool, reached through the gui_ctx.h externs) for the heap total.
    See gui_mem_stats_t (gui.h) for the bucket meanings.
==============================================================================================*/

gui_mem_stats_t
gui_mem_stats( void )
{
    /* Count live surfaces in the one global viewport table so the backend can report per-surface
       costs against the true surface count. */
    u32 live_viewports = 0;
    for ( u32 v = 0; v < GUI_MAX_VIEWPORTS; ++v )
        if ( s_vp_pool[ v ].live )
            ++live_viewports;

    /* Backend fills GPU + CPU .bss; this unit adds the frontend statics, the CPU-heap context
       blocks, and the totals. */
    gui_mem_stats_t s = backend_memory( live_viewports );

    s.cpu_frontend_bytes = gui_ui_memory();
    s.cpu_static_total  += s.cpu_frontend_bytes;

    /* CPU heap: one malloc block per live context (recorded at allocation as _alloc_size), plus
       the atlas-owned heap the backend already filled in (cpu_atlas_bytes). */
    for ( u32 i = 0; i < s_ctx_pool_count; ++i )
    {
        gui_context_t* ctx = s_ctx_pool[ i ];
        if ( !ctx ) continue;
        s.cpu_context_bytes += ctx->_alloc_size;
        ++s.context_count;
    }
    s.cpu_dynamic_total = s.cpu_context_bytes + s.cpu_atlas_bytes;

    s.total_bytes = s.gpu_total + s.cpu_static_total + s.cpu_dynamic_total;
    return s;
}

/* Dump the full breakdown as a sectioned table: GPU / CPU static / CPU heap, each with its own
   subtotal, then the grand total.  Bytes and KiB side by side so small (font glyph tables) and
   large (geometry buffers) buckets are both legible at a glance.

   One gui_log per ROW rather than one per table: a sink frames per message, so a table emitted as
   a single blob would arrive as one unsplittable line.  Row-at-a-time costs nothing here (this is
   an explicitly requested dump, not a frame path) and keeps every line individually greppable. */
void
gui_print_mem_stats( void )
{
    gui_mem_stats_t s = gui_mem_stats();
    const f32 kb = 1024.0f;

    #define GUI_MEM_ROW( label, bytes ) \
        gui_log( GUI_LOG_INFO, "  %-22s %10u B  (%8.1f KB)", (label), (u32)(bytes), (u32)(bytes) / kb )

    gui_log( GUI_LOG_INFO, "memory usage -- full breakdown:" );

    gui_log( GUI_LOG_INFO, "  -- GPU device (%u live surface%s) ------------------------",
             s.viewport_count, s.viewport_count == 1u ? "" : "s" );
    GUI_MEM_ROW( "atlas textures",   s.gpu_texture_bytes );
    GUI_MEM_ROW( "clip + prim tables", s.gpu_table_bytes );
    if ( s.gpu_debug_bytes )
        GUI_MEM_ROW( "debug overlay buffers", s.gpu_debug_bytes );
    GUI_MEM_ROW( "  GPU subtotal",   s.gpu_total         );

    gui_log( GUI_LOG_INFO, "  -- CPU static (fixed backend buffers) ------------------" );
    GUI_MEM_ROW( "draw command list",  s.cpu_drawlist_bytes );
    GUI_MEM_ROW( "tessellation stage", s.cpu_tess_bytes     );
    GUI_MEM_ROW( "retained cache",     s.cpu_cache_bytes    );
    GUI_MEM_ROW( "icon + sprite tables", s.cpu_draw_bytes   );
    GUI_MEM_ROW( "atlas records",      s.cpu_res_bytes      );
    GUI_MEM_ROW( "render + shaders",   s.cpu_render_bytes   );
    GUI_MEM_ROW( "text-select capture", s.cpu_select_bytes  );
    if ( s.cpu_debug_bytes )
        GUI_MEM_ROW( "debug tooling",  s.cpu_debug_bytes    );
    GUI_MEM_ROW( "frontend statics",   s.cpu_frontend_bytes );
    GUI_MEM_ROW( "  CPU static subtotal", s.cpu_static_total );

    gui_log( GUI_LOG_INFO, "  -- CPU heap (%u context%s) -----------------------------",
             s.context_count, s.context_count == 1u ? "" : "s" );
    GUI_MEM_ROW( "context blocks",        s.cpu_context_bytes );
    GUI_MEM_ROW( "atlas mirrors + tenants", s.cpu_atlas_bytes );
    GUI_MEM_ROW( "  CPU heap subtotal",   s.cpu_dynamic_total );

    /* Atlas occupancy -- dims are live (the atlases grow under pressure); a row only prints for
       an atlas that exists. */
    {
        f32 pct; u32 tn, w, h;
        res_atlas_occupancy( &pct, &tn, &w, &h );
        gui_log( GUI_LOG_INFO, "  %-22s %ux%u   %u tenant%s   %.0f%% full",
                 "coverage atlas", w, h, tn, tn == 1u ? "" : "s", pct );
        res_sprite_occupancy( &pct, &tn, &w, &h );
        if ( w )
            gui_log( GUI_LOG_INFO, "  %-22s %ux%u   %u tenant%s   %.0f%% full",
                     "sprite atlas", w, h, tn, tn == 1u ? "" : "s", pct );
        res_sdf_occupancy( &pct, &tn, &w, &h );
        if ( w )
            gui_log( GUI_LOG_INFO, "  %-22s %ux%u   %u tenant%s   %.0f%% full",
                     "sdf atlas", w, h, tn, tn == 1u ? "" : "s", pct );
    }

    gui_log( GUI_LOG_INFO, "  --------------------------------------------------------" );
    gui_log( GUI_LOG_INFO, "  %-22s %10u B  (%8.1f KB)  (%.1f MB)",
             "TOTAL", s.total_bytes, s.total_bytes / kb, s.total_bytes / ( kb * kb ) );

    #undef GUI_MEM_ROW
}

// clang-format on
/*============================================================================================*/
