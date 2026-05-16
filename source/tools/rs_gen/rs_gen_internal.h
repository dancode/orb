#ifndef RS_GEN_INTERNAL_H
#define RS_GEN_INTERNAL_H
/*==============================================================================================

    rs_gen_internal.h - internal types and declarations for the rs_gen tool

    rs_gen is a standalone C11 tool (no engine deps, no orb.h).
    It scans annotated .h files and generates rs_-compatible registration code.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*==============================================================================================
    Limits
==============================================================================================*/

#define RG_MAX_FILES   256
#define RG_MAX_PATH    512
#define RG_MAX_NAME    128

/*==============================================================================================
    File list
==============================================================================================*/

typedef struct rg_file_list_s
{
    char paths[ RG_MAX_FILES ][ RG_MAX_PATH ];
    int  count;

} rg_file_list_t;

/*==============================================================================================
    Parsed data  (phase 1 stub: just counts; full AST comes later)
==============================================================================================*/

typedef struct rg_parse_data_s
{
    int type_count;
    int enum_count;

} rg_parse_data_t;

/*==============================================================================================
    std
==============================================================================================*/

void rg_str_copy( char* dst, const char* src, int max );
int  rg_str_len( const char* s );
void rg_str_cat( char* dst, const char* src, int max );
int  rg_str_ends_with( const char* s, const char* suffix );

/*==============================================================================================
    platform
==============================================================================================*/

void rg_platform_mkdir( const char* path );
int  rg_platform_scan_dir( const char* dir, char out_paths[][ RG_MAX_PATH ], int max_files );
void rg_platform_exe_dir( char* out, int max );  // directory containing the running executable

/*==============================================================================================
    scan
==============================================================================================*/

void rg_scan( const char* source_dir, rg_file_list_t* out );

/*==============================================================================================
    parse
==============================================================================================*/

void rg_parse( const rg_file_list_t* files, rg_parse_data_t* out );

/*==============================================================================================
    output
==============================================================================================*/

int rg_output( const char* output_dir, const char* module_name, const rg_parse_data_t* data );

/*============================================================================================*/
#endif // RS_GEN_INTERNAL_H
