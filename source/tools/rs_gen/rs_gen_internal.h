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

#define RG_MAX_FILES           256
#define RG_MAX_PATH            512
#define RG_MAX_NAME            128

#define RG_MAX_TYPES           128 /* types per module                     */
#define RG_MAX_FIELDS_PER_TYPE 64  /* fields per struct                    */
#define RG_MAX_ENUMS_PER_TYPE  128 /* enums per enum/bitset                */
#define RG_MAX_ATTRS_PER_ITEM  8   /* attributes per field/type            */
#define RG_MAX_API_FUNCS       64  /* RS_API() functions per module        */
#define RG_MAX_PARAM_STR       512 /* param list string per API function   */

/*==============================================================================================
    File list
==============================================================================================*/

/* Used to pass scan results from scan to parse, and to track which headers actually
   contained RS_ declarations for output includes. */

typedef struct rg_file_list_s
{
    char paths[ RG_MAX_FILES ][ RG_MAX_PATH ];
    int  count;
    char source_dir[ RG_MAX_PATH ]; /* directory passed to rg_scan, reused for .c pass */

} rg_file_list_t;

/*==============================================================================================
    Parsed AST
==============================================================================================*/

typedef enum rg_attr_kind_e
{
    RG_ATTR_TAG    = 0,    // bare identifier:  transient
    RG_ATTR_INT    = 1,    // integer literal:  range=0
    RG_ATTR_FLOAT  = 2,    // float literal:    weight=0.5
    RG_ATTR_STRING = 3,    // quoted string:    tooltip="text"

} rg_attr_kind_t;

/*--------------------------------------------------------------------------------------------*/

typedef struct rg_attr_s
{
    int  kind;                    // rg_attr_kind_t
    char name[ RG_MAX_NAME ];     //
    char value[ RG_MAX_NAME ];    // verbatim literal (without quotes)

} rg_attr_t;

/*--------------------------------------------------------------------------------------------*/

typedef struct rg_decl_field_s
{
    char      name[ RG_MAX_NAME ];
    char      base_type[ RG_MAX_NAME ]; /* e.g. "vec3_t", "int32_t"             */
    uint16_t  mods;                     /* RS_MODS() packed value               */
    uint16_t  array_count;              /* aux; 0 if not an array               */
    uint8_t   base_const;               /* `const T x`                          */

    int       attr_count;
    rg_attr_t attrs[ RG_MAX_ATTRS_PER_ITEM ];

} rg_decl_field_t;

/*--------------------------------------------------------------------------------------------*/

typedef struct rg_enum_s
{
    char name[ RG_MAX_NAME ];
    char value_expr[ RG_MAX_NAME ]; /* verbatim C expression; empty = auto  */
    int  has_value;

} rg_enum_t;

/*--------------------------------------------------------------------------------------------*/

typedef enum rg_kind_e
{
    RG_KIND_STRUCT = 0,
    RG_KIND_ENUM   = 1,
    RG_KIND_BITSET = 2,

} rg_kind_t;

/*--------------------------------------------------------------------------------------------*/
/* Per module type declarations filled by the parser and consumed by the output generator. */

typedef struct rg_decl_type_s
{
    char            name[ RG_MAX_NAME ];    // the name of the struct/enum/bitset (not tag name)
    int             kind;                   // rg_kind_t (STRUCT, ENUM, BITSET)

    rg_decl_field_t fields[ RG_MAX_FIELDS_PER_TYPE ];
    int             field_count;

    rg_enum_t       enums[ RG_MAX_ENUMS_PER_TYPE ];
    int             enum_count;

    rg_attr_t       attrs[ RG_MAX_ATTRS_PER_ITEM ];
    int             attr_count;

} rg_decl_type_t;

/*==============================================================================================
    Module API (RS_MODULE / RS_API)
==============================================================================================*/

/* One RS_API()-annotated function. */

typedef struct rg_api_func_s
{
    char ret_type  [ RG_MAX_NAME * 2 ];  /* return type string e.g. "const char*" */
    char name      [ RG_MAX_NAME ];      /* full function name  e.g. "audio_play"  */
    char field_name[ RG_MAX_NAME ];      /* struct field name   e.g. "play"         */
    char params    [ RG_MAX_PARAM_STR ]; /* param list string   e.g. "int x, int y" */
    int  src_is_c;                       /* 1 if found in a .c file, 0 if in a .h   */

} rg_api_func_t;

/* Module-level API descriptor collected from RS_MODULE() / RS_API() markers. */

typedef struct rg_module_api_s
{
    int           has_module;            /* non-zero when RS_MODULE() was seen */
    char          name[ RG_MAX_NAME ];   /* module name from RS_MODULE( name ) */
    rg_api_func_t funcs[ RG_MAX_API_FUNCS ];
    int           func_count;

} rg_module_api_t;

/*--------------------------------------------------------------------------------------------*/
/* Top-level parse output. The parser fills this with all the types it finds, and the output
   generator loops over it to emit code. */

typedef struct rg_parse_data_s
{
    rg_decl_type_t types[ RG_MAX_TYPES ];

    int            type_count;      // total declared types (any kind)
    int            struct_count;    // count used for output loops
    int            enum_count;      // enum + bitset

    /* Include paths (relative to source/) for headers that actually contained
       RS_ markers. The generated .c #includes these so type names resolve. */

    char headers[ RG_MAX_FILES ][ RG_MAX_PATH ];
    int  header_count;

    /* Module API collected from RS_MODULE() / RS_API() markers. */

    rg_module_api_t module_api;

} rg_parse_data_t;

/*==============================================================================================

    Public function declarations

    Internal static helpers in lex/attr/parse are visible within the unity build by
    inclusion order: std -> platform -> scan -> lex -> attr -> parse -> output

==============================================================================================*/
// clang-format off

/* rs_gen_std.c */

void    rg_str_copy             ( char* dst, const char* src, int max );
int     rg_str_len              ( const char* s );
void    rg_str_cat              ( char* dst, const char* src, int max );
int     rg_str_ends_with        ( const char* s, const char* suffix );

/* rs_gen_platform.c */

void    rg_platform_mkdir       ( const char* path );
int     rg_platform_scan_dir    ( const char* dir, char out_paths[][ RG_MAX_PATH ], int max_files );
void    rg_platform_exe_dir     ( char* out, int max );

/* rs_gen_scan.c */

void    rg_scan                 ( const char* source_dir, rg_file_list_t* out );

/* rs_gen_parse.c */

void    rg_parse                 ( const rg_file_list_t* files, rg_parse_data_t* out );

/* rs_gen_output.c */

int     rg_output                ( const char* output_dir, const char* module_name, const rg_parse_data_t* data );

// clang-format on
/*============================================================================================*/
#endif    // RS_GEN_INTERNAL_H
