/*==============================================================================================

    vk_pipeline_cache.c -- VkPipelineCache disk persistence.

    Saves compiled PSO binary data to bin/vk_pipeline_cache.bin on shutdown and loads it
    on the next session, eliminating driver SPIR-V -> ISA recompilation stalls.

    vk_pipeline_cache_load  called from vk_device_create before any pipeline creation.
    vk_pipeline_cache_save  called from vk_device_destroy and on error unwind paths.

==============================================================================================*/

static const char* const PIPELINE_CACHE_PATH = "bin/vk_pipeline_cache.bin";

static void
vk_pipeline_cache_load( void )
{
    if ( vk.use_pipeline_cache )
    {
        LOG_INFO( "pipeline_cache_load: enabled; attempting to load from disk" );
    }
    else 
    {
        LOG_INFO( "pipeline_cache_load: disabled; skipping" );
        return;
    }

    void*  data      = NULL;
    size_t data_size = 0;

    FILE* f = fopen( PIPELINE_CACHE_PATH, "rb" );
    if ( f )
    {
        fseek( f, 0, SEEK_END );
        long sz = ftell( f );
        fseek( f, 0, SEEK_SET );

        if ( sz > 0 )
        {
            data = malloc( (size_t)sz );
            if ( data )
            {
                data_size = fread( data, 1, (size_t)sz, f );
                if ( data_size != (size_t)sz )
                {
                    free( data );
                    data      = NULL;
                    data_size = 0;
                }
            }
        }
        fclose( f );
    }

    VkPipelineCacheCreateInfo ci = { 0 };
    ci.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = data_size;
    ci.pInitialData    = data;

    VkResult r = vkCreatePipelineCache( vk.device, &ci, vk.alloc_cb, &vk.pipeline_cache );
    if ( r != VK_SUCCESS )
    {
        LOG_WARN( "pipeline_cache_load: vkCreatePipelineCache: %s; continuing without cache",
                  string_VkResult( r ) );
        vk.pipeline_cache = VK_NULL_HANDLE;
    }
    else
    {
        LOG_INFO( "pipeline_cache_load: OK (%zu bytes from disk)", data_size );
    }

    free( data );
}

/*============================================================================================*/
/* Save the pipeline cache to disk for loading on the next run.  Called from vk_device_destroy
   and on error unwind paths. */

static void
vk_pipeline_cache_save( void )
{
    if ( !vk.use_pipeline_cache )
    {
        LOG_INFO( "pipeline_cache_save: disabled; skipping" );
        return;
    }

    if ( vk.pipeline_cache == VK_NULL_HANDLE )
        return;

    size_t data_size = 0;
    VkResult r = vkGetPipelineCacheData( vk.device, vk.pipeline_cache, &data_size, NULL );
    if ( r != VK_SUCCESS || data_size == 0 )
        return;

    void* data = malloc( data_size );
    if ( !data )
        return;

    r = vkGetPipelineCacheData( vk.device, vk.pipeline_cache, &data_size, data );
    if ( r == VK_SUCCESS )
    {
        FILE* f = fopen( PIPELINE_CACHE_PATH, "wb" );
        if ( f )
        {
            fwrite( data, 1, data_size, f );
            fclose( f );
            LOG_INFO( "pipeline_cache_save: %zu bytes written", data_size );
        }
    }

    free( data );
}

/*============================================================================================*/
