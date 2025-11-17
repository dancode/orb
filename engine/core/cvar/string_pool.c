/*==============================================================================================

    cvar system : string pool

================================================================================================

    Linear allocator for strings. All strings stored as contiguous bytes with u16 offsets
    for access. Pool can grow dynamically while maintaining offset validity.

    * Fast linear string pool.
    * Returns offsets into the pool.
    * No per-string malloc overhead.
    * Offsets remain valid even if pool reallocates (like vector growth).
    * Supports hot reload — since the offsets will stay valid.

==============================================================================================*/
/* Align value up to STRING_POOL_ALIGN boundary */

u32
string_pool_align_up( u32 value )
{
    const u32 a = STRING_POOL_ALIGN;
    return ( value + ( a - 1u ) ) & ~( a - 1u );
}

/*============================================================================================*/
/* Initialize string pool */

void
string_pool_init( string_pool_t* pool )
{
    if ( pool->data )
    {
        free( pool->data );
        pool->data = NULL;
    }

    pool->used     = 0;
    pool->capacity = 0;
    pool->maximum  = STRING_POOL_MAX_BYTES;

    /* create empty string at offset 0 */
    string_pool_ensure( pool, string_pool_align_up( STRING_POOL_ALIGN ) );
    if ( pool->data )
        pool->data[ pool->used ] = '\0';

    pool->used += string_pool_align_up( STRING_POOL_ALIGN );
}

void
string_pool_exit( string_pool_t* pool )
{
    // Free the fixed read only string pool data.
    if ( pool->data )
    {
        free( pool->data );
        pool->data = NULL;
    }

    pool->used = 0;
    pool->capacity = 0;
    pool->maximum  = 0;
}

/*============================================================================================*/
/* Ensure pool has capacity for allocation, returns current used offset */

u32
string_pool_ensure( string_pool_t* pool, u32 alloc_size )
{
    alloc_size = string_pool_align_up( alloc_size );

    /* Already have space */
    if ( pool->used + alloc_size <= pool->capacity )
    {
        return pool->used;
    }

    /* Check against maximum pool size */
    if ( pool->used + alloc_size > pool->maximum )
    {
        fprintf( stderr, "cvar: string pool exhausted (max %u bytes)\n", pool->maximum );
        exit( 1 );
    }

    /* Calculate new capacity (add 1024 until it fits) */
    u32 need = pool->used + alloc_size;
    u32 cap  = pool->capacity ? pool->capacity : 1024u;
    while ( cap < need ) cap += 1024u;

    /* Reallocate */
    char* new_data = ( char* )realloc( pool->data, cap );
    if ( new_data == NULL )
    {
        exit( 1 );
    }

    /* MODIFIED: Operates on 'pool' argument */
    pool->data     = new_data;
    pool->capacity = cap;

    return pool->used;
}

/*============================================================================================*/
/* Add string to pool, returns u32 offset (caller casts to u16) */

u32
string_pool_push( string_pool_t* pool, const char* str )
{
    /* Empty string is always at offset 0 */
    if ( !str || str[ 0 ] == '\0' )
        return 0;

    u32 len    = ( u32 )strlen( str ) + 1u;
    len        = string_pool_align_up( len );
    pool->used = string_pool_align_up( pool->used );

    /* Ensure capacity and get offset */
    u32 offset = string_pool_ensure( pool, len );

    /* Copy string */
    memcpy( pool->data + pool->used, str, len );
    pool->used += len;

    return offset;
}

/*============================================================================================*/
/* Reserve writable space in pool, returns offset */

u32
string_pool_reserve( string_pool_t* pool, u32 size )
{
    u32 offset = string_pool_ensure( pool, size );
    memset( pool->data + pool->used, 0, size );
    pool->used += size;
    return offset;
}

/*============================================================================================*/
/* Write string to previously reserved space (with truncation if needed) */

bool
string_pool_write( string_pool_t* pool, u32 offset, const char* str, u32 size )
{
    if ( offset >= pool->used )
        return false;

    if ( str == NULL )
        str = "";

    cu32 len = ( u32 )strlen( str ) + 1u; /* include null */
    if ( len > size )
    {
        /* Truncate */
        memcpy( pool->data + offset, str, size - 1 );
        pool->data[ offset + size - 1 ] = '\0';
        return false;
    }

    memcpy( pool->data + offset, str, len );
    return true;
}

/*============================================================================================*/
/* Get string from pool by offset */

const char*
string_pool_get( const string_pool_t* pool, u16 offset )
{
    assert( ( u32 )offset < pool->used );

    if ( offset == 0u )
        return "";

    if ( offset >= pool->used )
        return "";

    return pool->data + offset;
}

/*==============================================================================================

    cvar system : user string pool

================================================================================================

    cvar : user variable value string pool

    A destructive, pooled allocator for CVAR_USR strings.

    - Uses fixed-size buckets.
    - Maintains a free list for each bucket.
    - Allocates from the top if a free list is empty.
    - "Destructive" means strings can be freed and their memory reused.

==============================================================================================*/

/* Fixed-size buckets for user strings */
static const u32 g_user_bucket_sizes[ USER_STRING_BUCKET_COUNT ] = { 8, 16, 32, 64, 128, 256 };

user_string_pool_t g_user_string_pool;

/*============================================================================================*/
/* Initialize the user string pool */

void
user_string_pool_init( user_string_pool_t* usp )
{
    /* Initialize all free lists to empty (0xFFFF) */
    for ( int i = 0; i < USER_STRING_BUCKET_COUNT; ++i )
    {
        usp->free_list[ i ] = USER_STRING_INVALID_LIST;
    }
    string_pool_init( &usp->pool );
}

void
user_string_pool_exit( user_string_pool_t* usp )
{
    // Free the dynamic new user string pool data
    if ( usp->pool.data )
    {
        free( usp->pool.data );
        usp->pool.data = NULL;
    }
}

/*============================================================================================*/
/* Find the smallest fitting bucket for a size */

u16
user_string_get_bucket( u32 size )
{
    for ( int i = 0; i < USER_STRING_BUCKET_COUNT; ++i )
    {
        if ( size <= g_user_bucket_sizes[ i ] )
            return ( u16 )i;
    }
    /* If too large, use the largest bucket (truncation will occur) */
    return ( u16 )( USER_STRING_BUCKET_COUNT - 1 );
}

/*============================================================================================*/
/* Allocate a string from the user pool */

u16
user_string_pool_alloc( user_string_pool_t* usp, const char* str, u16* bucket_index )
{
    u32 len        = ( u32 )strlen( str ) + 1;

    *bucket_index  = user_string_get_bucket( len );

    u32 alloc_size = g_user_bucket_sizes[ *bucket_index ];
    u16 offset     = usp->free_list[ *bucket_index ];

    if ( offset != USER_STRING_INVALID_LIST )
    {
        /* Found a free block. Pull it from the free list. */
        /* The offset of the *next* free block is stored in the block itself. */
        usp->free_list[ *bucket_index ] = *( u16* )( usp->pool.data + offset );
    }
    else
    {
        /*  No free blocks ("allocate from top"). Reserve new space. */
        offset = ( u16 )string_pool_reserve( &usp->pool, alloc_size );
    }

    /* Write the string into the allocated block (with truncation) */
    string_pool_write( &usp->pool, offset, str, alloc_size );
    return offset;
}

/*============================================================================================*/
/* Free a string, adding it to its bucket's free list */

void
user_string_pool_free( user_string_pool_t* usp, u16 offset, u16 bucket_index )
{
    if ( offset == USER_STRING_INVALID_OFFSET || bucket_index >= USER_STRING_BUCKET_COUNT )
        return; /* Cannot free the invalid offset */

    /* Add to head of free list. Store the old head offset in our new block. */
    *( u16* )( usp->pool.data + offset ) = usp->free_list[ bucket_index ];
    usp->free_list[ bucket_index ]       = offset;
}

/*============================================================================================*/
/* Get a string from the user pool */

const char*
user_string_pool_get( const user_string_pool_t* usp, u16 offset )
{
    /* Just a wrapper around the generic string_pool_get */
    return string_pool_get( &usp->pool, offset );
}

/*============================================================================================*/