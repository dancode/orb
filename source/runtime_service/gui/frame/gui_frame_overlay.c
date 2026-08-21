/*==============================================================================================

    gui/frame/gui_frame_overlay.c -- debug overlays and performance readouts.

    Panels drawn through the ordinary GUI pipeline, so no separate host UI code is needed.
    The host's only job is to call debug_enable() and give it a clock and idle-wait function
    through frame_set_hooks().

    Press '.' to toggle the debug overlay once it's enabled.

==============================================================================================*/
// clang-format off

/*==============================================================================================
    Performance Overlay 
    
    Built-in FPS / cost readout, no host code required.

    The gui owns no clock so the host supplies a monotonic seconds callback via,
    frame_set_hooks(); and gui brackets the frame with it.

    Three clocks summing to the whole CPU frame:

    - emit:    frame_begin -> frame_end (the UI build cost).
    - render:  sum of every render() flush this frame -- one call per viewport (the main surface
               plus every open floater, each its own rhi_ctx), accumulated into a single total.
    - present: (boot path only) the present pair's wall time minus render -- the non-render
               overhead, dominated by the frame_begin fence wait (GPU backpressure).
    
    All three are smoothed (EMA) into the readout at the NEXT frame_begin, so the panel trails
    the work it describes by one frame -- unavoidable self-measurement lag.

==============================================================================================*/

static struct
{
    gui_clock_fn    clock;              // host monotonic seconds source (NULL = timing off)

    f64             t_loop_start;       // clock() captured at boot_poll entry (0 = not armed)
    f64             t_emit_start;       // clock() captured at frame_begin (0 = not armed)
    f64             t_present_start;    // clock() captured at present_begin entry (0 = not armed)

    f64             emit_ms;            // this frame: frame_begin -> frame_end (ms)
    f64             rend_ms;            // this frame: accumulated render() wall time (ms)
    f64             pres_ms;            // this frame: present pair wall minus render (ms)

    f32             fps;                // smoothed readouts shown by the overlay
    f32             s_poll_ms;          // smooth time boot_poll      -> boot_present_begin
    f32             s_emit_ms;          // smooth time frame_begin    -> frame_end
    f32             s_rend_ms;          // smooth time first render() -> boot_present_end
    f32             s_pres_ms;          // smooth time present_begin  -> boot_present_end minus render
    f32             s_wait_ms;          // smooth time present_end    -> boot_poll

    /* The render server's own phase split, smoothed on the same curve as s_rend_ms so the rows
       can be read against it.  Sourced from gui_render_stats(), NOT measured here. */
    f32             s_diff_ms;          // smooth BUILD step 1 -- hash + diff
    f32             s_tess_ms;          // smooth BUILD step 2 -- reuse or tessellate
    f32             s_submit_ms;        // smooth SUBMIT -- uploads + draw calls
    f32             s_gpu_ms;           // smooth GPU execution (timestamped on the GPU, ~2 frames latent)

    bool            emit_captured;      // emit_ms latched at frame_end this frame

} s_perf;

/*==============================================================================================

    * Publish last frame's clock times into the smoothed readouts from gui()->frame_begin()
    * Start a fresh emit clock for the next frame. 

==============================================================================================*/

static void
perf_frame_begin( f32 dt )
{
    if ( dt > 0.0f )
    {
        f32 inst = 1.0f / dt; // instantaneous FPS this frame
        s_perf.fps = s_perf.fps <= 0.0f ? inst : s_perf.fps * 0.92f + inst * 0.08f;
    }
    f32 em = (f32)s_perf.emit_ms;
    f32 rm = (f32)s_perf.rend_ms;
    f32 pm = (f32)s_perf.pres_ms;

    s_perf.s_emit_ms = s_perf.s_emit_ms <= 0.0f ? em : s_perf.s_emit_ms * 0.9f + em * 0.1f;
    s_perf.s_rend_ms = s_perf.s_rend_ms <= 0.0f ? rm : s_perf.s_rend_ms * 0.9f + rm * 0.1f;
    s_perf.s_pres_ms = s_perf.s_pres_ms <= 0.0f ? pm : s_perf.s_pres_ms * 0.9f + pm * 0.1f;

    s_perf.emit_ms         = 0.0;
    s_perf.rend_ms         = 0.0;
    s_perf.pres_ms         = 0.0;
    s_perf.emit_captured   = false;
    s_perf.t_present_start = 0.0;
    s_perf.t_emit_start    = s_perf.clock ? s_perf.clock() : 0.0;
}

/*==============================================================================================

    * Fold the render server's phase split into the smoothed readouts.
    * Called from frame_begin AFTER build_stats_publish, which is what makes these the SAME frame
      the s_rend_ms published just above describes -- read before it and every row would trail the
      total it sits under by one more frame.

==============================================================================================*/

static void
perf_zones_publish( void )
{
    gui_render_stats_t rs = gui_render_stats();

    s_perf.s_diff_ms   = s_perf.s_diff_ms   <= 0.0f ? rs.diff_ms
                                                    : s_perf.s_diff_ms   * 0.9f + rs.diff_ms   * 0.1f;
    s_perf.s_tess_ms   = s_perf.s_tess_ms   <= 0.0f ? rs.tess_ms
                                                    : s_perf.s_tess_ms   * 0.9f + rs.tess_ms   * 0.1f;
    s_perf.s_submit_ms = s_perf.s_submit_ms <= 0.0f ? rs.submit_ms
                                                    : s_perf.s_submit_ms * 0.9f + rs.submit_ms * 0.1f;
    s_perf.s_gpu_ms    = s_perf.s_gpu_ms    <= 0.0f ? rs.gpu_ms
                                                    : s_perf.s_gpu_ms    * 0.9f + rs.gpu_ms    * 0.1f;
}

/*==============================================================================================

    * Close out the emit phase at gui()->frame_end()

==============================================================================================*/

static void
perf_frame_end( void )
{
    if ( !s_perf.clock )
         return;
    
    if ( !s_perf.emit_captured && s_perf.t_emit_start > 0.0 )
    {
        s_perf.emit_ms       = ( s_perf.clock() - s_perf.t_emit_start ) * 1000.0;
        s_perf.emit_captured = true;
    }
}

/*==============================================================================================

    * Render bracket -- render-tier only.
    * An ACCUMULATOR, not a single span: gui_render() is called once per viewport (the main
      surface plus every open floater, each its own rhi_ctx) -- every call this frame adds into
      rend_ms instead of overwriting it, so the readout is the frame's total render cost across
      every context, not any one of them.
    * Returns 0 if no timers is active.

==============================================================================================*/

static f64
perf_render_begin( void )
{
    return s_perf.clock ? s_perf.clock() : 0.0;
}

static void
perf_render_end( f64 t0 )
{
    if ( s_perf.clock && t0 > 0.0 )
         s_perf.rend_ms += ( s_perf.clock() - t0 ) * 1000.0;
}

/*==============================================================================================

    * Present bracket -- boot-tier only.
    * Returns 0 if no timers is active.

==============================================================================================*/

static void
perf_present_begin( void )
{
    s_perf.t_present_start = s_perf.clock ? s_perf.clock() : 0.0;
}

static void
perf_present_end( void )
{
    if ( !s_perf.clock || s_perf.t_present_start <= 0.0 )
         return;

    f64 span = ( s_perf.clock() - s_perf.t_present_start ) * 1000.0 - s_perf.rend_ms;
    s_perf.pres_ms = span > 0.0 ? span : 0.0;   /* clamp: render flush can't exceed the pair span */
}

/*==============================================================================================

    * Span bracket -- generic single-span timer, shared by the poll and wait brackets below.
    * Returns 0 if no timer is active.
    * End folds the span from t0 -> now into *dst via EMA.
    * Used in gui_boot_poll, and gui_boot_pace.

==============================================================================================*/

static f64
perf_span_begin( void )
{
    return s_perf.clock ? s_perf.clock() : 0.0;
}

static void
perf_span_end( f32* dst, f64 t0 )
{
    if ( !s_perf.clock || t0 <= 0.0 )
        return;

    f32 ms = ( f32 )( ( s_perf.clock() - t0 ) * 1000.0 );
    *dst = ( *dst <= 0.0f ) ? ms : *dst * 0.9f + ms * 0.1f;
}

/*==============================================================================================

    * Debug Overlay -- the built-in performance readout, no host code required.
    * Used by both performance and state overlay.

==============================================================================================*/

/* Debug HUD column origins, left to right.  Every HUD is an autosized region pinned to a fixed x
   so any combination can be up at once without coordinating widths at runtime; the gaps are sized
   from each one's widest row.  They live together here because moving one means checking the next.

       perf    the FPS / timings / counts ladder -- narrow, a value column of "12/34"
       memory  byte breakdown -- wider, a padded label column plus a size
       state   interaction ids -- its widest tier rows run to about +200
       fonts   the loaded-font registry, one row per slot (rarely open) */

#define OVL_COL_PERF    8.0f
#define OVL_COL_MEMORY  224.0f
#define OVL_COL_STATE   404.0f
#define OVL_COL_FONTS   660.0f

/* Every HUD shares one top edge, so they read as one instrument row. */
#define OVL_TOP_PAD     32.0f

static void
overlay_backdrop( void )
{
    /* Called as the first statement inside a region's body, right after gui_region_begin, so
       lf() is still that region's frame -- outer is the exact box it resolved this frame
       (fixed, autosized to content, or user-resized via GUI_WIN_CHILD_RESIZE_X/_Y).  Reading
       the box directly, rather than re-deriving a size from the region's measured content,
       keeps this correct once a region is resizeable: a dragged box need not match its
       content's extent, so the two can no longer be assumed equal.  gui_region_begin never
       resolves a zero-size box, so no positive-size guard is needed here. */

    gui_rect_t box = lf()->outer;
    gui_draw_rect( box.x, box.y, box.w, box.h, GUI_COLOR( 0x10, 0x10, 0x14, 0xFF ));
}

/* Bytes as a compact figure -- "912 B", "14.2 KB", "3.14 MB".  Round-robins a few static buffers
   so several sizes can go into one gui_textf without clobbering each other, the same shape as
   overlay_id_str below.  Every byte figure in the HUD carries its unit: the rows around it count
   entries, and a bare five-digit number in either column would be unreadable. */
static const char*
overlay_bytes( u32 b )
{
    static char bufs[ 4 ][ 16 ];
    static u32  next = 0;

    char* s = bufs[ next ];
    next    = ( next + 1u ) & 3u;

    if ( b < 1024u )              fmt_snprintf( s, sizeof( bufs[ 0 ] ), "%u B",    b );
    else if ( b < 1024u * 1024u ) fmt_snprintf( s, sizeof( bufs[ 0 ] ), "%.1f KB", (f32)b / 1024.0f );
    else                          fmt_snprintf( s, sizeof( bufs[ 0 ] ), "%.2f MB",
                                                (f32)b / ( 1024.0f * 1024.0f ) );
    return s;
}

static void
overlay_perf( int mode )
{
    if ( mode <= 0 )
        return;

    f32 fps = s_perf.fps;

    f32 top_y  = gui_viewport_content_y( 0 ) + OVL_TOP_PAD;
    f32 left_x = OVL_COL_PERF;

    /* A root region, autosized to its content (w/h <= 0), fixed top-left.
       - NOSCROLL/NO_INPUT: pure text readout -- shouldn't grab hover or eat the mouse wheel.
       - DEBUG_BAND: exempt from its own stats -- the digits it draws must not count toward the
         numbers it displays, or poison idle-skip.
       - GUI_REGION_FG: foreground band, above every popup and modal (e.g. the dev console) --
         a debug readout you can't see is useless. */

    gui_region_begin( "perf_overlay", left_x, top_y, 0.0f, 0.0f, GUI_REGION_FG, GUI_VP_MAIN,
                     GUI_WIN_DEBUG_BAND | GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT | GUI_WIN_ALWAYS_AUTOSIZE );
    {
        overlay_backdrop();

        gui_stack();
        gui_scale_push( GUI_SCALE_DENSE );   /* tight row pitch -- a HUD, not a form */

        /* FPS, graded by health: >=60 green, >=30 amber, else red. */
        u32 fps_col = fps >= 60.0f ? GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )
                    : fps >= 30.0f ? GUI_COLOR( 0xE0, 0xC0, 0x40, 0xFF )
                    :                GUI_COLOR( 0xEE, 0x55, 0x44, 0xFF );
        
        UNUSED( fps );
        UNUSED( fps_col );

        char line[ 64 ];
        fmt_snprintf( line, sizeof( line ), "FPS %5.1f  (%4.2f ms)", fps, fps > 0.0f ? 1000.0f / fps : 0.0f );
        gui_text_colored( fps_col, line );
        
        bool show_timing_rows = ( mode >= 2 );
        if ( show_timing_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "emit    %5.2f ms", s_perf.s_emit_ms );
            gui_textf( "render  %5.2f ms", s_perf.s_rend_ms );

            /* Where the render time went, indented as its children.  They do NOT sum to it: the
               debug overlay's own flush and the per-surface setup around the zones sit outside all
               three, and diff/tess hold the last REAL frame's cost while an idle frame still pays
               submit.  A gap between the sum and the parent IS the reading -- it is the part of
               render that is neither building geometry nor handing it to the GPU.
                 - diff:   hash every emitted command, diff every window (what retention costs)
                 - tess:   reuse or tessellate every changed window (what retention saves)
                 - submit: uploads + draw-call recording, every surface, every frame */
            gui_textf( "  diff  %5.2f ms", s_perf.s_diff_ms );
            gui_textf( "  tess  %5.2f ms", s_perf.s_tess_ms );
            gui_textf( "  submit %4.2f ms", s_perf.s_submit_ms );

            /* The GPU's own clock, apart from the CPU children above: what the recorded frame
               cost to EXECUTE, from the rhi's timestamp pair.  Not a child of render -- it runs
               ~2 frames behind and overlaps the CPU rows in wall time. */
            gui_textf( "gpu     %5.2f ms", s_perf.s_gpu_ms );

            /* Full loop breakdown -- tier 2 only. Tiers 3+ swap this for geometry/pool stats,
               where these fence/sleep numbers are just noise.
               - present: non-render overhead (fence wait + acquire + submit + present)
               - poll:    OS pump + input
               - wait:    boot_pace sleep / idle -- the loop's sleep, made visible instead of hidden
               total sums all five and should track the FPS ms above (residual = loop arithmetic +
               one frame of self-measurement lag). */
        
            if ( mode == 2 )
            {
                gui_textf( "present %5.2f ms", s_perf.s_pres_ms );
                gui_textf( "poll    %5.2f ms", s_perf.s_poll_ms );
                gui_textf( "wait    %5.2f ms", s_perf.s_wait_ms );
                gui_textf( "total   %5.2f ms", s_perf.s_emit_ms + s_perf.s_rend_ms
                                             + s_perf.s_pres_ms + s_perf.s_poll_ms
                                             + s_perf.s_wait_ms );
            }
        }
        
        bool show_geometry_rows = ( mode >= 3 );
        gui_render_stats_t rs = gui_render_stats();
        if ( show_geometry_rows )
        {
            /* APPLICATION cost -- the debug band (this overlay included) is netted out, so these
               are the numbers a real UI is answerable for.  A quad record IS one shape; the
               rasterizer sees quads * 2 triangles, and the draw asks for quads * 6 vertices. */
            gui_new_line( 2.0f );
            gui_textf( "quads   %6u", rs.quad_count );
            gui_textf( "tris    %6u", rs.quad_count * 2u );
            gui_textf( "styles  %6u", rs.prim_count );
            gui_textf( "batches %6u", rs.draw_calls );
            gui_textf( "cmds    %6u", rs.cmd_count  );
            gui_textf( "clips   %6u", rs.clip_count );

            bool show_retained_rows = ( mode >= 4 );
            if ( show_retained_rows )
            {
                /* Retained-mode stats: geometry reused vs re-tessellated. vol patch is separate
                   from wins ret above -- a window with an animating volatile widget
                   (gui()->volatile_cb) still counts as fully retained; this is what moved inside
                   it this frame. */

                gui_new_line( 2.0f );
                gui_textf( "wins ret  %u/%u", rs.win_retained,  rs.win_total   );
                gui_textf( "quads ret %u/%u", rs.quad_retained, rs.quad_count  );
                gui_textf( "vol patch %u",    rs.volatile_patched              );
                gui_textf( "vol rows  %u/%u", volatile_row_count(), GUI_MAX_VOLATILE );
                
                /* Upload stats: GPU memory bandwidth. */

                gui_new_line( 2.0f );
                gui_textf( "up batch  %u", rs.upload_batches );
                gui_textf( "up bytes  %u", rs.upload_bytes   );
            
                /* Keyed state pool load per class: live (touched within a frame) / occupied
                   (live + unreclaimed tombstones) / capacity.  The partition-tuning metric. */

                gui_state_usage_t su = gui_state_usage();
                gui_new_line( 2.0f );
                gui_textf( "st tiny  %u/%u/%u", su.tiny_live,  su.tiny_used,  su.tiny_cap  );
                gui_textf( "st small %u/%u/%u", su.small_live, su.small_used, su.small_cap );
                gui_textf( "st big   %u/%u/%u", su.big_live,   su.big_used,   su.big_cap   );
            
                /* Fixed-pool pressure: used vs cap.  PHYSICAL fill (_all: both bands, this
                   overlay included) -- the pools are shared, so overflow risk is physical and a
                   band-netted number would hide it.  These pools fail silently when full --
                   watch for caps under load and raise them BEFORE labels drop, clips break, or
                   nav items fall off the list. nav is this frame's live count (the overlay emits
                   last, after every window has registered). */
                gui_new_line( 2.0f );
                gui_textf( "quads  %u/%u", rs.quad_count_all, (u32)GUI_MAX_QUADS      );
                gui_textf( "styles %u/%u", rs.prim_count_all, (u32)GUI_MAX_PRIMS      );
                gui_textf( "cmds   %u/%u", rs.cmd_count_all,  (u32)GUI_MAX_CMDS       );
                gui_textf( "segs   %u/%u", rs.seg_count,      (u32)GUI_MAX_SEGS       );
                gui_textf( "clips  %u/%u", rs.clip_count_all, (u32)GUI_MAX_CLIP_RECTS );
                gui_textf( "text   %u/%u", rs.text_pool_used, (u32)GUI_MAX_TEXT_POOL  );
                gui_textf( "nav    %u/%u", g_ctx->nav.item_count, (u32)GUI_NAV_ITEMS_MAX );
            }
        }
        
        /* Debug-lever status (mode >= 3): the live emit / tessellation / pacing toggles, so you
           don't need the console log to know which regime the numberss above were measured under.
           Toggled from the selector menu (right edge of viewport), not their own hotkey.
           Fixed-width states keep the row from resizing. */
        bool show_status_rows = ( mode >= 3 );
        if ( show_status_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "emit  %s", gui_force_redraw()        ? "forced  " : "on-dirty" );
            gui_textf( "tess  %s", build_retained_skip() ? "cached  " : "always  " );
            gui_textf( "pace  %s", gui_idle_skip()           ? "idleskip" : "spin    " );
        }

        gui_scale_pop();
    }
    gui_region_end();
}

/*==============================================================================================
    Memory overlay -- where the gui's bytes are.

    Its own HUD rather than a perf tier: the perf ladder answers "how fast", this answers "how
    big", and by its top tier that ladder is already the tallest thing on screen.  Its own region
    also buys the room to answer the question a byte figure always raises next -- WHAT is that size
    made of -- so every row carries the count and stride behind it rather than a bare total.

    Four sections, each a coloured heading followed by its rows and closed by a rule, narrowest
    scope first:

      FRAME       what THIS frame built and pushed.  The only rows a widget edit moves.
      GPU         device memory, and the only place a raised GUI_MAX_* cap gets expensive: the
                  quad, style and clip tables each hold a full set of entries per (frame-in-flight,
                  viewport), so their cost is cap x stride x regions.
      CPU STATIC  fixed buffers baked into the image -- paid whether one window is open or fifty.
      CPU HEAP    what actually grows at runtime: one block per live context, plus the atlases'
                  staging mirrors and their tenants' retained source copies.

    Every row is "label / detail / size", and the label column is space-padded to a fixed width so
    the sizes line up down the panel.  gui_mem_stats walks the atlases' tenant lists, so this is a
    HUD the reader opts into (the selector menu's Memory checkbox), never one riding the FPS line.
==============================================================================================*/

/* Section heads, one hue each, so a glance lands in the right block without reading the labels. */
#define OVL_MEM_FRAME_COL  GUI_COLOR( 0xE0, 0xC0, 0x40, 0xFF )   /* amber  -- per-frame  */
#define OVL_MEM_GPU_COL    GUI_COLOR( 0x66, 0xDD, 0x55, 0xFF )   /* green  -- device     */
#define OVL_MEM_CPU_COL    GUI_COLOR( 0x66, 0xBB, 0xEE, 0xFF )   /* blue   -- .bss       */
#define OVL_MEM_HEAP_COL   GUI_COLOR( 0xC8, 0x8A, 0xE8, 0xFF )   /* purple -- malloc     */
#define OVL_MEM_TOTAL_COL  GUI_COLOR( 0xF0, 0xF0, 0xF0, 0xFF )   /* white  -- the sum    */

/* A section heading: "NAME .......... size" in the section's colour.  The size on the heading is
   that section's subtotal, so the four headings alone read as the whole breakdown. */
static void
overlay_mem_head( u32 col, const char* name, u32 bytes )
{
    char line[ 48 ];
    fmt_snprintf( line, sizeof( line ), "%-11s %s", name, overlay_bytes( bytes ) );
    gui_text_colored( col, line );
}

/* One detail row: a padded label, the count/stride that produces the size, then the size.  `detail`
   may be "" for a bucket with nothing to decompose. */
static void
overlay_mem_row( const char* label, const char* detail, u32 bytes )
{
    gui_textf( "  %-9s %-12s %s", label, detail, overlay_bytes( bytes ) );
}

static void
overlay_memory( void )
{
    /* Second column of the HUD row, immediately right of perf (see the column map above). */
    f32 top_y = gui_viewport_content_y( 0 ) + OVL_TOP_PAD;

    gui_region_begin( "mem_overlay", OVL_COL_MEMORY, top_y, 0.0f, 0.0f, GUI_REGION_FG, GUI_VP_MAIN,
                      GUI_WIN_DEBUG_BAND | GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT
                      | GUI_WIN_ALWAYS_AUTOSIZE );
    {
        overlay_backdrop();

        gui_stack();
        gui_scale_push( GUI_SCALE_DENSE );

        gui_render_stats_t rs = gui_render_stats();
        gui_mem_stats_t    ms = gui_mem_stats();

        char det[ 24 ];

        /* FRAME.  The arena fills here are PHYSICAL (both bands, slot padding included) because
           that is what the bytes actually are; the app-only counts live on the perf HUD. */
        u32 quad_b = rs.quad_count_all * (u32)GUI_QUAD_BYTES;
        u32 prim_b = rs.prim_count_all * (u32)GUI_PRIM_BYTES;
        overlay_mem_head( OVL_MEM_FRAME_COL, "FRAME", quad_b + prim_b );
        fmt_snprintf( det, sizeof( det ), "%u x %u B", rs.quad_count_all, (u32)GUI_QUAD_BYTES );
        overlay_mem_row( "quads", det, quad_b );
        fmt_snprintf( det, sizeof( det ), "%u x %u B", rs.prim_count_all, (u32)GUI_PRIM_BYTES );
        overlay_mem_row( "styles", det, prim_b );
        overlay_mem_row( "text", "", rs.text_pool_used );
        fmt_snprintf( det, sizeof( det ), "%u writes", rs.upload_batches );
        overlay_mem_row( "uploaded", det, rs.upload_bytes );
        gui_separator();

        /* GPU.  cap x stride x regions for the three regioned tables; the glyph table is ONE shared
           copy, replaced rather than rewritten, so it shows no region multiplier. */
        overlay_mem_head( OVL_MEM_GPU_COL, "GPU", ms.gpu_total );
        fmt_snprintf( det, sizeof( det ), "%u x %u reg", (u32)GUI_MAX_QUADS, ms.gpu_regions );
        overlay_mem_row( "quad tbl", det, ms.gpu_quad_bytes );
        fmt_snprintf( det, sizeof( det ), "%u x %u reg", (u32)GUI_MAX_PRIMS, ms.gpu_regions );
        overlay_mem_row( "style tbl", det, ms.gpu_style_bytes );
        fmt_snprintf( det, sizeof( det ), "%u x %u reg",
                      (u32)( RENDER_MAX_WIN * GUI_WIN_CLIP_MAX ), ms.gpu_regions );
        overlay_mem_row( "clip tbl", det, ms.gpu_clip_bytes );
        fmt_snprintf( det, sizeof( det ), "%u ids", (u32)GUI_GLYPH_TABLE_MAX );
        overlay_mem_row( "glyph tbl", det, ms.gpu_glyph_bytes );

        /* Atlas rows carry live dimensions and tenant count -- the atlases GROW under pressure, so
           the size is a measurement, not a constant.  A never-created atlas reports zero dims. */
        {
            f32 pct; u32 tn, w, h;
            res_atlas_occupancy( &pct, &tn, &w, &h );
            if ( w )
            {
                fmt_snprintf( det, sizeof( det ), "%ux%u %.0f%%", w, h, pct );
                overlay_mem_row( "cov atlas", det, w * h );
            }
            res_sprite_occupancy( &pct, &tn, &w, &h );
            if ( w )
            {
                fmt_snprintf( det, sizeof( det ), "%ux%u %.0f%%", w, h, pct );
                overlay_mem_row( "spr atlas", det, w * h * 4u );
            }
            res_sdf_occupancy( &pct, &tn, &w, &h );
            if ( w )
            {
                fmt_snprintf( det, sizeof( det ), "%ux%u %.0f%%", w, h, pct );
                overlay_mem_row( "sdf atlas", det, w * h );
            }
        }
        if ( ms.gpu_debug_bytes )
            overlay_mem_row( "dbg bufs", "", ms.gpu_debug_bytes );
        gui_separator();

        /* CPU STATIC -- the same buckets gui_print_mem_stats logs, so the HUD and the dump read
           the same way. */
        overlay_mem_head( OVL_MEM_CPU_COL, "CPU STATIC", ms.cpu_static_total );
        overlay_mem_row( "draw list",  "", ms.cpu_drawlist_bytes );
        overlay_mem_row( "arenas",     "", ms.cpu_tess_bytes     );
        overlay_mem_row( "cache",      "", ms.cpu_cache_bytes    );
        overlay_mem_row( "render",     "", ms.cpu_render_bytes   );
        overlay_mem_row( "frontend",   "", ms.cpu_frontend_bytes );
        if ( ms.cpu_debug_bytes )
            overlay_mem_row( "debug", "", ms.cpu_debug_bytes );
        gui_separator();

        overlay_mem_head( OVL_MEM_HEAP_COL, "CPU HEAP", ms.cpu_dynamic_total );
        fmt_snprintf( det, sizeof( det ), "%u live", ms.context_count );
        overlay_mem_row( "contexts", det, ms.cpu_context_bytes );
        overlay_mem_row( "atlas cpu", "", ms.cpu_atlas_bytes );
        gui_separator();

        overlay_mem_head( OVL_MEM_TOTAL_COL, "TOTAL", ms.total_bytes );

        gui_scale_pop();
    }
    gui_region_end();
}

/*==============================================================================================
    State overlay

    Text readout of live interaction state: hover/active/focused widget, hover window, keyboard
    nav cursor. Ids resolve to their source label/title via the id name registry (gui_debug_name,
    gui_debug_overlay.c) instead of a raw hash.

    Debug builds populate that registry at every id mint point (DBG_NAME in item_id /
    window_begin_ex / region / child / table). Release builds leave it empty, so every id shows
    as hex -- same always-available-but-more-useful-in-Debug shape as perf_overlay.
==============================================================================================*/

/* id -> "name" or "0x########" -- round-robins through a few static scratch buffers so multiple
   ids can be formatted into the same gui_textf() call without clobbering each other. */
static const char*
overlay_id_str( gui_id_t id )
{
    static char   bufs[ 4 ][ 24 ];
    static u32    next = 0;

    if ( id == GUI_ID_NONE ) return "-";

    const char* name = gui_debug_name( id );
    if ( name ) return name;

    char* b = bufs[ next ];
    next    = ( next + 1u ) & 3u;
    fmt_snprintf( b, sizeof( bufs[ 0 ] ), "0x%08X", id );
    return b;
}

/*============================================================================================*/

static void
overlay_state( int mode )
{
    if ( mode <= 0 )
        return;

    /* Third column of the HUD row (see the column map above); GUI_REGION_FG puts it over popups
       and the modal console, same as perf_overlay. */
    f32 top_y = gui_viewport_content_y( 0 ) + OVL_TOP_PAD;

    gui_region_begin( "state_overlay", OVL_COL_STATE, top_y, 0.0f, 0.0f, GUI_REGION_FG, GUI_VP_MAIN,
                      GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT );
    {
        overlay_backdrop();
        gui_stack();
        gui_scale_push( GUI_SCALE_DENSE );   /* tight row pitch -- a HUD, not a form */

        gui_textf( "Hover   %s", overlay_id_str( s_interaction.hover_id ) );
        gui_textf( "Active  %s (btn %u)", overlay_id_str( s_interaction.active_id ), s_interaction.active_button );
        gui_textf( "Window  %s", overlay_id_str( s_interaction.hover_win ) );

        bool show_extended_rows = ( mode >= 2 );
        if ( show_extended_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "Focused %s", overlay_id_str( s_interaction.focused_id ) );
            gui_textf( "Nav id  %s", overlay_id_str( g_ctx->nav.id ) );
            gui_textf( "Nav win %s", overlay_id_str( g_ctx->nav.win ) );
            gui_textf( "Mouse   %6.1f, %6.1f", s_io.mouse_x, s_io.mouse_y );
        }

        bool show_popup_rows = ( mode >= 3 );
        if ( show_popup_rows )
        {
            gui_new_line( 2.0f );
            gui_textf( "Popups  %u", g_ctx->popup.open_count );
            if ( g_ctx->popup.open_count )
            {
                gui_id_t top_popup = g_ctx->popup.open[ g_ctx->popup.open_count - 1u ].id;
                gui_textf( "Top pop %s", overlay_id_str( top_popup ) );
            }
            gui_textf( "Ctx salt 0x%08X", g_ctx->retained.id_salt );
        }

        gui_scale_pop();
    }
    gui_region_end();
}

/*==============================================================================================
    Font overlay

    Text readout of the font registry and the resolver ledger behind it: one row per loaded
    slot (size, coverage, resident bitmap, ownership marks, source file), plus the resolver's
    memo / shipped-scan / baker facts.  The view for "why are there this many fonts": every
    row names who owns it (H held, S/L ramp role, v<N> a viewport's landed DPI font) -- an
    unmarked row is evictable, a marked one is somebody's live answer.

    Toggled by the selector menu's checkbox (no hotkey -- rarely flipped mid-chase).
==============================================================================================*/

static void
overlay_fonts( void )
{
    /* Last column of the HUD row (see the column map above). */
    f32 top_y = gui_viewport_content_y( 0 ) + OVL_TOP_PAD;

    gui_region_begin( "font_overlay", OVL_COL_FONTS, top_y, 0.0f, 0.0f, GUI_REGION_FG, GUI_VP_MAIN,
                      GUI_WIN_DEBUG_BAND | GUI_WIN_NOSCROLL | GUI_WIN_NO_INPUT
                      | GUI_WIN_ALWAYS_AUTOSIZE );
    {
        overlay_backdrop();
        gui_stack();
        gui_scale_push( GUI_SCALE_DENSE );   /* tight row pitch -- a HUD, not a form */

        /* Header: slot occupancy + total resident glyph pixels (CPU-side R8 bitmaps). */
        u32 used = 0, resident = 0;
        for ( u32 id = 0; id < GUI_FONT_REGISTRY_MAX; ++id )
        {
            font_slot_t* s = font_slot_ptr( id );
            if ( s && s->used )
            {
                ++used;
                resident += s->atlas_w * s->atlas_h;
            }
        }
        gui_textf( "Fonts %u/%u slots  %u kB resident",
                   used, (u32)GUI_FONT_REGISTRY_MAX, ( resident + 1023u ) / 1024u );
        /* Flag key.  The "key:" prefix matters: without it this line reads as a live status
           message ("! upload failed") rather than a legend for the row flags below. */
        gui_text_colored( COL_TEXT_SECONDARY_IDLE,
                          "key:  *=active  G=font_get  S/L=role  vN=dpi  !=upload-failed" );
        gui_new_line( 2.0f );

        /* One row per loaded slot.  Glyph coverage = the dense ASCII tier plus the extended
           records a -range bake carries.  Slot 0 is the default font (implicitly pinned). */
        for ( u32 id = 0; id < GUI_FONT_REGISTRY_MAX; ++id )
        {
            font_slot_t* s = font_slot_ptr( id );
            if ( !s || !s->used )
                continue;

            char flags[ 16 ];
            font_resolve_debug_flags( id, flags, sizeof( flags ) );

            gui_textf( "%c%2u %3upx %4u gl %4ux%-4u %-4s %s%s%s",
                       id == font_active_id() ? '*' : ' ', id,
                       (u32)( s->metrics.size + 0.5f ),
                       (u32)ORB_FONT_CP_COUNT + s->ext_count,
                       s->atlas_w, s->atlas_h,
                       flags,
                       s->name[ 0 ] ? s->name : "(unnamed)",
                       s->sdf_range     ? " sdf" : "",
                       s->upload_failed ? " !"   : "" );
        }

        /* The resolver ledger behind the slots. */
        font_resolve_debug_t rd = font_resolve_debug();
        gui_new_line( 2.0f );
        gui_textf( "memo %u/%u  shipped %u%s  baker %s",
                   rd.memo_used, rd.memo_cap,
                   rd.ship_count, rd.ship_scanned ? "" : " (unscanned)",
                   rd.baker ? "yes" : "no" );

        gui_scale_pop();
    }
    gui_region_end();
}

/*==============================================================================================
    Frame hooks -- OS services gui cannot reach itself

    gui links only app + rhi (no sys), so the wall clock, sleep, and block-on-input wait arrive as
    callbacks, set once after init().
    - clock powers the perf overlay's emit/render timing.
    - sleep + wait power boot_pace() (gui_boot.c); inert for a host that paces itself.

    Any hook may be NULL -- the dependent feature just switches off (no clock -> timing reads
    zero; no wait -> idle skip unavailable).
==============================================================================================*/

static gui_sleep_fn       s_hook_sleep;
static gui_wait_events_fn s_hook_wait;

void
gui_frame_set_hooks( gui_clock_fn clock, gui_sleep_fn sleep_ms, gui_wait_events_fn wait_events )
{
    s_perf.clock = clock;            /* adopted by perf_frame_begin / render brackets next frame */
    gui_render_set_clock( clock );   /* the render server times its own phases with the same one */
    s_hook_sleep = sleep_ms;
    s_hook_wait  = wait_events;
}

/*==============================================================================================
    Debug driver -- hotkeys + internal overlay emission (armed by debug_enable( true ))

    Debug modes that used to be host-side loop state (perf/state overlay tiers, pipeline
    dashboard, render mode, retained/idle skip) live here, driven by hotkeys read from the
    frame's own IO snapshot.
    - debug_hotkeys() runs from frame_begin, after io_frame_begin.
    - debug_overlays_emit() runs from ctx_end, while the DEFAULT context is still bound -- last
      in its build, so overlays draw on top and their cost counts like any other widget.

    Every hotkey is gated behind a master ARM, so the broad single-letter keys stay inert during
    normal use:

        NP_DOT  master arm ('.'): toggles EVERY debug hotkey on/off as a group. Off by default.
                Disarming resets every debug mode to normal (overlays off, render mode normal,
                layers cleared). Main-row '.' arms too (laptop keyboards), except while a stepper
                freeze owns it for scrub.
        NP1-NP7 debug layers (window / interact / resize / layout / clip / content / region rects)
        F7      style-record census: dump the histogram to the log (shift-F7 clears it)
        F8      command stepper: show/hide the control window (Capture there freezes the frame)
        F9      render mode: normal -> wireframe -> batch tint
        F10     pipeline dashboard window
        NP+     perf overlay tier  (off / fps / +timings / +counts / +retained)
        NP-     state overlay tier (off / ids / +focus,nav / +popups)
        , .     command stepper (while frozen): step the replay cursor back/forward
                (repeat-aware -- holding scrubs; shift steps by 16)
                (picking a command under the mouse is the stepper window's own Pick toggle, not
                a hotkey -- a hotkey would fight the focused window's keyboard nav/type-ahead)

    While armed, a selector panel (debug_selector_menu, right edge of viewport) is also up:
    - Mirrors NP+ / NP- as sliders.
    - Adds the levers that have no key of their own: retained skip (tessellation cache), force
      redraw, idle skip.
    - Ends with a KEY LEGEND -- every key above, its name, and live value (lit when on). This is
      the only readout the NP1-NP7 layer bits have. Counted as debug rendering (GUI_WIN_DEBUG_BAND),
      so it never perturbs the perf-stats or counts it's showing.

    Letter and numpad keys are fenced by want_capture_keyboard, so typing in a text field never
    triggers them (numpad digits are text input with Num Lock on).

    NOTE: a host that writes set_force_redraw itself every frame (e.g. sb_gui_editor, pinned for
    play mode / always-emit) owns that flag -- its per-frame write overrides the hotkey toggle.
==============================================================================================*/


/* Master switch for this whole section: gui_debug_enable( true ) opts a host into the debug
   driver; NP_DOT then arms the individual hotkeys below. */

static bool s_debug_enabled;

void gui_debug_enable( bool enable )
{
    s_debug_enabled = enable;
    if ( enable )
        gui_log( GUI_LOG_INFO,
                 "debug driver on -- press '.' (main row or numpad) to arm the debug hotkeys" );
}

bool gui_debug_is_enabled( void ) { return s_debug_enabled; }

static int  s_dbg_perf_mode;     /* perf overlay tier, NP_ADD cycles 0..DBG_PERF_TIERS-1     */
static int  s_dbg_state_mode;    /* state overlay tier, NP_SUB cycles 0..3                  */
static bool s_dbg_font_open;     /* font registry overlay, selector menu checkbox toggles  */
static bool s_dbg_mem_open;      /* memory overlay, selector menu checkbox toggles          */
static bool s_dbg_dash_open;     /* pipeline dashboard, F10 toggles (X button writes false) */
static bool s_dbg_step_open;     /* command stepper window, F8 opens (X button hides)       */
static bool s_idle_skip;         /* boot_pace: block on OS input when idle, selector menu toggles */
static bool s_dbg_hotkeys_armed; /* master arm: every hotkey below is inert until NP_DOT arms it */

/* For hosts that own a debug lever themselves (e.g. sb_gui_editor's set_force_redraw write for
   its scene pass): while armed, the selector menu's checkboxes are the sole owner of force
   redraw / retained skip / idle skip. A host's own per-frame write should stand down and let the
   menu's value stick, rather than fighting it every frame. */
bool gui_debug_hotkeys_armed( void ) { return s_dbg_hotkeys_armed; }

/* Remembered selector-menu lever values.
   - debug_reset() snapshots these when the arm goes off, so disarming can still force the live
     flags back to normal.
   - debug_restore() re-applies them when the arm goes back on, so reopening the menu picks up
     exactly where it left off instead of the arm's "normal" defaults.
   Perf/state tier and idle skip need no shadow of their own -- debug_reset() doesn't touch them,
   so they're already sitting at their last value when the menu reopens. */
static bool s_dbg_force_redraw_saved;
static bool s_dbg_retained_skip_saved = true;   /* default: cached (skip tess when unchanged) */

/* True while any context that closed this frame still had an animation in flight -- the OR of
   every ctx_end's wants_redraw, reset each frame_begin.  boot_pace reads it to keep pumping
   ~60 Hz frames while a transition settles instead of blocking on input mid-animation. */
static bool s_any_redraw;

/* Programmatic idle-skip control, for hosts that want it on without the hotkey. */
void gui_set_idle_skip( bool on ) { s_idle_skip = on; }
bool gui_idle_skip( void )        { return s_idle_skip; }

/*==============================================================================================
    Key tables -- bindings AND their display names, in one place

    Read by both consumers: debug_hotkeys() polls the keys; the selector panel's key legend
    (debug_selector_menu) prints them with live state. One row per setting, so adding a layer or
    tier updates the hotkey, the legend, and the slider range from a single edit -- array LENGTHS
    double as the tier cycles' moduli and the sliders' ranges.
==============================================================================================*/

/* NP1-NP7 -- the debug layer bits. */
static const struct
{
    app_key_t   key;        // numpad key that toggles the bit
    const char* key_name;   // how the key reads in the legend
    const char* name;       // what the layer draws
    u32         layer;      // GUI_DBG_* bit

} k_dbg_layer[] = {
    { APP_KEY_NP_1, "NP1", "window",   GUI_DBG_WINDOW   },
    { APP_KEY_NP_2, "NP2", "interact", GUI_DBG_INTERACT },
    { APP_KEY_NP_3, "NP3", "resize",   GUI_DBG_RESIZE   },
    { APP_KEY_NP_4, "NP4", "layout",   GUI_DBG_LAYOUT   },
    { APP_KEY_NP_5, "NP5", "clip",     GUI_DBG_CLIP     },
    { APP_KEY_NP_6, "NP6", "content",  GUI_DBG_CONTENT  },
    { APP_KEY_NP_7, "NP7", "region",   GUI_DBG_REGION   },
};

/* NP+ / NP- tiers and the F9 render mode, spelled out -- what the sliders' bare numbers mean. */
static const char* const k_perf_tier   [] = { "off", "fps", "+timings", "+counts", "+retained" };
static const char* const k_state_tier  [] = { "off", "ids", "+focus", "+popups" };
static const char* const k_render_mode [] = { "normal", "wireframe", "batch" };

#define DBG_LAYER_COUNT  ( (u32)( sizeof( k_dbg_layer   ) / sizeof( k_dbg_layer  [ 0 ] ) ) )
#define DBG_PERF_TIERS   ( (int)( sizeof( k_perf_tier   ) / sizeof( k_perf_tier  [ 0 ] ) ) )
#define DBG_STATE_TIERS  ( (int)( sizeof( k_state_tier  ) / sizeof( k_state_tier [ 0 ] ) ) )

/* Return every debug mode to normal. Called when the master arm switches off, so disarming
   visibly clears the screen (overlays, selector menu, layer rects, render mode) instead of
   leaving whatever was toggled on frozen in place.

   The two real engine flags (force redraw, retained skip) are snapshotted into s_dbg_*_saved
   first, so debug_restore() can put them back on re-arm. Everything else the selector menu shows
   (perf/state tier, idle skip) is plain local state this function doesn't touch -- it just sits
   at its last value, hidden by the arm's own gate meanwhile. */
static void
debug_reset( void )
{
    s_dbg_dash_open  = false;  /* dashboard closed  */
    s_dbg_step_open  = false;  /* stepper window closed */

    gui_render_set_mode( GUI_RENDER_NORMAL );   /* wireframe / batch tint -> normal */
    gui_debug_set_layers( 0 );                  /* clear all NP1-7 layer rects      */

    s_dbg_retained_skip_saved = build_retained_skip();
    s_dbg_force_redraw_saved  = gui_force_redraw();
    build_set_retained_skip( true );        /* normal: skip tess when unchanged */
    gui_set_force_redraw( false );              /* normal: allow clean-frame emit skip */

#ifdef GUI_CMD_STEPPER
    if ( step_frozen() )
        step_release();                     /* unfreeze back to live emission */
#endif

    redraw_request();
}

/*============================================================================================*/
/* Put the remembered selector-menu lever values back. Called when the master arm switches back
   on, so the panel (and the behavior it drives) reopens exactly where the user left it, instead
   of debug_reset()'s defaults. Perf/state tier and idle skip need no restore -- they were never
   reset, so they're already correct. */

static void
debug_restore( void )
{
    build_set_retained_skip( s_dbg_retained_skip_saved );
    gui_set_force_redraw( s_dbg_force_redraw_saved );
}

/*============================================================================================*/
/* Poll the debug hotkeys from this frame's IO snapshot. Called from frame_end -- after
   nav_new_frame and all widget emission, so nav/widgets have already consumed any key they use
   (see gui_want_capture_keyboard) -- only while debug_enable is on.

   Runs AFTER this frame's overlay emit, so a mode change here is one frame too late for THIS
   frame's draw list. Every branch that mutates a mode calls redraw_request() so frame_begin sees
   the frame as dirty next time, instead of an idle/retained replay sitting on the stale mode. */

static void
debug_hotkeys( void )
{
    /* Master arm: numpad '.' is the one always-live debug key. It gates every other hotkey below,
       so function keys stay inert during normal use until this explicit opt-in. Disarming resets
       every debug mode to normal (debug_reset), returning the view to a clean state in one press.
       Fenced by want_capture_keyboard, like the letter keys (numpad '.' is text input with Num
       Lock on) -- never fires while a text field is focused. Chosen because it's rarely bound
       elsewhere.

       Main-row '.' arms too (laptop keyboards have no numpad), except during a stepper freeze,
       where '.' is the scrub-forward key below and owns the row -- NP_DOT still disarms then. */

    bool arm_toggle = gui_is_key_pressed( APP_KEY_NP_DOT );
#ifdef GUI_CMD_STEPPER
    if ( !step_frozen() )
#endif
        arm_toggle = arm_toggle || gui_is_key_pressed( APP_KEY_PERIOD );
    if ( !gui_want_capture_keyboard() && arm_toggle )
    {
        s_dbg_hotkeys_armed = !s_dbg_hotkeys_armed;
        gui_log( GUI_LOG_INFO, "debug hotkeys: %s", s_dbg_hotkeys_armed ? "ARMED" : "off" );
        if ( s_dbg_hotkeys_armed )
            debug_restore();
        else
            debug_reset();
        redraw_request();
    }
    if ( !s_dbg_hotkeys_armed )
        return;

    /* Function keys are never text input -- no keyboard fence needed. */
#ifdef GUI_PRIM_CENSUS
    /* F7 dumps the style-record census to the log; shift-F7 clears it, so a run can be scoped to
       one window or one demo instead of everything since boot.  Read-only either way -- nothing
       about the frame changes, so no redraw_request(). */
    if ( gui_is_key_pressed( APP_KEY_F7 ) )
    {
        if ( gui_is_key_down( APP_KEY_LSHIFT ) || gui_is_key_down( APP_KEY_RSHIFT ) )
        {
            prim_census_reset();
            gui_log( GUI_LOG_INFO, "style census: cleared" );
        }
        else
        {
            prim_census_dump( "F7" );
        }
    }
#endif
    if ( gui_is_key_pressed( APP_KEY_F9 ) )
    {
        gui_render_mode_t m = ( gui_render_get_mode() + 1 ) % GUI_RENDER_MODE_COUNT;
        gui_render_set_mode( m );
        gui_log( GUI_LOG_INFO, "render mode: %s", k_render_mode[ m ] );
        redraw_request();
    }
    if ( gui_is_key_pressed( APP_KEY_F10 ) )
    {
        s_dbg_dash_open = !s_dbg_dash_open;
        gui_log( GUI_LOG_INFO, "pipeline dashboard: %s", s_dbg_dash_open ? "open" : "closed" );
        redraw_request();
    }

#ifdef GUI_CMD_STEPPER
    /* F8 shows/hides the control window (gui_step_window.c) -- it does NOT freeze. Opening the
       stepper leaves the scene live; only the window's Capture button freezes this frame's
       band-0 command list for stepped replay (the , . hotkeys below scrub an active freeze).
       The window's X button hides it but never releases an active freeze. */
    if ( gui_is_key_pressed( APP_KEY_F8 ) )
    {
        s_dbg_step_open = !s_dbg_step_open;
        gui_log( GUI_LOG_INFO, "command stepper window: %s", s_dbg_step_open ? "open" : "closed" );
        redraw_request();
    }
#endif

    /* Letter and numpad keys: fenced so a focused text field owns them (numpad digits ARE text
       input with Num Lock on, unlike the function keys above). */
    if ( gui_want_capture_keyboard() )
        return;

    /* NP1-NP7 toggle the debug layer mask (window / interact / resize / layout / clip / content
       / region geometry). Initial-press only, so holding a key never flickers the layer.

       Layer setters compile to no-ops in Release (render/gui_debug_overlay.c), so no build guard
       is needed here. CONTENT differs from the rest: it draws into the MAIN list at region pop
       (gui_scroll.c) rather than the overlay list, so toggling it changes every scrollable
       window's emitted commands -- redraw_request() below makes that land instead of sitting
       behind the clean-frame emit skip. */
    for ( u32 i = 0; i < DBG_LAYER_COUNT; ++i )
        if ( gui_is_key_pressed( k_dbg_layer[ i ].key ) )
        {
            gui_debug_set_layers( gui_debug_get_layers() ^ k_dbg_layer[ i ].layer );
            redraw_request();
        }

    /* Perf / state overlay tiers keep a quick hotkey (numpad +/-, paired away from the letter
       row) alongside their slider in debug_selector_menu -- flipped often enough while chasing a
       frame that a click is friction. C/F/I have no letter keys at all: rarely-toggled booleans
       are better discovered as checkboxes than memorized as hotkeys. */
    if ( gui_is_key_pressed( APP_KEY_NP_ADD ) )
    {
        s_dbg_perf_mode = ( s_dbg_perf_mode + 1 ) % DBG_PERF_TIERS;
        redraw_request();
    }

    if ( gui_is_key_pressed( APP_KEY_NP_SUB ) )
    {
        s_dbg_state_mode = ( s_dbg_state_mode + 1 ) % DBG_STATE_TIERS;
        redraw_request();
    }

#ifdef GUI_CMD_STEPPER
    /* , . step the frozen replay cursor (repeat-aware -- holding scrubs; shift steps by 16).
       The seek latches at the next frame's restore, so redraw_request() is required here, or the
       clean-frame emit skip would sit on the stale cursor (the deferred-update rule). */
    if ( step_frozen() )
    {
        u32  stride = ( gui_is_key_down( APP_KEY_LSHIFT ) || gui_is_key_down( APP_KEY_RSHIFT ) )
                          ? 16u : 1u;
        bool back   = gui_is_key_pressed_repeat( APP_KEY_COMMA );
        bool fwd    = gui_is_key_pressed_repeat( APP_KEY_PERIOD );
        if ( back || fwd )
        {
            u32 c = step_cursor();
            if ( back )
                c = c > stride ? c - stride : 0u;
            else
                c = c + stride;               /* seek clamps to the frozen command count */
            step_seek( c );
            gui_log( GUI_LOG_INFO, "command stepper: %u/%u", step_cursor(), step_count() );
            redraw_request();
        }
    }
#endif
}

/*==============================================================================================
    Debug selector menu -- dense lever panel + key legend, right edge of the viewport

    An actual UI, where C/F/I/P/O used to be single letters read out of the raw key stream:
    - Three checkboxes: retained skip, force redraw, idle skip.
    - Perf/state overlay tier sliders.
    - A KEY LEGEND at the bottom: every key-driven setting, its name, and live value (lit when
      on). This is what makes the numpad keys usable without reading source -- a tier slider
      reading "3" says nothing, "NP+ perf +counts" does. The seven layer bits (NP1-NP7) have no
      other readout at all. Both halves walk the same k_dbg_layer / k_*_tier tables the hotkeys use.

    Whole panel at GUI_SCALE_DENSE (HUD row pitch, like the two overlays), so the legend costs
    less height than the old five rows did.

    Shown exactly while the master arm is on (NP_DOT) -- press it again and debug_reset() clears
    the levers back to default the same frame this panel disappears.

    GUI_WIN_DEBUG_BAND, not GUI_WIN_NO_INPUT (unlike the read-only overlays above): this panel
    must be clickable, but its own geometry still has to stay out of the stats/counts it's used
    to tweak -- same arena-band exemption the perf/state overlays get. */

/* Panel strings, named once so the width measure below and the emit further down always agree.
   A measure of text the panel doesn't actually print is how a "fits" panel quietly stops
   fitting. */

#define SEL_HINT    "debug -- '.' to close"
#define SEL_FORCE   "Force redraw"
#define SEL_TESS    "Tess cache"
#define SEL_IDLE    "Idle skip"
#define SEL_PAL     "Style palette"
#define SEL_INTERN  "Style interning"
#define SEL_FONTS   "Font registry"
#define SEL_MEM     "Memory"
#define SEL_PERF    "NP+ perf"
#define SEL_STATE   "NP- state"

   /*============================================================================================*/
/* One legend line: "<key> <name>" plus its value where it has one (a tier/mode; NULL for the
   layer bits, which are just on or off). Both the measure pass and the paint go through this one
   helper, so they can never disagree on row width. */
static const char*
legend_line( char* buf, u32 cap, const char* key_name, const char* name, const char* value )
{
    fmt_snprintf( buf, cap, "%s %s%s%s", key_name, name, value ? " " : "", value ? value : "" );
    return buf;
}

/*============================================================================================*/
/* Paint one legend row: lit while the setting is on, dim while off -- reads as "what's on right
   now" at a glance, while still naming every key for discovery. One line per row, one colour per
   line -- keeps the list a narrow column, which is what keeps the panel thin. */
static void
legend_row( const char* key_name, const char* name, const char* value, bool on )
{
    char line[ 48 ];
    gui_text_colored( on ? COL_MARK_IDLE : COL_TEXT_SECONDARY_IDLE,
                      legend_line( line, sizeof( line ), key_name, name, value ) );
}

/*============================================================================================*/
/* The widest row this panel can EVER print, in px of the live font -- the content width it needs.

   MEASURED, not a constant: text moves with the DPI / ui_scale response (frame/gui_frame_dpi.c),
   so a hardcoded width fits at one scale and clips at every other. Every legend row is measured
   at every VALUE it can take, not just today's, so the panel holds one width as tiers cycle
   instead of breathing (same fixed-footprint rule the overlay's status rows follow).

   ~20 short measures a frame, only while the debug arm is on. Call inside the scale scope the
   panel emits in -- WIDGET_PAD / CHECKBOX_SZ are style reads. */

static f32
selector_content_w( f32 label_w )
{
    char buf[ 48 ];
    f32  w = font_text_w( SEL_HINT );

    /* Checkbox rows: indicator box + gap + label. */
    static const char* const k_lever[] = { SEL_FORCE, SEL_TESS, SEL_IDLE, SEL_PAL,
                                           SEL_INTERN, SEL_FONTS, SEL_MEM };
    for ( u32 i = 0; i < sizeof( k_lever ) / sizeof( k_lever[ 0 ] ); ++i )
    {
        f32 row = CHECKBOX_SZ + WIDGET_PAD + font_text_w( k_lever[ i ] );
        if ( row > w ) w = row;
    }

    /* Slider rows: the label column plus a track wide enough to read a knob and its value. */
    f32 slider_row = label_w + WIDGET_PAD + font_text_w( "0" ) * 8.0f;
    if ( slider_row > w ) w = slider_row;

    /* Legend rows, each at its longest possible value. */
    for ( int i = 0; i < DBG_PERF_TIERS; ++i )
    {
        f32 row = font_text_w( legend_line( buf, sizeof( buf ), "NP+", "perf", k_perf_tier[ i ] ) );
        if ( row > w ) w = row;
    }
    for ( int i = 0; i < DBG_STATE_TIERS; ++i )
    {
        f32 row = font_text_w( legend_line( buf, sizeof( buf ), "NP-", "state", k_state_tier[ i ] ) );
        if ( row > w ) w = row;
    }
    for ( int i = 0; i < GUI_RENDER_MODE_COUNT; ++i )
    {
        f32 row = font_text_w( legend_line( buf, sizeof( buf ), "F9", "render", k_render_mode[ i ] ) );
        if ( row > w ) w = row;
    }
    for ( u32 i = 0; i < DBG_LAYER_COUNT; ++i )
    {
        f32 row = font_text_w( legend_line( buf, sizeof( buf ), k_dbg_layer[ i ].key_name,
                                            k_dbg_layer[ i ].name, NULL ) );
        if ( row > w ) w = row;
    }
    return w;
}

/*============================================================================================*/
static void
debug_selector_menu( void )
{
    /* Dense HUD pitch, pushed BEFORE the region opens: region_begin bakes REGION_PAD_DEFAULT
       (a WIDGET_PAD / WIDGET_GAP read) in at push time. Pushing dense scale AFTER would leave
       the panel's own inset at the STD step while rows inside ran dense, and would measure the
       width below against the wrong pad. */
    gui_scale_push( GUI_SCALE_DENSE );

    /* Work top (caption band + menu bar) + this panel's own margin -- see perf_overlay. */
    f32 top_y = gui_viewport_content_y( 0 ) + 8.0f;

    /* Thin by design: the legend runs straight down (one key per row), so the widest row is a
       single "<key> <name> <value>" run (e.g. "NP- state +retained"), not two columns of them.
       Panel width = widest row it can print + the region's own left/right inset
       (REGION_PAD_DEFAULT = WIDGET_PAD each side); label_w holds the longest slider label. */
    f32 label_w = font_text_w( SEL_STATE ) + WIDGET_PAD;
    f32 w       = selector_content_w( label_w ) + 2.0f * WIDGET_PAD;
    f32 x       = (f32)s_io.display_w - w - 8.0f;

    gui_region_begin( "debug_selector", x, top_y, w, 0.0f, GUI_REGION_FG, GUI_VP_MAIN,
                      GUI_WIN_NOSCROLL | GUI_WIN_DEBUG_BAND );
    {
        overlay_backdrop();
        gui_stack();

        /* The ambient field is a set-once global (gui_field_set) -- whatever the last host
           window declared is still installed here, and this panel sets its own below. Snapshot
           and restore so neither direction leaks across the seam. */
        gui_field_t saved_field = *gui_field_get();
        gui_field_set( NULL );                  /* default: box on the left, label trailing */

        gui_text_colored( COL_TEXT_SECONDARY_IDLE, SEL_HINT );

        bool force = gui_force_redraw();
        if ( gui_checkbox( SEL_FORCE, &force ) )
            gui_set_force_redraw( force );

        bool cached = build_retained_skip();
        if ( gui_checkbox( SEL_TESS, &cached ) )
            build_set_retained_skip( cached );

        gui_checkbox( SEL_IDLE,  &s_idle_skip );

        /* Off, every style takes a per-slot arena record the way it did before the palette --
           more records, the same pixels.  Any difference on screen between the two settings is a
           palette bug, which is the whole point of the lever.  Deliberately NOT restored by
           debug_reset: comparing the two states usually means closing this panel so it stops
           covering what is being compared. */
        bool pal = pal_enabled();
        if ( gui_checkbox( SEL_PAL, &pal ) )
            pal_set_enabled( pal );

        /* The finer half: on, a record earns a palette entry once the frame has drawn it
           again; off, the table stops growing and everything beyond it takes a per-slot
           record.  Free to flip either way -- interning only appends, so nothing already
           handed out changes meaning. */
        bool intern = pal_intern_enabled();
        if ( gui_checkbox( SEL_INTERN, &intern ) )
            pal_set_intern( intern );

        gui_checkbox( SEL_FONTS, &s_dbg_font_open );
        gui_checkbox( SEL_MEM,   &s_dbg_mem_open  );

        /* Tier sliders under a left label column, so the two labels align and the tracks line up
           with each other instead of each starting after its own label's width. */
        gui_field_label_left( label_w );
        gui_slider_int( SEL_PERF,  &s_dbg_perf_mode,  0, DBG_PERF_TIERS  - 1 );
        gui_slider_int( SEL_STATE, &s_dbg_state_mode, 0, DBG_STATE_TIERS - 1 );
        gui_field_set( NULL );

        /* Key legend, straight down: tier rows spell out the number their slider shows; layer
           rows are the only state readout NP1-NP7 have. */
        gui_separator_text( "keys" );

        legend_row( "NP+", "perf",  k_perf_tier [ s_dbg_perf_mode  ], s_dbg_perf_mode  > 0 );
        legend_row( "NP-", "state", k_state_tier[ s_dbg_state_mode ], s_dbg_state_mode > 0 );

        gui_render_mode_t rmode = gui_render_get_mode();
        legend_row( "F9", "render", k_render_mode[ rmode ], rmode != GUI_RENDER_NORMAL );

        u32 layers = gui_debug_get_layers();
        for ( u32 i = 0; i < DBG_LAYER_COUNT; ++i )
            legend_row( k_dbg_layer[ i ].key_name, k_dbg_layer[ i ].name, NULL,
                        ( layers & k_dbg_layer[ i ].layer ) != 0u );

        gui_field_set( &saved_field );
    }
    gui_region_end();
    gui_scale_pop();
}

/*============================================================================================*/
/* Emit the debug overlays into the currently bound (default) context. Called from ctx_end before
   it rebinds -- last in the default context's build, drawing on top of everything else emitted,
   exactly where a host used to hand-place these. */

static void
debug_overlays_emit( void )
{
    dash_window( &s_dbg_dash_open );
    step_window( &s_dbg_step_open );
    if ( s_dbg_hotkeys_armed )
        debug_selector_menu();

    /* Tier state is no longer zeroed on disarm (debug_reset) so the selector menu can remember
       it -- gate visibility on the arm here instead, the same net effect (hidden while off). */

    overlay_perf  ( s_dbg_hotkeys_armed ? s_dbg_perf_mode  : 0 );
    overlay_state ( s_dbg_hotkeys_armed ? s_dbg_state_mode : 0 );
    if ( s_dbg_hotkeys_armed && s_dbg_mem_open )
        overlay_memory();
    if ( s_dbg_hotkeys_armed && s_dbg_font_open )
        overlay_fonts();
}

/* NOTE: gui_boot_pace() -- the boot loop's end-of-frame sleep -- lives in gui_boot.c with the
   rest of that loop.  It reads the frame hooks (s_hook_sleep/wait), s_idle_skip, and s_perf set
   here, plus s_any_redraw folded in gui_frame_loop.c; the gui_frame.c unity includes gui_boot.c
   last, so those statics are all in scope there. */

// clang-format on
/*============================================================================================*/
