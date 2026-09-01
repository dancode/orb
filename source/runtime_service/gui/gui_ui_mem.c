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
       own statics (the context, ambient records, io snapshot, id/flag stacks) via its seam. */
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

    /* frame/ + root -- font stack, DPI response state, boot/present state. */
    b += (u32)( sizeof( s_font_stack ) + sizeof( s_dpi )
              + sizeof( s_boot ) + sizeof( s_present ) );

    /* core/gui_ctx.c -- the one global viewport table (s_vp_pool). */
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
    memory / fixed CPU .bss / CPU heap).  The backend fills its own buckets through backend_memory
    (GPU buffers + the fixed backend buffers); this unit owns the aggregation, counting the live
    GPU surfaces to scale the geometry buffers.  The only heap the gui holds is the atlases'.
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

    /* Backend fills GPU + CPU .bss; this unit adds the frontend statics and the totals. */
    gui_mem_stats_t s = backend_memory( live_viewports );

    s.cpu_frontend_bytes = gui_ui_memory();
    s.cpu_static_total  += s.cpu_frontend_bytes;

    /* CPU heap: the atlas-owned heap the backend already filled in (cpu_atlas_bytes). */
    s.cpu_dynamic_total = s.cpu_atlas_bytes;

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

    gui_log( GUI_LOG_INFO, "  -- GPU device (%u live surface%s) --------",
             s.viewport_count, s.viewport_count == 1u ? "" : "s" );
    /* Atlas textures broken out per texture, since which one grew is the whole question when this
       total moves.  A lazily created atlas prints nothing rather than a zero row. */
    GUI_MEM_ROW( "atlas textures",     s.gpu_texture_bytes );
    GUI_MEM_ROW( "    coverage",       s.gpu_tex_cov_bytes );
    if ( s.gpu_tex_sdf_bytes )
        GUI_MEM_ROW( "    sdf",        s.gpu_tex_sdf_bytes );
    if ( s.gpu_tex_spr_bytes )
        GUI_MEM_ROW( "    sprite",     s.gpu_tex_spr_bytes );
    /* The tables broken out: quad/prim are claim-sized (live capacity x frame-in-flight copies,
       grown on demand), clip carries a full set of window slabs per frame-in-flight
       (window-keyed, not viewport-keyed). */
    GUI_MEM_ROW( "  quad records",   s.gpu_quad_bytes  );
    GUI_MEM_ROW( "  prim records",   s.gpu_prim_bytes );
    GUI_MEM_ROW( "  clip entries",   s.gpu_clip_bytes  );
    GUI_MEM_ROW( "  glyph uv table", s.gpu_glyph_bytes );
    if ( s.gpu_debug_bytes )
        GUI_MEM_ROW( "debug overlay buffers", s.gpu_debug_bytes );
    GUI_MEM_ROW( "  GPU subtotal",   s.gpu_total         );

    gui_log( GUI_LOG_INFO, "  -- CPU static (fixed backend buffers) ------------------" );
    GUI_MEM_ROW( "draw command list",  s.cpu_drawlist_bytes );
    GUI_MEM_ROW( "quad + prim arenas", s.cpu_tess_bytes    );
    GUI_MEM_ROW( "retained cache",     s.cpu_cache_bytes    );
    GUI_MEM_ROW( "icon + sprite tables", s.cpu_draw_bytes   );
    GUI_MEM_ROW( "atlas records",      s.cpu_res_bytes      );
    GUI_MEM_ROW( "render state",       s.cpu_render_bytes   );
    GUI_MEM_ROW( "text-select capture", s.cpu_select_bytes  );
    if ( s.cpu_debug_bytes )
        GUI_MEM_ROW( "debug tooling",  s.cpu_debug_bytes    );
    GUI_MEM_ROW( "frontend statics",   s.cpu_frontend_bytes );
    GUI_MEM_ROW( "  CPU static subtotal", s.cpu_static_total );

    gui_log( GUI_LOG_INFO, "  -- CPU heap ------------------------------------------" );
    /* Split the same way as the textures above: an atlas's mirror tracks its texture, and its
       tenants' retained sources sit on top, so each row here runs ahead of its GPU twin. */
    GUI_MEM_ROW( "atlas mirrors + tenants", s.cpu_atlas_bytes );
    GUI_MEM_ROW( "    coverage",          s.cpu_atlas_cov_bytes );
    if ( s.cpu_atlas_sdf_bytes )
        GUI_MEM_ROW( "    sdf",           s.cpu_atlas_sdf_bytes );
    if ( s.cpu_atlas_spr_bytes )
        GUI_MEM_ROW( "    sprite",        s.cpu_atlas_spr_bytes );
    GUI_MEM_ROW( "  CPU heap subtotal",   s.cpu_dynamic_total );

    /* Atlas occupancy -- dims are live (the atlases grow under pressure); a row only prints for an
       atlas that exists.  Under each, the packed cell area attributed to what registered it.  That
       attribution is the second axis of the same question: a kind cuts ACROSS the atlases (an icon
       is a coverage tenant or an SDF one depending only on how it was baked), so a texture row
       says which atlas grew and these say what put it there.  Cells, not raw tenant pixels, so
       they read against the percent on the line above rather than against the mirror's bytes. */
    {
        f32 pct; u32 tn, w, h;
        u32 kind[ RES_TENANT_KIND_COUNT ];

        #define GUI_MEM_ATLAS( label, occupancy_fn, kind_fn )                             \
            do {                                                                          \
                ( occupancy_fn )( &pct, &tn, &w, &h );                                    \
                if ( !w ) break;                                                          \
                gui_log( GUI_LOG_INFO, "  %-22s %ux%u   %u tenant%s   %.0f%% full",       \
                         ( label ), w, h, tn, tn == 1u ? "" : "s", pct );                 \
                ( kind_fn )( kind );                                                      \
                for ( u32 k = 0; k < RES_TENANT_KIND_COUNT; ++k )                         \
                    if ( kind[ k ] )                                                      \
                        gui_log( GUI_LOG_INFO, "      %-18s %10u B  (%8.1f KB)",          \
                                 res_tenant_kind_name( (res_tenant_kind_t)k ),            \
                                 kind[ k ], (f32)kind[ k ] / kb );                        \
            } while ( 0 )

        GUI_MEM_ATLAS( "coverage atlas", res_atlas_occupancy,  res_atlas_kind_bytes  );
        GUI_MEM_ATLAS( "sprite atlas",   res_sprite_occupancy, res_sprite_kind_bytes );
        GUI_MEM_ATLAS( "sdf atlas",      res_sdf_occupancy,    res_sdf_kind_bytes    );

        #undef GUI_MEM_ATLAS
    }

    /* How full the caps behind those buckets have actually been -- the other half of the question
       (backend_pool_report, render/gui_render_mem.c).  Printed here because the two are read
       together: a bucket is only worth shrinking if its pool never fills, and only worth growing
       if it does. */
    backend_pool_report();

    gui_log( GUI_LOG_INFO, "  --------------------------------------------------------" );
    gui_log( GUI_LOG_INFO, "  %-22s %10u B  (%8.1f KB)  (%.1f MB)",
             "TOTAL", s.total_bytes, s.total_bytes / kb, s.total_bytes / ( kb * kb ) );

    #undef GUI_MEM_ROW
}

// clang-format on
/*============================================================================================*/
