/*==============================================================================================

    engine/app/win/win_dpi.c -- Per-monitor DPI awareness.

    The process declares Per-Monitor V2 awareness before the first window is created, so
    Windows stops bitmap-stretching the client area on scaled monitors and instead delivers
    WM_DPICHANGED when a window's monitor scale changes.  Whether the engine *responds* to
    the reported scale is ui-side policy (gui rescales its metrics, or ignores DPI and stays
    at 1:1) -- the app layer only reports faithfully.

    All post-Vista DPI entry points are resolved from user32 at runtime (the same pattern as
    the Vulkan loader): a static import would keep the exe from starting on systems older
    than Win10 1607/1703.  Every helper degrades to the classic system-DPI call when the
    per-monitor variant is unavailable, so behavior on old systems is unchanged.

==============================================================================================*/

#define APP_DPI_BASE 96 /* USER_DEFAULT_SCREEN_DPI -- 100% scale */

/* DPI_AWARENESS_CONTEXT is a pseudo-handle; -4 = PER_MONITOR_AWARE_V2.  Declared locally so
   this compiles against any SDK vintage. */
typedef void* win_dpi_ctx_t;
#define WIN_DPI_CTX_PER_MONITOR_V2 ( ( win_dpi_ctx_t )-4 )

typedef BOOL( WINAPI* win_fn_set_dpi_ctx_t )( win_dpi_ctx_t );
typedef UINT( WINAPI* win_fn_dpi_for_window_t )( HWND );
typedef UINT( WINAPI* win_fn_dpi_for_system_t )( void );
typedef int( WINAPI* win_fn_metrics_for_dpi_t )( int, UINT );
typedef BOOL( WINAPI* win_fn_adjust_for_dpi_t )( LPRECT, DWORD, BOOL, DWORD, UINT );

static struct
{
    bool                     resolved;        // win_dpi_boot ran (pointers valid or NULL)
    win_fn_dpi_for_window_t  dpi_for_window;  // GetDpiForWindow        (1607+)
    win_fn_dpi_for_system_t  dpi_for_system;  // GetDpiForSystem        (1607+)
    win_fn_metrics_for_dpi_t metrics_for_dpi; // GetSystemMetricsForDpi (1607+)
    win_fn_adjust_for_dpi_t  adjust_for_dpi;  // AdjustWindowRectExForDpi (1607+)

} g_dpi;

/* Declare Per-Monitor V2 awareness and resolve the per-monitor metric entry points.
   Runs once, before the first window exists (awareness cannot change after that). */
static void
win_dpi_boot( void )
{
    if ( g_dpi.resolved )
        return;
    g_dpi.resolved = true;

    HMODULE user32 = GetModuleHandleW( L"user32.dll" );
    if ( !user32 )
        return;

    g_dpi.dpi_for_window  = ( win_fn_dpi_for_window_t )GetProcAddress( user32, "GetDpiForWindow" );
    g_dpi.dpi_for_system  = ( win_fn_dpi_for_system_t )GetProcAddress( user32, "GetDpiForSystem" );
    g_dpi.metrics_for_dpi = ( win_fn_metrics_for_dpi_t )GetProcAddress( user32, "GetSystemMetricsForDpi" );
    g_dpi.adjust_for_dpi  = ( win_fn_adjust_for_dpi_t )GetProcAddress( user32, "AdjustWindowRectExForDpi" );

    win_fn_set_dpi_ctx_t set_ctx =
        ( win_fn_set_dpi_ctx_t )GetProcAddress( user32, "SetProcessDpiAwarenessContext" );

    if ( set_ctx )
        set_ctx( WIN_DPI_CTX_PER_MONITOR_V2 ); /* Win10 1703+ */
    else
        SetProcessDPIAware(); /* Vista+ system-DPI awareness -- no per-monitor messages */
}

/* The window's current monitor DPI (96 = 100%).  APP_DPI_BASE when unavailable. */
static u32
win_dpi_for_window( HWND hwnd )
{
    if ( g_dpi.dpi_for_window && hwnd )
    {
        UINT dpi = g_dpi.dpi_for_window( hwnd );
        if ( dpi )
            return ( u32 )dpi;
    }
    return APP_DPI_BASE;
}

/* System (primary-monitor) DPI -- the best guess before a window exists. */
static u32
win_dpi_system( void )
{
    if ( g_dpi.dpi_for_system )
    {
        UINT dpi = g_dpi.dpi_for_system();
        if ( dpi )
            return ( u32 )dpi;
    }
    return APP_DPI_BASE;
}

/* GetSystemMetrics scaled for the given DPI, falling back to the system-DPI value. */
static i32
win_dpi_metric( int index, u32 dpi )
{
    if ( g_dpi.metrics_for_dpi )
        return g_dpi.metrics_for_dpi( index, ( UINT )dpi );
    return GetSystemMetrics( index );
}

/* AdjustWindowRectEx scaled for the given DPI, falling back to the classic call. */
static void
win_dpi_adjust_rect( RECT* rect, DWORD style, DWORD ex_style, u32 dpi )
{
    if ( g_dpi.adjust_for_dpi )
        g_dpi.adjust_for_dpi( rect, style, FALSE, ex_style, ( UINT )dpi );
    else
        AdjustWindowRectEx( rect, style, FALSE, ex_style );
}

/*============================================================================================*/
