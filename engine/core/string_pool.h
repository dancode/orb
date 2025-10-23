#ifndef STRING_POOL_HEADER_H
#define STRING_POOL_HEADER_H

/*==============================================================================================

    string pool

==============================================================================================*/
// clang-format off

#define STRING_POOL_MAX_BYTES       0xFFFEu    // Max pool size (fits in u16)
#define STRING_POOL_ALIGN           4          // Alignment for pool allocations

u32         string_pool_align_up    ( u32 value );
u32         string_pool_ensure      ( string_pool_t* pool, u32 alloc_size );
u32         string_pool_push        ( string_pool_t* pool, const char* str );
u32         string_pool_reserve     ( string_pool_t* pool, u32 size );
bool        string_pool_write       ( string_pool_t* pool, u32 offset, const char* str, u32 size );
const char* string_pool_get         ( const string_pool_t* pool, u16 offset );
void        string_pool_init        ( string_pool_t* pool, u32 maximum );

#define USER_STRING_BUCKET_COUNT   6          // Number of different size buckets
#define USER_STRING_INVALID_OFFSET 0u         // Offset 0 is reserved for ""
#define USER_STRING_INVALID_LIST   0xFFFFu    // Invalid free list index

void        user_string_pool_init   ();
u16         user_string_get_bucket  ( u32 size );
u16         user_string_pool_alloc  ( const char* str, u16* bucket_index );
void        user_string_pool_free   (u16 offset, u16 bucket_index );
const char* user_string_pool_get    ( u16 offset );

/*============================================================================================*/
// clang-format on

#endif    // STRING_POOL_HEADER_H