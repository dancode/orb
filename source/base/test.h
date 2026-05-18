#pragma once
/*==============================================================================================

    base/test.h -- Lightweight single-file test framework for the base library sandbox.

    Usage in a test function:
        test_true( char_is_alpha( 'A' ) );
        test_equal( 5, str_len( "hello" ) );
        test_str_equal( "hello", buf );

    Usage in base_run_tests():
        test_register( "char_classify", test_char_classify );
        return test_run();

==============================================================================================*/
#include <stdio.h>
#include <stdbool.h>

/*==============================================================================================
    State  (static so each TU gets its own copy)
==============================================================================================*/

typedef struct
{
    const char* name;
    void ( *fn )( void );
} test_case_t;

static test_case_t s_cases[ 128 ];
static i32         s_case_count  = 0;
static i32         s_pass_count  = 0;
static i32         s_fail_count  = 0;
static bool        s_case_failed = false;

/*==============================================================================================
    Internal assertion core
==============================================================================================*/

#define _TEST_ASSERT( condition, format, ... )                                  \
    do {                                                                        \
        __pragma( warning( suppress : 4127 ) )                                 \
        if ( !( condition ) )                                                   \
        {                                                                       \
            if ( !s_case_failed )                                               \
                printf( "\n" );                                                 \
            printf( "    %s(%d): " format, __FILE__, __LINE__, ##__VA_ARGS__ ); \
            s_case_failed = true;                                               \
        }                                                                       \
    }                                                                           \
    while ( 0 )

/*==============================================================================================
    Named assertion macros
==============================================================================================*/

#define test_assert( condition ) _TEST_ASSERT( ( condition ), "%s\n", #condition )

#define test_true( condition )   _TEST_ASSERT( ( condition ), "expected true:     %s\n", #condition )

#define test_false( condition )  _TEST_ASSERT( !( condition ), "expected false:    %s\n", #condition )

#define test_null( pointer )     _TEST_ASSERT( ( pointer ) == NULL, "expected NULL:     %s\n", #pointer )

#define test_not_null( pointer ) _TEST_ASSERT( ( pointer ) != NULL, "expected non-NULL: %s\n", #pointer )

#define test_equal( expected, actual )                                                    \
    _TEST_ASSERT( ( long long )( expected ) == ( long long )( actual ),                   \
                  "expected %s == %s  [expected: %lld, got: %lld]\n", #expected, #actual, \
                  ( long long )( expected ), ( long long )( actual ) )

#define test_not_equal( expected, actual ) \
    _TEST_ASSERT( ( expected ) != ( actual ), "expected %s != %s\n", #expected, #actual )

#define test_str_equal( expected, actual )                                                \
    _TEST_ASSERT( str_equal( ( expected ), ( actual ) ), "str_equal: \"%s\" != \"%s\"\n", \
                  ( expected ) ? ( expected ) : "(null)", ( actual ) ? ( actual ) : "(null)" )

/*==============================================================================================
    Registration and runner
==============================================================================================*/

static void
test_register( const char* name, void ( *fn )( void ) )
{
    i32 max = ( i32 )( sizeof( s_cases ) / sizeof( s_cases[ 0 ] ) );
    if ( s_case_count < max )
    {
        s_cases[ s_case_count ].name = name;
        s_cases[ s_case_count ].fn   = fn;
        s_case_count++;
    }
    else
    {
        printf( "warning: test case limit reached (%d)\n", max );
    }
}

static i32
test_run( const char* suite_name )
{
    printf( "\n" );
    printf( "---------------------------------------------\n" );
    printf( " %s tests: %d cases\n", suite_name, s_case_count );
    printf( "---------------------------------------------\n" );

    for ( i32 i = 0; i < s_case_count; i++ )
    {
        s_case_failed = false;
        printf( "  %-30s", s_cases[ i ].name );
        s_cases[ i ].fn();
        if ( s_case_failed )
        {
            printf( "  [FAIL]\n" );
            s_fail_count++;
        }
        else
        {
            printf( "[PASS]\n" );
            s_pass_count++;
        }
    }

    printf( "---------------------------------------------\n" );
    printf( " passed: %d   failed: %d\n", s_pass_count, s_fail_count );
    printf( "---------------------------------------------\n" );
    return s_fail_count;
}

/*============================================================================================*/
