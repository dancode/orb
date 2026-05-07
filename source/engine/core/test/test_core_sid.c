/*==============================================================================================

    SID : Test Intern System

==============================================================================================*/

int
intern_test( void )
{
    /**************************************************************/
    // Test: Init and Shutdown
    /**************************************************************/
    {
        sid_init();
        sid_exit();
    }
    /**************************************************************/
    // Test: Case Inensitive and Canonical
    /**************************************************************/
    {
        sid_init();

        const char* s    = "Transform";
        const char* sl   = "transform";
        const char* su   = "TRANSFORM";
        const int   len  = 9;

        sid_t       sid1 = sid_intern_cstr( s );
        sid_t       sid2 = sid_intern_cstr( sl );
        sid_t       sid3 = sid_intern( su, len );

        assert( !sid_equals( sid1, SID_INVALID ) );
        assert( sid_equals( sid1, sid2 ) );
        assert( sid_equals( sid2, sid3 ) );

        assert( strcmp( sid_cstr( sid1 ), s ) == 0 );
        assert( sid_is_canonical( sid1, s, ( size_t )len ) );
        assert( !sid_is_canonical( sid1, sl, ( size_t )len ) );
        assert( !sid_is_canonical( sid1, su, ( size_t )len ) );

        sid_exit();
    }
    /**************************************************************/
    // Test: Mixed Case In Middle of String
    /**************************************************************/
    {
        sid_init();

        sid_t s1 = sid_intern_cstr( "MixedCaseString" );
        sid_t s2 = sid_intern_cstr( "mixedcasestring" );
        sid_t s3 = sid_intern_cstr( "MIXEDCASESTRING" );

        assert( sid_equals( s1, s2 ) && sid_equals( s2, s3 ) );
        assert( strcmp( sid_cstr( s1 ), "MixedCaseString" ) == 0 );

        sid_exit();
    }
    /**************************************************************/
    // Test: Special Characters
    /**************************************************************/
    {
        sid_init();

        const char* specials[] = { "test_123",   "test-with-dashes", "test.with.dots", "test/path",
                                   "test\\path", "test:colon",       "test@symbol",    "test#hash" };

        for ( size_t i = 0; i < sizeof( specials ) / sizeof( specials[ 0 ] ); i++ )
        {
            sid_t s = sid_intern_cstr( specials[ i ] );
            assert( !sid_equals( s, SID_INVALID ) );
            assert( strcmp( sid_cstr( s ), specials[ i ] ) == 0 );
        }

        sid_exit();
    }
    /**************************************************************/
    // Test: Single Character Strings
    /**************************************************************/
    {
        sid_init();

        sid_t s1 = sid_intern_cstr( "A" );
        sid_t s2 = sid_intern_cstr( "a" );

        assert( sid_equals( s1, s2 ) );
        assert( strcmp( sid_cstr( s1 ), "A" ) == 0 );
        assert( sid_length( s1 ) == 1 );

        sid_exit();
    }
    /**************************************************************/
    // Test: Accessors and Equals
    /**************************************************************/
    {
        sid_init();

        sid_t t = sid_intern_cstr( "type" );
        sid_t p = sid_intern_cstr( "position" );

        assert( sid_equals( t, t ) );
        assert( !sid_equals( t, p ) );

        assert( sid_length( t ) == ( uint8_t )strlen( "type" ) );
        assert( sid_length( p ) == ( uint8_t )strlen( "position" ) );

        const char* invalid = sid_cstr( SID_INVALID );
        assert( invalid != NULL );
        assert( invalid[ 0 ] == '\0' );

        sid_exit();
    }
    /**************************************************************/
    // Test: String Boundary Length
    /**************************************************************/
    {
        sid_init();

        char buf[ 256 ];
        memset( buf, 'a', 255 );
        buf[ 255 ] = '\0';

        sid_t s    = sid_intern( buf, 255 );
        assert( !sid_equals( s, SID_INVALID ) );
        assert( sid_length( s ) == 255 );

        const char* stored = sid_cstr( s );
        assert( stored != NULL );
        assert( memcmp( stored, buf, 255 ) == 0 );

        // Note: Do NOT test len 0 or >255 here, as implementation asserts on invalid lengths.

        sid_exit();
    }
    /**************************************************************/
    // Test: Hash Functions
    /**************************************************************/
    {
        sid_init();

        const char* a  = "AbC";
        const char* b  = "aBc";
        uint32_t    h1 = sid_hash( a );
        uint32_t    h2 = sid_hash( b );
        assert( h1 == h2 );

        const char* samples[] = { "transform", "Component", "POSITION", "Key_0123" };
        for ( size_t i = 0; i < sizeof( samples ) / sizeof( samples[ 0 ] ); ++i )
        {
            const char* s  = samples[ i ];
            uint32_t    hA = sid_hash( s );
            uint32_t    hB = sid_hash_len( s, strlen( s ) );
            assert( hA == hB );
        }

        sid_exit();
    }
    /**************************************************************/
    // Test: Rehashing and Arena Grow
    /**************************************************************/
    {
        sid_init();

        enum
        {
            COUNT = 600    // > 0.7 * 512 to trigger at least one rehash
        };

        char  key[ 32 ];
        sid_t samples[ 5 ]    = { 0 };
        int   sample_idx[ 5 ] = { 0, 1, 123, 357, 599 };

        for ( int i = 0; i < COUNT; ++i )
        {
            snprintf( key, sizeof( key ), "key_%04d", i );
            sid_t s = sid_intern_cstr( key );
            assert( !sid_equals( s, SID_INVALID ) );

            // capture a few sample SIDs to validate stability across growth
            for ( size_t k = 0; k < sizeof( sample_idx ) / sizeof( sample_idx[ 0 ] ); ++k )
            {
                if ( i == sample_idx[ k ] )
                    samples[ k ] = s;
            }
        }

        // Re-intern with different cases to verify case-insensitive lookup and canonical storage
        for ( size_t k = 0; k < sizeof( sample_idx ) / sizeof( sample_idx[ 0 ] ); ++k )
        {
            int idx = sample_idx[ k ];
            snprintf( key, sizeof( key ), "KEY_%04d", idx );    // different case
            sid_t again = sid_intern_cstr( key );
            assert( sid_equals( again, samples[ k ] ) );

            // verify canonical matches first-inserted lowercase "key_xxxx"
            char canonical[ 32 ];
            snprintf( canonical, sizeof( canonical ), "key_%04d", idx );
            assert( strcmp( sid_cstr( again ), canonical ) == 0 );
            assert( sid_is_canonical( again, canonical, strlen( canonical ) ) );
        }

        // Exercise stats code paths (cannot assert exact values without internal access)
        sid_print_stats( stdout );
        sid_reset_stats();

        sid_exit();
    }
    /**************************************************************/
    // Test: Verify hash collision handling
    /**************************************************************/
    {
        sid_init();

        // Find strings that hash to same bucket (if possible)
        // Or just test many strings to ensure collisions work
        sid_t sids[ 1000 ];
        char  key[ 32 ];
        for ( int i = 0; i < 1000; i++ )
        {
            snprintf( key, sizeof( key ), "test_%d", i );
            sids[ i ] = sid_intern_cstr( key );
            assert( !sid_equals( sids[ i ], SID_INVALID ) );
        }

        // Verify all are unique and retrievable
        for ( int i = 0; i < 1000; i++ )
        {
            snprintf( key, sizeof( key ), "test_%d", i );
            sid_t lookup = sid_intern_cstr( key );
            assert( sid_equals( lookup, sids[ i ] ) );
        }

        sid_print_stats( stdout );
        sid_reset_stats();

        sid_exit();
    }
    /**************************************************************/
    // Test: Print statistics
    /**************************************************************/
    {
        sid_init();
        sid_print_stats( stdout );
        sid_exit();
    }
    /**************************************************************/
    // Test: Test usage example
    /**************************************************************/
    {
        sid_init();
        hash_perf_test();
        sid_exit();
    }

    return true;
}

/*============================================================================================*/