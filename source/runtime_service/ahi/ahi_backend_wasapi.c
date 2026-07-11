/*==============================================================================================

    runtime_service/ahi/ahi_backend_wasapi.c -- Windows WASAPI output backend.

    One dedicated audio thread owns everything: the COM apartment, the device, and the
    render loop.  The thread opens the device itself so no COM object ever crosses a
    thread boundary; ahi_backend_start() just spawns it and waits for the ready flag.

    Format: shared mode, event driven, f32 stereo at AHI_SAMPLE_RATE.  The
    AUTOCONVERTPCM flag makes WASAPI convert to whatever the endpoint actually runs at,
    so the mixer never sees hardware formats.  Requires Windows 10+.

    NO LOGGING in this file -- all of it runs on the audio thread.

==============================================================================================*/

#pragma comment( lib, "ole32.lib" )

#define AHI_WASAPI_BUFFER_MS 40    /* device buffer; wakeups arrive at roughly half this */

/* The WASAPI CLSIDs/IIDs are MIDL-declared extern; define them here instead of relying on
   uuid.lib contents varying across SDK versions. */
static const CLSID ahi_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID ahi_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID ahi_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID ahi_IID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

/*==============================================================================================
    State
==============================================================================================*/

static volatile i32 s_ahi_run;      // 1 while the render loop should live
static volatile i32 s_ahi_ready;    // 0 starting, 1 device up, -1 startup failed
static thread_t     s_ahi_thread;
static HANDLE       s_ahi_wake;     // device event; also signaled by stop() to unblock
static ahi_mix_fn   s_ahi_mix_cb;

/*==============================================================================================
    Audio thread
==============================================================================================*/

static void
ahi_wasapi_thread( void* arg )
{
    UNUSED( arg );

    IMMDeviceEnumerator* enm        = NULL;
    IMMDevice*           dev        = NULL;
    IAudioClient*        ac         = NULL;
    IAudioRenderClient*  rc         = NULL;
    UINT32               buf_frames = 0;
    BYTE*                data       = NULL;
    bool                 com        = false;
    bool                 started    = false;

    HRESULT hr = CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if ( FAILED( hr ) )
        goto done;
    com = true;

    hr = CoCreateInstance( &ahi_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                           &ahi_IID_IMMDeviceEnumerator, ( void** )&enm );
    if ( FAILED( hr ) )
        goto done;

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint( enm, eRender, eConsole, &dev );
    if ( FAILED( hr ) )
        goto done;

    hr = IMMDevice_Activate( dev, &ahi_IID_IAudioClient, CLSCTX_ALL, NULL, ( void** )&ac );
    if ( FAILED( hr ) )
        goto done;

    WAVEFORMATEX fmt    = { 0 };
    fmt.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels       = AHI_CHANNELS;
    fmt.nSamplesPerSec  = AHI_SAMPLE_RATE;
    fmt.wBitsPerSample  = 32;
    fmt.nBlockAlign     = ( WORD )( AHI_CHANNELS * sizeof( f32 ) );
    fmt.nAvgBytesPerSec = AHI_SAMPLE_RATE * fmt.nBlockAlign;

    hr = IAudioClient_Initialize( ac, AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  AHI_WASAPI_BUFFER_MS * 10000, 0, &fmt, NULL );
    if ( FAILED( hr ) )
        goto done;

    hr = IAudioClient_SetEventHandle( ac, s_ahi_wake );
    if ( FAILED( hr ) )
        goto done;

    hr = IAudioClient_GetBufferSize( ac, &buf_frames );
    if ( FAILED( hr ) )
        goto done;

    hr = IAudioClient_GetService( ac, &ahi_IID_IAudioRenderClient, ( void** )&rc );
    if ( FAILED( hr ) )
        goto done;

    /* Prefill the whole buffer so Start() never plays garbage. */
    if ( SUCCEEDED( IAudioRenderClient_GetBuffer( rc, buf_frames, &data ) ) )
    {
        s_ahi_mix_cb( ( f32* )data, buf_frames );
        IAudioRenderClient_ReleaseBuffer( rc, buf_frames, 0 );
    }

    hr = IAudioClient_Start( ac );
    if ( FAILED( hr ) )
        goto done;
    started = true;

    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );
    sys_atomic_write( &s_ahi_ready, 1 );

    /* Render loop: wake on the device event, top the buffer back up. */
    while ( sys_atomic_read( &s_ahi_run ) )
    {
        WaitForSingleObject( s_ahi_wake, 2000 );
        if ( !sys_atomic_read( &s_ahi_run ) )
            break;

        UINT32 pad = 0;
        if ( FAILED( IAudioClient_GetCurrentPadding( ac, &pad ) ) )
            break;    // device lost: fall silent until service restart

        UINT32 todo = buf_frames - pad;
        if ( !todo )
            continue;

        if ( FAILED( IAudioRenderClient_GetBuffer( rc, todo, &data ) ) )
            break;
        s_ahi_mix_cb( ( f32* )data, todo );
        IAudioRenderClient_ReleaseBuffer( rc, todo, 0 );
    }

done:
    if ( started )
        IAudioClient_Stop( ac );
    if ( rc )
        IAudioRenderClient_Release( rc );
    if ( ac )
        IAudioClient_Release( ac );
    if ( dev )
        IMMDevice_Release( dev );
    if ( enm )
        IMMDeviceEnumerator_Release( enm );
    if ( com )
        CoUninitialize();

    if ( sys_atomic_read( &s_ahi_ready ) == 0 )
        sys_atomic_write( &s_ahi_ready, -1 );    // never came up: report startup failure
}

/*==============================================================================================
    Backend seam
==============================================================================================*/

static bool
ahi_backend_start( ahi_mix_fn mix )
{
    s_ahi_mix_cb = mix;
    s_ahi_wake   = CreateEventA( NULL, FALSE, FALSE, NULL );
    if ( !s_ahi_wake )
        return false;

    sys_atomic_write( &s_ahi_ready, 0 );
    sys_atomic_write( &s_ahi_run, 1 );
    s_ahi_thread = thread_create( ahi_wasapi_thread, NULL, 0 );

    while ( sys_atomic_read( &s_ahi_ready ) == 0 ) thread_yield();

    if ( sys_atomic_read( &s_ahi_ready ) != 1 )
    {
        sys_atomic_write( &s_ahi_run, 0 );
        thread_join( s_ahi_thread );
        CloseHandle( s_ahi_wake );
        s_ahi_wake = NULL;
        return false;
    }
    return true;
}

static void
ahi_backend_stop( void )
{
    if ( !s_ahi_wake )
        return;

    sys_atomic_write( &s_ahi_run, 0 );
    SetEvent( s_ahi_wake );    // unblock the wait immediately
    thread_join( s_ahi_thread );
    CloseHandle( s_ahi_wake );
    s_ahi_wake = NULL;
}

/*============================================================================================*/
