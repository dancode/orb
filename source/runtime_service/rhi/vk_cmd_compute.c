/*==============================================================================================

    vk_cmd_compute.c -- Compute command recording.

    Compute commands operate on the same open command buffer as graphics commands but
    use VK_PIPELINE_BIND_POINT_COMPUTE.  Pipeline binding and push constants are shared
    with graphics (see vk_cmd_graphics.c); this file owns compute-specific dispatch.

==============================================================================================*/

static void
vk_cmd_dispatch( rhi_cmd_t cmd, u32 groups_x, u32 groups_y, u32 groups_z )
{
    if ( !cmd ) return;
    vkCmdDispatch( cmd->vk_cmd, groups_x, groups_y, groups_z );
}

/*============================================================================================*/
