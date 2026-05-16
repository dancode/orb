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
#define RG_MAX_ENUMS_PER_TYPE  128 /* enumerators per enum/bitset          */
#define RG_MAX_ATTRS_PER_ITEM  8   /* attributes per field/type            */

/*==============================================================================================
    File list
==============================================================================================*/

typedef struct rg_file_list_s
{
    char paths[ RG_MAX_FILES ][ RG_MAX_PATH ];
    int  count;

} rg_file_list_t;

/*==============================================================================================
    Parsed AST
==============================================================================================*/

typedef enum rg_attr_kind_e
{
    RG_ATTR_TAG    = 0, /* bare identifier:  transient                  */
    RG_ATTR_INT    = 1, /* integer literal:  range=0                    */
    RG_ATTR_FLOAT  = 2, /* float literal:    weight=0.5                 */
    RG_ATTR_STRING = 3, /* quoted string:    tooltip="text"             */

} rg_attr_kind_t;

typedef struct rg_attr_s
{
    char name[ RG_MAX_NAME ];
    int  kind;                 /* rg_attr_kind_t                       */
    char value[ RG_MAX_NAME ]; /* verbatim literal (without quotes)    */

} rg_attr_t;

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

typedef struct rg_enumerator_s
{
    char name[ RG_MAX_NAME ];
    char value_expr[ RG_MAX_NAME ]; /* verbatim C expression; empty = auto  */
    int  has_value;

} rg_enumerator_t;

typedef enum rg_kind_e
{
    RG_KIND_STRUCT = 0,
    RG_KIND_ENUM   = 1,
    RG_KIND_BITSET = 2,

} rg_kind_t;

typedef struct rg_decl_type_s
{
    char            name[ RG_MAX_NAME ];
    int             kind; /* rg_kind_t                            */

    rg_decl_field_t fields[ RG_MAX_FIELDS_PER_TYPE ];
    int             field_count;

    rg_enumerator_t enumerators[ RG_MAX_ENUMS_PER_TYPE ];
    int             enum_count;

    rg_attr_t       attrs[ RG_MAX_ATTRS_PER_ITEM ];
    int             attr_count;

} rg_decl_type_t;

typedef struct rg_parse_data_s
{
    rg_decl_type_t types[ RG_MAX_TYPES ];
    int            type_count; /* total declared types (any kind)      */
    int            struct_count;
    int            enum_count; /* enum + bitset                        */

    /* Include paths (relative to source/) for headers that actually contained
       RS_ markers. The generated .c #includes these so type names resolve. */
    char           headers[ RG_MAX_FILES ][ RG_MAX_PATH ];
    int            header_count;

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
void rg_platform_exe_dir( char* out, int max );

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
#endif    // RS_GEN_INTERNAL_H
