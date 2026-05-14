/*==============================================================================================

    engine/app/win/win_fiber.c — Fiber-based message pump for modal loop bypass.

    Problem
    -------
    DefWindowProc enters an internal GetMessage loop during window resize, move, and
    menu operations.  That loop blocks until the interaction ends, stalling the game
    loop for the entire drag duration.

    Solution
    --------
    Two cooperative fibers share the main thread:

      fiber_main    — owns the game loop  (pump_events → frame → pump_events …)
      fiber_message — owns message dispatch (PeekMessage drain loop)

    pump_events switches to fiber_message.  A 1 ms WM_TIMER fires continuously; its
    TIMERPROC switches back to fiber_main so the game loop ticks once per timer
    interval, then returns to fiber_message.

    During DefWindowProc's internal modal GetMessage loop the timer still fires
    (modal loops dispatch all thread messages), so the game loop keeps running at
    ~1 ms cadence throughout a resize or move drag.

==============================================================================================*/

/* TIMERPROC — called by DispatchMessage when WM_TIMER is processed, even inside
   DefWindowProc's modal GetMessage loop.  Yields to the game loop for one frame. */
static VOID CALLBACK
win_fiber_timer_proc( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    (void)hwnd; (void)msg; (void)id; (void)time;
    if ( g_pool.fiber_main )
        SwitchToFiber( g_pool.fiber_main );
}

/* Message fiber body — drains messages then yields; suspended inside DefWindowProc
   during modal ops, periodically woken by the timer callback. */
static VOID CALLBACK
win_fiber_message_proc( PVOID param )
{
    (void)param;
    for ( ;; )
    {
        MSG msg;
        while ( PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ) )
        {
            if ( msg.message == WM_QUIT )
                g_app_quit = true;
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        /* Queue drained — yield to game loop. */
        SwitchToFiber( g_pool.fiber_main );
    }
}

static void
win_fiber_init( void )
{
    g_pool.fiber_main    = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber( NULL );
    g_pool.fiber_message = CreateFiber( 0, win_fiber_message_proc, NULL );
    /* Thread-level timer (hwnd = NULL): fires into the calling thread's message
       queue so it reaches DefWindowProc's modal loop without needing a window. */
    SetTimer( NULL, APP_FIBER_TIMER_ID, 1, win_fiber_timer_proc );
}

static void
win_fiber_exit( void )
{
    KillTimer( NULL, APP_FIBER_TIMER_ID );

    if ( g_pool.fiber_message )
    {
        DeleteFiber( g_pool.fiber_message );
        g_pool.fiber_message = NULL;
    }

    if ( g_pool.fiber_main && IsThreadAFiber() )
    {
        ConvertFiberToThread();
        g_pool.fiber_main = NULL;
    }
}

/*============================================================================================*/
