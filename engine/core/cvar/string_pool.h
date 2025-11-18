#ifndef STRING_POOL_HEADER_H
#define STRING_POOL_HEADER_H

// clang-format off
/*==============================================================================================

    string pool

==============================================================================================*/

bool        str_icmp_eq             ( const char* a, const char* b );

/*============================================================================================*/

#define             STRING_POOL_MAX_BYTES       0xFFFEu    // Max pool size (fits in u16)
#define             STRING_POOL_ALIGN           4          // Alignment for pool allocations

typedef struct string_pool_s
{
    char*           data;                   // Linear heap for all strings
    u32             used;                   // Bytes currently used
    u32             capacity;               // Bytes allocated
    u32             maximum;                // Maximum bytes allowed (0xFFFE)

} string_pool_t;

/*============================================================================================*/

u32                 string_pool_align_up    ( u32 value );
void                string_pool_exit        ( string_pool_t* pool );
u32                 string_pool_ensure      ( string_pool_t* pool, u32 alloc_size );
u32                 string_pool_push        ( string_pool_t* pool, const char* str );
u32                 string_pool_reserve     ( string_pool_t* pool, u32 size );
bool                string_pool_write       ( string_pool_t* pool, u32 offset, const char* str, u32 size );
const char*         string_pool_get         ( const string_pool_t* pool, u16 offset );
void                string_pool_init        ( string_pool_t* pool );

/*============================================================================================*/

#define             USER_STRING_BUCKET_COUNT   6          // Number of different size buckets
#define             USER_STRING_INVALID_OFFSET 0u         // Offset 0 is reserved for ""
#define             USER_STRING_INVALID_LIST   0xFFFFu    // Invalid free list index

typedef struct user_string_pool_s
{
    string_pool_t   pool;
    u16             free_list[ USER_STRING_BUCKET_COUNT ];

} user_string_pool_t;

/*============================================================================================*/

void                user_string_pool_init   ( user_string_pool_t* usp );
void                user_string_pool_exit   ( user_string_pool_t* usp );
u16                 user_string_get_bucket  ( u32 size );
u16                 user_string_pool_alloc  ( user_string_pool_t* usp, const char* str, u16* bucket_index );
void                user_string_pool_free   ( user_string_pool_t* usp, u16 offset, u16 bucket_index );
const char*         user_string_pool_get    ( const user_string_pool_t* usp, u16 offset );

/*============================================================================================*/
// clang-format on

#endif    // STRING_POOL_HEADER_H
