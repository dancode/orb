/*==============================================================================================

    win_crash.c : Fatal exception capture -- SEH filter, minidump, backtrace, symbolization.

    Mechanism only; report formatting and file policy live in engine/core/debug/crash.c.
    dbghelp calls happen inside an already-crashed process, which Microsoft supports for
    exactly this use case (MiniDumpWriteDump / StackWalk64 on the faulting thread).

==============================================================================================*/

/*==============================================================================================
    State
==============================================================================================*/

static sys_crash_fn  s_crash_cb;
static void*         s_crash_user;
static volatile LONG s_crash_entered;    // first faulting thread wins; others park
static bool          s_crash_sym_ready;

/*==============================================================================================
    Symbol engine (lazy)
==============================================================================================*/

static void
crash_sym_init( void )
{
    if ( s_crash_sym_ready )
        return;
    SymSetOptions( SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME |
                   SYMOPT_FAIL_CRITICAL_ERRORS );
    SymInitialize( GetCurrentProcess(), NULL, TRUE );
    s_crash_sym_ready = true;
}

/*==============================================================================================
    Exception code names
==============================================================================================*/

const char*
sys_crash_code_str( u32 code )
{
    switch ( code )
    {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:          return "FLT_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_GUARD_PAGE:            return "GUARD_PAGE";
        default:                              return "UNKNOWN_EXCEPTION";
    }
}

/*==============================================================================================
    Minidump
==============================================================================================*/

bool
sys_crash_minidump( const char* path, const sys_crash_info_t* info )
{
    HANDLE file = CreateFileA( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if ( file == INVALID_HANDLE_VALUE )
        return false;

    MINIDUMP_EXCEPTION_INFORMATION  mei;
    MINIDUMP_EXCEPTION_INFORMATION* meip = NULL;
    if ( info && info->os_exception )
    {
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ( EXCEPTION_POINTERS* )info->os_exception;
        mei.ClientPointers    = FALSE;
        meip                  = &mei;
    }

    /* Small dump plus thread/module context -- enough to open in VS and see every stack. */
    MINIDUMP_TYPE type = ( MINIDUMP_TYPE )( MiniDumpNormal | MiniDumpWithThreadInfo |
                                            MiniDumpWithUnloadedModules |
                                            MiniDumpWithIndirectlyReferencedMemory );

    BOOL ok = MiniDumpWriteDump( GetCurrentProcess(), GetCurrentProcessId(), file, type, meip, NULL, NULL );
    CloseHandle( file );
    return ok != FALSE;
}

/*==============================================================================================
    Backtrace
==============================================================================================*/

int
sys_crash_backtrace( const sys_crash_info_t* info, void** frames, int max )
{
    if ( max <= 0 )
        return 0;

    /* No exception context: cheap capture of the calling thread. */
    if ( !info || !info->os_exception )
        return ( int )RtlCaptureStackBackTrace( 1, ( ULONG )max, frames, NULL );

    /* Walk from the faulting CONTEXT so the trace starts at the crash site,
       not inside the exception dispatcher. StackWalk64 mutates its context. */
    crash_sym_init();
    CONTEXT ctx = *( ( EXCEPTION_POINTERS* )info->os_exception )->ContextRecord;

    STACKFRAME64 sf;
    memset( &sf, 0, sizeof( sf ) );
    DWORD machine;
#if defined( _M_X64 )
    machine             = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset    = ctx.Rip;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrStack.Offset = ctx.Rsp;
#elif defined( _M_ARM64 )
    machine             = IMAGE_FILE_MACHINE_ARM64;
    sf.AddrPC.Offset    = ctx.Pc;
    sf.AddrFrame.Offset = ctx.Fp;
    sf.AddrStack.Offset = ctx.Sp;
#else
#    error "win_crash.c: unsupported architecture"
#endif
    sf.AddrPC.Mode    = AddrModeFlat;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Mode = AddrModeFlat;

    HANDLE proc   = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    int    n      = 0;
    while ( n < max && StackWalk64( machine, proc, thread, &sf, &ctx, NULL, SymFunctionTableAccess64,
                                    SymGetModuleBase64, NULL ) )
    {
        if ( sf.AddrPC.Offset == 0 )
            break;
        frames[ n++ ] = ( void* )( usize )sf.AddrPC.Offset;
    }
    return n;
}

/*==============================================================================================
    Symbolization
==============================================================================================*/

bool
sys_crash_symbolize( void* addr, char* out, int out_size )
{
    crash_sym_init();

    HANDLE  proc = GetCurrentProcess();
    DWORD64 a    = ( DWORD64 )( usize )addr;

    IMAGEHLP_MODULE64 mod;
    memset( &mod, 0, sizeof( mod ) );
    mod.SizeOfStruct = sizeof( mod );
    const char* mod_name = SymGetModuleInfo64( proc, a, &mod ) ? mod.ModuleName : "?";

    u8           sym_buf[ sizeof( SYMBOL_INFO ) + 256 ];
    SYMBOL_INFO* sym  = ( SYMBOL_INFO* )sym_buf;
    sym->SizeOfStruct = sizeof( SYMBOL_INFO );
    sym->MaxNameLen   = 255;

    DWORD64 disp = 0;
    if ( !SymFromAddr( proc, a, &disp, sym ) )
    {
        snprintf( out, ( size_t )out_size, "%s!0x%016llx", mod_name, ( unsigned long long )a );
        return false;
    }

    IMAGEHLP_LINE64 line;
    memset( &line, 0, sizeof( line ) );
    line.SizeOfStruct = sizeof( line );
    DWORD line_disp   = 0;
    if ( SymGetLineFromAddr64( proc, a, &line_disp, &line ) )
        snprintf( out, ( size_t )out_size, "%s!%s + 0x%llx  [%s:%lu]", mod_name, sym->Name,
                  ( unsigned long long )disp, line.FileName, ( unsigned long )line.LineNumber );
    else
        snprintf( out, ( size_t )out_size, "%s!%s + 0x%llx", mod_name, sym->Name,
                  ( unsigned long long )disp );
    return true;
}

/*==============================================================================================
    Unhandled exception filter
==============================================================================================*/

static LONG WINAPI
crash_unhandled_filter( EXCEPTION_POINTERS* ep )
{
    /* First crasher wins. A second faulting thread parks here until the report
       finishes and process termination kills it. */
    if ( InterlockedCompareExchange( &s_crash_entered, 1, 0 ) != 0 )
    {
        Sleep( INFINITE );
    }

    crash_sym_init();

    if ( s_crash_cb )
    {
        sys_crash_info_t info;
        info.code         = ( u32 )ep->ExceptionRecord->ExceptionCode;
        info.address      = ep->ExceptionRecord->ExceptionAddress;
        info.os_exception = ep;
        s_crash_cb( &info, s_crash_user );
    }

    /* Under a debugger, fall through so it breaks at the faulting instruction. */
    if ( IsDebuggerPresent() )
        return EXCEPTION_CONTINUE_SEARCH;

    return EXCEPTION_EXECUTE_HANDLER;    // die quietly: no WER dialog after the report
}

void
sys_crash_install( sys_crash_fn cb, void* user )
{
    s_crash_cb   = cb;
    s_crash_user = user;

    /* Reserve handler stack so the filter can run after EXCEPTION_STACK_OVERFLOW
       (installing thread only -- worker threads crash with whatever they have left). */
    ULONG guarantee = 32 * 1024;
    SetThreadStackGuarantee( &guarantee );

    SetUnhandledExceptionFilter( crash_unhandled_filter );
}

/*============================================================================================*/
