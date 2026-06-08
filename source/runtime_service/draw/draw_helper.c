/*==============================================================================================

    runtime_service/draw/draw_helper.c -- High-level draw helpers.

    Included by draw.c after draw_cmd.c so draw_begin, draw_end, and the file-scope
    state struct s (from draw_cmd.c) are all visible in this translation unit.

    Three helpers:

        draw_ortho_2d    -- build a column-major pixel-space orthographic matrix for
                            use with draw()->begin().

        draw_begin_pass  -- open a full-window 2D render pass: binds bindless descriptors,
                            clears the swapchain image, sets viewport/scissor, and calls
                            draw_begin() with a pixel-space ortho matrix.  Reduces frame
                            setup to a single call.

        draw_end_pass    -- flush accumulated draw calls (draw_end) and close the render
                            pass (cmd_end_rendering).  Matches every draw_begin_pass().

==============================================================================================*/
// clang-format off

/*----------------------------------------------------------------------------------------------
    draw_ortho_2d

    Fills out[16] with a column-major orthographic projection that maps pixel coordinates
    ([0,w] x [0,h], origin top-left) to Vulkan NDC (x in [-1,+1], y=-1 at top, y=+1 at
    bottom).  z passes through unchanged with a scale of 1 and no translation, so 2D
    primitives at z=0 land at NDC z=0 (mid-depth).
----------------------------------------------------------------------------------------------*/

static void
draw_ortho_2d( f32 out[ 16 ], f32 w, f32 h )
{
    /* column 0 */ out[  0 ] =  2.0f / w; out[  1 ] =  0.0f;      out[  2 ] = 0.0f; out[  3 ] = 0.0f;
    /* column 1 */ out[  4 ] =  0.0f;     out[  5 ] =  2.0f / h;  out[  6 ] = 0.0f; out[  7 ] = 0.0f;
    /* column 2 */ out[  8 ] =  0.0f;     out[  9 ] =  0.0f;      out[ 10 ] = 1.0f; out[ 11 ] = 0.0f;
    /* column 3 */ out[ 12 ] = -1.0f;     out[ 13 ] = -1.0f;      out[ 14 ] = 0.0f; out[ 15 ] = 1.0f;
}

/*----------------------------------------------------------------------------------------------
    draw_begin_pass

    Canonical frame setup for a 2D overlay pass:

        1. cmd_bind_bindless   -- establish the global descriptor set for this frame.
        2. cmd_begin_rendering -- open a dynamic pass on the swapchain image with a CLEAR.
        3. cmd_set_viewport    -- full-window viewport (always dynamic state, must be set).
        4. cmd_set_scissor     -- full-window scissor  (always dynamic state, must be set).
        5. draw_begin          -- cache cmd and install a pixel-space ortho view-projection.

    After this call the frame is ready for rect / circle / box calls.  close with
    draw_end_pass() before frame_end().
----------------------------------------------------------------------------------------------*/

static void
draw_begin_pass( rhi_cmd_t cmd, i32 win_w, i32 win_h, const f32 clear_rgba[ 4 ] )
{
    rhi()->cmd_bind_bindless( cmd );

    rhi_color_attachment_t color_att = {
        .texture  = { .id = RHI_SWAPCHAIN_COLOR },
        .load_op  = RHI_LOAD_OP_CLEAR,
        .store_op = RHI_STORE_OP_STORE,
        .clear    = { .r = clear_rgba[ 0 ], .g = clear_rgba[ 1 ],
                      .b = clear_rgba[ 2 ], .a = clear_rgba[ 3 ] },
    };
    rhi()->cmd_begin_rendering( cmd, &color_att, 1, NULL );

    rhi()->cmd_set_viewport( cmd, &( rhi_viewport_t ){
        .x = 0.0f, .y = 0.0f,
        .width     = (f32)win_w,
        .height    = (f32)win_h,
        .min_depth = 0.0f,
        .max_depth = 1.0f,
    } );
    rhi()->cmd_set_scissor( cmd, &( rhi_rect_t ){
        .x = 0, .y = 0, .width = win_w, .height = win_h,
    } );

    f32 vp[ 16 ];
    draw_ortho_2d( vp, (f32)win_w, (f32)win_h );
    draw_begin( cmd, vp );
}

/*----------------------------------------------------------------------------------------------
    draw_end_pass

    Flush all queued draw calls and close the render pass opened by draw_begin_pass().
    s.cmd is the command list cached in draw_begin; it remains valid until frame_end().
----------------------------------------------------------------------------------------------*/

static void
draw_end_pass( void )
{
    draw_end();
    rhi()->cmd_end_rendering( s.cmd );
}

// clang-format on
/*============================================================================================*/
