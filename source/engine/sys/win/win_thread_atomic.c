/*==============================================================================================

    win_thread_atomic.c : Win32 atomic operations implementation.

    Maps platform-independent sys_atomic_* functions to Windows Interlocked* compiler
    intrinsics. Enforces full memory barriers (acquire/release semantics) on x86/x64.

    Declare a: volatile i32 int_used_by_atomics = 0;

==============================================================================================*/

i32
sys_atomic_increment( volatile i32* target )
{
    // LONG is guaranteed to be a 32-bit signed integer on Windows.
    // InterlockedIncrement returns the resulting incremented value.
    return ( i32 )InterlockedIncrement( ( volatile LONG* )target );
}

i32
sys_atomic_decrement( volatile i32* target )
{
    // InterlockedDecrement returns the resulting decremented value.
    return ( i32 )InterlockedDecrement( ( volatile LONG* )target );
}

i32
sys_atomic_exchange_add( volatile i32* target, i32 value )
{
    // InterlockedExchangeAdd adds value and returns the original value before addition.
    return ( i32 )InterlockedExchangeAdd( ( volatile LONG* )target, ( LONG )value );
}

i32
sys_atomic_compare_exchange( volatile i32* target, i32 exchange, i32 comperand )
{
    // InterlockedCompareExchange sets target to exchange only if target == comperand.
    // Returns the original value of target.
    return ( i32 )InterlockedCompareExchange( ( volatile LONG* )target, ( LONG )exchange, ( LONG )comperand );
}

i32
sys_atomic_read( volatile i32* target )
{
    // Performs an atomic OR with 0 to read the value with an implicit hardware memory fence,
    // ensuring up-to-date visibility across cores.
    return ( i32 )InterlockedOr( ( volatile LONG* )target, 0 );
}

void
sys_atomic_write( volatile i32* target, i32 value )
{
    // InterlockedExchange writes the value atomically and returns the original value.
    // Enforces a hardware memory fence to immediately flush the write to other cores.
    InterlockedExchange( ( volatile LONG* )target, ( LONG )value );
}

i64
sys_atomic_exchange_add_64( volatile i64* target, i64 value )
{
    // InterlockedExchangeAdd64 adds value and returns the original value before addition.
    return ( i64 )InterlockedExchangeAdd64( ( volatile LONG64* )target, ( LONG64 )value );
}

i64
sys_atomic_compare_exchange_64( volatile i64* target, i64 exchange, i64 comperand )
{
    // InterlockedCompareExchange64 sets target to exchange only if target == comperand.
    // Returns the original value of target.
    return ( i64 )InterlockedCompareExchange64( ( volatile LONG64* )target, ( LONG64 )exchange,
                                                ( LONG64 )comperand );
}

i64
sys_atomic_read_64( volatile i64* target )
{
    // Atomic OR with 0 reads the value with an implicit hardware memory fence,
    // ensuring up-to-date visibility across cores.
    return ( i64 )InterlockedOr64( ( volatile LONG64* )target, 0 );
}

/*============================================================================================*/
