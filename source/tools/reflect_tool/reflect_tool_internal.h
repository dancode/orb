#ifndef REFLECT_TOOL_INTERNAL_H
#define REFLECT_TOOL_INTERNAL_H
/*==============================================================================================

    reflect_tool_internal.h - internal types and declarations for the reflect_tool tool

    reflect_tool is a standalone C11 tool (no engine deps, no orb.h).
    It scans annotated .h files and generates REF_-compatible registration code.

==============================================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*==============================================================================================
    Limits
==============================================================================================*/

#define RT_MAX_FILES           256
#define RT_MAX_PATH            512
#define RT_MAX_NAME            128

#define RT_MAX_TYPES           128 /* types per module                     */
#define RT_MAX_FIELDS_PER_TYPE 64  /* fields per struct                    */
#define RT_MAX_ENUMS_PER_TYPE  128 /* enums per enum/bitset                */
#define RT_MAX_ATTRS_PER_ITEM  8   /* attributes per field/type            */
#define RT_MAX_API_FUNCS       64  /* REF_API() functions per module        */
#define RT_MAX_PARAM_STR       512 /* param list string per API function   */

/*==============================================================================================
    File list
==============================================================================================*/

/* Used to pass scan results from scan to parse, and to track which headers actually
   contained REF_ declarations for output includes. */

typedef struct file_list_s
{
    char paths[ RT_MAX_FILES ][ RT_MAX_PATH ];
    int  count;
    char source_dir[ RT_MAX_PATH ]; /* directory passed to scan, reused for .c pass */

} file_list_t;

/*==============================================================================================
    Parsed AST
==============================================================================================*/

typedef enum attr_kind_e
{
    RT_ATTR_TAG    = 0,    // bare identifier:  transient
    RT_ATTR_INT    = 1,    // integer literal:  range=0
    RT_ATTR_FLOAT  = 2,    // float literal:    weight=0.5
    RT_ATTR_STRING = 3,    // quoted string:    tooltip="text"

} attr_kind_t;

/*--------------------------------------------------------------------------------------------*/

typedef struct attr_s
{
    int  kind;                    // attr_kind_t
    char name[ RT_MAX_NAME ];     //
    char value[ RT_MAX_NAME ];    // verbatim literal (without quotes)

} attr_t;

/*--------------------------------------------------------------------------------------------*/

typedef struct decl_field_s
{
    char      name[ RT_MAX_NAME ];
    char      base_type[ RT_MAX_NAME ]; /* e.g. "vec3_t", "int32_t"             */
    uint16_t  mods;                     /* REF_mods_t enum value from ref.h       */
    uint16_t  array_count;              /* aux; 0 if not an array               */

    int       attr_count;
    attr_t attrs[ RT_MAX_ATTRS_PER_ITEM ];

} decl_field_t;

/*--------------------------------------------------------------------------------------------*/

typedef struct enum_s
{
    char name[ RT_MAX_NAME ];
    char value_expr[ RT_MAX_NAME ]; /* verbatim C expression; empty = auto  */
    int  has_value;

} enum_t;

/*--------------------------------------------------------------------------------------------*/

typedef enum kind_e
{
    RT_KIND_STRUCT   = 0,
    RT_KIND_ENUM     = 1,
    RT_KIND_BITSET   = 2,
    RT_KIND_UNION    = 3,
    RT_KIND_FUNCTION = 4,   /* typedef void (*name_fn)(...) -- field[0]=return, field[1..]=params */

} kind_t;

/*--------------------------------------------------------------------------------------------*/
/* Per module type declarations filled by the parser and consumed by the output generator. */

typedef struct decl_type_s
{
    char            name[ RT_MAX_NAME ];    // the name of the struct/enum/bitset (not tag name)
    int             kind;                   // kind_t (STRUCT, ENUM, BITSET)

    decl_field_t fields[ RT_MAX_FIELDS_PER_TYPE ];
    int             field_count;

    enum_t       enums[ RT_MAX_ENUMS_PER_TYPE ];
    int             enum_count;

    attr_t       attrs[ RT_MAX_ATTRS_PER_ITEM ];
    int             attr_count;

} decl_type_t;

/*==============================================================================================
    Module API (REF_MODULE / REF_API)
==============================================================================================*/

/* One REF_API()-annotated function. */

typedef struct api_func_s
{
    char ret_type  [ RT_MAX_NAME * 2 ];  /* return type string e.g. "const char*" */
    char name      [ RT_MAX_NAME ];      /* full function name  e.g. "audio_play"  */
    char field_name[ RT_MAX_NAME ];      /* struct field name   e.g. "play"         */
    char params    [ RT_MAX_PARAM_STR ]; /* param list string   e.g. "int x, int y" */
    int  src_is_c;                       /* 1 if found in a .c file, 0 if in a .h   */

} api_func_t;

/* Module-level API descriptor collected from REF_MODULE() / REF_API() markers. */

typedef struct module_api_s
{
    int           has_module;            /* non-zero when REF_MODULE() was seen */
    char          name[ RT_MAX_NAME ];   /* module name from REF_MODULE( name ) */
    api_func_t funcs[ RT_MAX_API_FUNCS ];
    int           func_count;

} module_api_t;

/*--------------------------------------------------------------------------------------------*/
/* Top-level parse output. The parser fills this with all the types it finds, and the output
   generator loops over it to emit code. */

typedef struct parse_data_s
{
    decl_type_t types[ RT_MAX_TYPES ];

    int            type_count;      // total declared types (any kind)
    int            struct_count;    // count used for output loops
    int            enum_count;      // enum + bitset
    int            func_count;      // REF_FUNC function signature types

    /* Include paths (relative to source/) for headers that actually contained
       REF_ markers. The generated .c #includes these so type names resolve. */

    char headers[ RT_MAX_FILES ][ RT_MAX_PATH ];
    int  header_count;

    /* Module API collected from REF_MODULE() / REF_API() markers. */

    module_api_t module_api;

} parse_data_t;

/*==============================================================================================

    Public function declarations

    Internal static helpers in lex/attr/parse are visible within the unity build by
    inclusion order: std -> platform -> scan -> lex -> attr -> parse -> output

==============================================================================================*/
// clang-format off

/* reflect_tool_std.c */

void    str_copy             ( char* dst, const char* src, int max );
int     str_len              ( const char* s );
void    str_cat              ( char* dst, const char* src, int max );
int     str_ends_with        ( const char* s, const char* suffix );

/* reflect_tool_platform.c */

void    platform_mkdir       ( const char* path );
int     platform_scan_dir    ( const char* dir, char out_paths[][ RT_MAX_PATH ], int max_files );
void    platform_exe_dir     ( char* out, int max );

/* reflect_tool_scan.c */

void    scan                 ( const char* source_dir, file_list_t* out );

/* reflect_tool_parse.c */

void    parse                 ( const file_list_t* files, parse_data_t* out );

/* reflect_tool_output.c */

int     output                ( const char* output_dir, const char* module_name, const parse_data_t* data );

// clang-format on
/*============================================================================================*/
#endif    // REFLECT_TOOL_INTERNAL_H
