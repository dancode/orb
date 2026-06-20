/*==============================================================================================

    win_sys.c : System information

==============================================================================================*/

u32
sys_cpu_count( void )
{
    SYSTEM_INFO si;
    GetSystemInfo( &si );
    return ( u32 )si.dwNumberOfProcessors;
}

u32
sys_physical_core_count( void )
{
    /* Walk LOGICAL_PROCESSOR_RELATIONSHIP for ProcessorCore */
    DWORD len = 0;
    GetLogicalProcessorInformation( NULL, &len );
    if ( len == 0 )
        return sys_cpu_count();

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buf =
        ( SYSTEM_LOGICAL_PROCESSOR_INFORMATION* )VirtualAlloc( NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
    if ( !buf )
        return sys_cpu_count();

    GetLogicalProcessorInformation( buf, &len );
    u32   count = 0;
    DWORD num   = len / sizeof( *buf );
    for ( DWORD i = 0; i < num; ++i )
        if ( buf[ i ].Relationship == RelationProcessorCore )
            ++count;
    VirtualFree( buf, 0, MEM_RELEASE );
    return count ? count : sys_cpu_count();
}

/*==============================================================================================

==============================================================================================*/

u32
sys_process_id( void )
{
    return ( u32 )GetCurrentProcessId();
}

/*==============================================================================================

==============================================================================================*/

void
sys_datetime_local( SysDateTime* dt )
{
    SYSTEMTIME st;
    GetLocalTime( &st );
    dt->year        = st.wYear;
    dt->month       = st.wMonth;
    dt->day         = st.wDay;
    dt->hour        = st.wHour;
    dt->minute      = st.wMinute;
    dt->second      = st.wSecond;
    dt->millisecond = st.wMilliseconds;
}

/*============================================================================================*/
