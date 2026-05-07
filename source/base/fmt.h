/*==============================================================================================

    fmt.h -- Formatted output utilities

==============================================================================================*/
#ifndef FMT_H
#define FMT_H
/*============================================================================================*/

i32 fmt_vbuf( char* buf, usize buf_cap, const char* fmt, va_list args );
i32 fmt_append( char* buf, usize buf_cap, i32 offset, const char* fmt, ... );
i32 fmt_i64( char* buf, usize buf_cap, i64 v );
i32 fmt_u64( char* buf, usize buf_cap, u64 v );
i32 fmt_hex64( char* buf, usize buf_cap, u64 v );

/*============================================================================================*/
#endif    // FMT_H
