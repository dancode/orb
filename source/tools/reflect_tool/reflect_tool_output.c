/*==============================================================================================

    reflect_tool_output.c - write <module>.generated.h and <module>.generated.c

    Output shape: ONE `void {module}_ref_register( const ref_reg_api_t* api )` per module,
    emitted as imperative registration calls using the ref_ low-level API directly.

    Modules wire the generated function into their mod_desc_t via MOD_REFLECT_FUNC. The
    host's ref_register_module() reads desc->ref_register and calls it directly â€” same
    path for static and dynamic builds. No DLL symbol lookup, no descriptor types, no
    string tables.

==============================================================================================*/

#include "reflect_tool_internal.h"

static const char* s_banner_open  = "/*==============================================================================================";
static const char* s_banner_close = "==============================================================================================*/";
static const char* s_strip        = "/*============================================================================================*/";

/*----------------------------------------------------------------------------------------------
    Helpers
----------------------------------------------------------------------------------------------*/

static const char*
mods_name( uint16_t mods )
{
    switch ( mods )
    {
        case 0x0000: return "REF_MODS_VALUE";
        case 0x0001: return "REF_MODS_PTR";
        case 0x0101: return "REF_MODS_PTR_PTR";
        case 0x0009: return "REF_MODS_CONST_PTR";
        case 0x0002: return "REF_MODS_ARRAY";
        case 0x0201: return "REF_MODS_PTR_ARRAY";
        case 0x0102: return "REF_MODS_ARRAY_PTR";
        case 0x0004: return "REF_MODS_FUNCTION";
        case 0x0010: return "REF_MODS_CONST_VALUE";
        case 0x0011: return "REF_MODS_PTR_TO_CONST";
        default:     return NULL;
    }
}

static void
to_upper_guard( char* dst, const char* src, int max )
{
    int i = 0;
    for ( ; i < max - 1 && src[ i ]; i++ )
    {
        char c = src[ i ];
        if ( c >= 'a' && c <= 'z' )
            c = (char)( c - 32 );
        else if ( c == '.' || c == '-' || c == '/' || c == '\\' )
            c = '_';
        dst[ i ] = c;
    }
    dst[ i ] = '\0';
}

static const char*
attr_kind_macro( int kind )
{
    switch ( kind )
    {
        case RT_ATTR_TAG:    return "REF_ATTR_BOOL";
        case RT_ATTR_INT:    return "REF_ATTR_INT";
        case RT_ATTR_FLOAT:  return "REF_ATTR_FLOAT";
        case RT_ATTR_STRING: return "REF_ATTR_STRING";
    }
    return "REF_ATTR_NONE";
}

static const char*
kind_macro( int kind )
{
    switch ( kind )
    {
        case RT_KIND_STRUCT:   return "REF_KIND_STRUCT";
        case RT_KIND_ENUM:     return "REF_KIND_ENUM";
        case RT_KIND_BITSET:   return "REF_KIND_BITSET";
        case RT_KIND_UNION:    return "REF_KIND_UNION";
        case RT_KIND_FUNCTION: return "REF_KIND_FUNCTION";
    }
    return "REF_KIND_PRIM";
}

/* Find a RT_KIND_FUNCTION type in the module's type table by name. Returns NULL if not found. */
static const decl_type_t*
find_func_type( const parse_data_t* data, const char* name )
{
    for ( int i = 0; i < data->type_count; i++ )
    {
        if ( data->types[ i ].kind == RT_KIND_FUNCTION && strcmp( data->types[ i ].name, name ) == 0 )
            return &data->types[ i ];
    }
    return NULL;
}

/* Write the local-variable name used to capture a function signature's type_id. */
static void
func_tid_var( char* out, int max, const char* type_name )
{
    snprintf( out, max, "_tid_%s", type_name );
}

/* Emit one ref_attrib_t inline and call the given add function. */
static void
emit_attr_call( FILE* fc, const attr_t* a, const char* add_fn, const char* id_expr,
                const char* indent )
{
    fprintf( fc, "%s{ ref_attrib_t _a = {\n", indent );
    fprintf( fc, "%s      .name_id = api->intern( \"%s\" ),\n", indent, a->name );
    fprintf( fc, "%s      .type    = %s,\n",                    indent, attr_kind_macro( a->kind ) );
    fprintf( fc, "%s      .ci      = REF_ATTR_CI_SINGLE,\n",     indent );
    fprintf( fc, "%s      .value   = { ",                       indent );
    switch ( a->kind )
    {
        case RT_ATTR_TAG:    fprintf( fc, ".u32 = 1" ); break;
        case RT_ATTR_INT:    fprintf( fc, ".i32 = (int32_t)(%s)", a->value[ 0 ] ? a->value : "0" ); break;
        case RT_ATTR_FLOAT:  fprintf( fc, ".f32 = (float)(%s)",   a->value[ 0 ] ? a->value : "0" ); break;
        case RT_ATTR_STRING: fprintf( fc, ".str = api->intern( \"%s\" )", a->value ); break;
    }
    fprintf( fc, " },\n" );
    fprintf( fc, "%s  };\n",                         indent );
    fprintf( fc, "%s  api->%s( %s, &_a ); }\n", indent, add_fn, id_expr );
}

/* Emit the ref_type_t _t = { ... }; block shared by structs and enums. */
static void
emit_type_decl( FILE* fc, const decl_type_t* t )
{
    fprintf( fc, "        ref_type_t _t = {\n" );
    fprintf( fc, "            .name_id  = api->intern( \"%s\" ),\n", t->name );
    fprintf( fc, "            .name_hash = ref_hash_str( \"%s\" ),\n", t->name );
    fprintf( fc, "            .size     = REF_SIZEOF( %s ),\n", t->name );
    fprintf( fc, "            .align    = REF_ALIGNOF( %s ),\n", t->name );
    fprintf( fc, "            .kind     = %s,\n", kind_macro( t->kind ) );
    fprintf( fc, "        };\n" );
}

/* Emit the api->ref_register_*() call, optionally capturing the returned type ID. */
static void
emit_register( FILE* fc, const char* fn, const char* items, int count, int capture_tid )
{
    if ( capture_tid )
        fprintf( fc, "        uint16_t tid = api->%s( &_t, %s, %d );\n", fn, items, count );
    else
        fprintf( fc, "        api->%s( &_t, %s, %d );\n", fn, items, count );
}

/*----------------------------------------------------------------------------------------------
    Type block emitters
----------------------------------------------------------------------------------------------*/

static void
emit_struct_block( FILE* fc, const decl_type_t* t, const parse_data_t* data )
{
    int has_field_attrs = 0;
    for ( int fi = 0; fi < t->field_count; fi++ )
        if ( t->fields[ fi ].attr_count > 0 ) { has_field_attrs = 1; break; }

    int needs_tid = ( t->attr_count > 0 || has_field_attrs );

    fprintf( fc, "    { /* %s */\n", t->name );
    emit_type_decl( fc, t );

    if ( t->field_count > 0 )
    {
        fprintf( fc, "        ref_field_t _fields[] = {\n" );
        for ( int fi = 0; fi < t->field_count; fi++ )
        {
            const decl_field_t*  f    = &t->fields[ fi ];
            const decl_type_t*   fsig = find_func_type( data, f->base_type );

            fprintf( fc, "            { .name_id = api->intern( \"%s\" ),\n", f->name );

            if ( fsig )
            {
                /* Function pointer field: base type resolves to the signature's return type;
                   mods = REF_MODS_FUNCTION; aux = the signature's captured type_id variable. */
                const char* ret_base = ( fsig->field_count > 0 ) ? fsig->fields[ 0 ].base_type : "void";
                char        var[ RT_MAX_NAME + 8 ];
                func_tid_var( var, sizeof var, fsig->name );
                fprintf( fc, "              .type_hash = ref_hash_str( \"%s\" ), "
                             ".type_id = REF_TYPE_INVALID,\n", ret_base );
                fprintf( fc, "              .offset = REF_OFFSETOF( %s, %s ), "
                             ".size = REF_FIELD_SIZE( %s, %s ),\n",
                         t->name, f->name, t->name, f->name );
                fprintf( fc, "              .mods = REF_MODS_FUNCTION, .aux = %s },\n", var );
            }
            else
            {
                fprintf( fc, "              .type_hash = ref_hash_str( \"%s\" ), "
                             ".type_id = REF_TYPE_INVALID,\n", f->base_type );
                fprintf( fc, "              .offset = REF_OFFSETOF( %s, %s ), "
                             ".size = REF_FIELD_SIZE( %s, %s )",
                         t->name, f->name, t->name, f->name );
                if ( f->mods )
                {
                    const char* mname = mods_name( f->mods );
                    if ( mname )
                        fprintf( fc, ",\n              .mods = %s", mname );
                    else
                        fprintf( fc, ",\n              .mods = (uint16_t)0x%04X", (unsigned)f->mods );
                }
                if ( f->array_count )
                    fprintf( fc, ",\n              .aux = %u", (unsigned)f->array_count );
                fprintf( fc, " },\n" );
            }
        }
        fprintf( fc, "        };\n" );
        emit_register( fc, "ref_register_type", "_fields", t->field_count, needs_tid );
    }
    else
    {
        emit_register( fc, "ref_register_type", "NULL", 0, needs_tid );
    }

    for ( int ai = 0; ai < t->attr_count; ai++ )
        emit_attr_call( fc, &t->attrs[ ai ], "ref_type_add_attr", "tid", "        " );

    if ( has_field_attrs )
    {
        fprintf( fc, "        { const ref_type_t* _rt = api->ref_get_type( tid );\n" );
        for ( int fi = 0; fi < t->field_count; fi++ )
        {
            const decl_field_t* f = &t->fields[ fi ];
            if ( f->attr_count == 0 )
                continue;
            char fid_expr[ 64 ];
            snprintf( fid_expr, sizeof fid_expr, "_rt->field_index + %d", fi );
            for ( int ai = 0; ai < f->attr_count; ai++ )
                emit_attr_call( fc, &f->attrs[ ai ], "ref_field_add_attr", fid_expr, "          " );
        }
        fprintf( fc, "        }\n" );
    }

    fprintf( fc, "    }\n\n" );
}

static void
emit_enum_block( FILE* fc, const decl_type_t* t )
{
    int         needs_tid = ( t->attr_count > 0 );
    const char* reg_fn    = ( t->kind == RT_KIND_BITSET ) ? "ref_register_bitset" : "ref_register_enum";

    fprintf( fc, "    { /* %s */\n", t->name );
    emit_type_decl( fc, t );

    if ( t->enum_count > 0 )
    {
        fprintf( fc, "        ref_enum_t _enums[] = {\n" );
        for ( int i = 0; i < t->enum_count; i++ )
        {
            const enum_t* e = &t->enums[ i ];
            fprintf( fc, "            { .name_id = api->intern( \"%s\" ), "
                         ".value = (int32_t)( %s ) },\n",
                     e->name, e->has_value ? e->value_expr : e->name );
        }
        fprintf( fc, "        };\n" );
        emit_register( fc, reg_fn, "_enums", t->enum_count, needs_tid );
    }
    else
    {
        emit_register( fc, reg_fn, "NULL", 0, needs_tid );
    }

    for ( int ai = 0; ai < t->attr_count; ai++ )
        emit_attr_call( fc, &t->attrs[ ai ], "ref_type_add_attr", "tid", "        " );

    fprintf( fc, "    }\n\n" );
}

/*----------------------------------------------------------------------------------------------
    Module API emitters  (REF_MODULE / REF_API)
----------------------------------------------------------------------------------------------*/

/* Emit the API typedef struct into the .h. */

static void
emit_module_api_typedef_h( FILE* fh, const char* module_name, const module_api_t* api )
{
    fprintf( fh, "#include \"engine/mod/mod_import.h\"\n\n" );
    fprintf( fh, "typedef struct %s_api_s\n{\n", module_name );
    for ( int i = 0; i < api->func_count; i++ )
    {
        const api_func_t* f     = &api->funcs[ i ];
        const char*       params = f->params[ 0 ] ? f->params : "void";
        fprintf( fh, "    %s ( *%s )( %s );\n", f->ret_type, f->field_name, params );
    }
    fprintf( fh, "\n} %s_api_t;\n", module_name );
}

/* Emit the MOD_GATEWAY block, MOD_USE/FETCH macros, and GEN_API_STRUCT define into the .h. */

static void
emit_module_api_gateway_h( FILE* fh, const char* module_name, const module_api_t* api )
{
    char upper[ RT_MAX_NAME ];
    to_upper_guard( upper, module_name, RT_MAX_NAME );
    (void)api;

    fprintf( fh, "#if defined( BUILD_STATIC ) || defined( %s_STATIC )\n", upper );
    fprintf( fh, "    MOD_GATEWAY_STATIC( %s_api_t, %s )\n", module_name, module_name );
    fprintf( fh, "#else\n" );
    fprintf( fh, "    MOD_GATEWAY_DYNAMIC( %s_api_t, %s )\n", module_name, module_name );
    fprintf( fh, "#endif\n\n" );

    fprintf( fh, "#if defined( BUILD_STATIC ) || defined( %s_STATIC )\n", upper );
    fprintf( fh, "    #define MOD_USE_%s    /* static build */\n", upper );
    fprintf( fh, "    #define MOD_FETCH_%s  true\n", upper );
    fprintf( fh, "#else\n" );
    fprintf( fh, "    #define MOD_USE_%s    MOD_DEFINE_API_PTR( %s_api_t, %s )\n", upper, module_name, module_name );
    fprintf( fh, "    #define MOD_FETCH_%s  MOD_FETCH_API( %s_api_t, %s )\n", upper, module_name, module_name );
    fprintf( fh, "#endif\n\n" );

    fprintf( fh, "#define %s_GEN_API_STRUCT ( &g_%s_api_struct )\n", upper, module_name );
}

/* Emit the const API struct definition into the .c, with forward declarations for any
   functions that were found in .c files rather than .h files (no header will declare them). */

static void
emit_module_api_c( FILE* fc, const char* module_name, const module_api_t* api )
{
    /* forward-declare .c-sourced functions so the struct initialiser can reference them */
    int has_c_decls = 0;
    for ( int i = 0; i < api->func_count; i++ )
        if ( api->funcs[ i ].src_is_c ) { has_c_decls = 1; break; }

    if ( has_c_decls )
    {
        for ( int i = 0; i < api->func_count; i++ )
        {
            const api_func_t* f = &api->funcs[ i ];
            if ( !f->src_is_c )
                continue;
            const char* params = f->params[ 0 ] ? f->params : "void";
            fprintf( fc, "%s %s( %s );\n", f->ret_type, f->name, params );
        }
        fprintf( fc, "\n" );
    }

    fprintf( fc, "const %s_api_t g_%s_api_struct =\n{\n", module_name, module_name );
    for ( int i = 0; i < api->func_count; i++ )
    {
        const api_func_t* f = &api->funcs[ i ];
        fprintf( fc, "    .%-24s = %s,\n", f->field_name, f->name );
    }
    fprintf( fc, "};\n\n" );
}

/*----------------------------------------------------------------------------------------------
    Function signature block emitter
----------------------------------------------------------------------------------------------*/

static void
emit_func_block( FILE* fc, const decl_type_t* t )
{
    char var[ RT_MAX_NAME + 8 ];
    func_tid_var( var, sizeof var, t->name );

    fprintf( fc, "    { /* %s */\n", t->name );

    /* kind is forced to REF_KIND_FUNCTION by ref_register_function; no need to set it here. */
    fprintf( fc, "        ref_type_t _t = {\n" );
    fprintf( fc, "            .name_id   = api->intern( \"%s\" ),\n", t->name );
    fprintf( fc, "            .name_hash = ref_hash_str( \"%s\" ),\n", t->name );
    fprintf( fc, "            .size      = REF_SIZEOF( %s ),\n", t->name );
    fprintf( fc, "            .align     = REF_ALIGNOF( %s ),\n", t->name );
    fprintf( fc, "        };\n" );

    if ( t->field_count > 0 )
    {
        fprintf( fc, "        ref_field_t _fields[] = {\n" );
        for ( int fi = 0; fi < t->field_count; fi++ )
        {
            const decl_field_t* f = &t->fields[ fi ];
            fprintf( fc, "            { .name_id = api->intern( \"%s\" ),\n", f->name );
            fprintf( fc, "              .type_hash = ref_hash_str( \"%s\" )", f->base_type );
            if ( f->mods )
            {
                const char* mname = mods_name( f->mods );
                if ( mname )
                    fprintf( fc, ",\n              .mods = %s", mname );
                else
                    fprintf( fc, ",\n              .mods = (uint16_t)0x%04X", (unsigned)f->mods );
            }
            fprintf( fc, " },\n" );
        }
        fprintf( fc, "        };\n" );
        fprintf( fc, "        %s = api->ref_register_function( &_t, _fields, %d );\n",
                 var, t->field_count );
    }
    else
    {
        fprintf( fc, "        %s = api->ref_register_function( &_t, NULL, 0 );\n", var );
    }

    fprintf( fc, "    }\n\n" );
}

/*----------------------------------------------------------------------------------------------
    Register function emitter

    Two-pass strategy: function signature types are emitted before struct/union/enum types so
    that their captured type_ids are available when struct fields referencing them are built.
----------------------------------------------------------------------------------------------*/

static void
emit_ref_register( FILE* fc, const char* module_name, const parse_data_t* data )
{
    fprintf( fc, "void\n%s_ref_register( const ref_reg_api_t* api )\n{\n", module_name );

    /* Declare type_id capture variables for all function signature types up front.
       C89/C11 requires declarations before statements inside a block. */
    for ( int i = 0; i < data->type_count; i++ )
    {
        if ( data->types[ i ].kind == RT_KIND_FUNCTION )
        {
            char var[ RT_MAX_NAME + 8 ];
            func_tid_var( var, sizeof var, data->types[ i ].name );
            fprintf( fc, "    uint16_t %s = REF_TYPE_INVALID;\n", var );
        }
    }
    if ( data->func_count > 0 )
        fprintf( fc, "\n" );

    /* Pass 1: register function signature types and capture their type_ids. */
    for ( int i = 0; i < data->type_count; i++ )
    {
        const decl_type_t* t = &data->types[ i ];
        if ( t->kind == RT_KIND_FUNCTION )
            emit_func_block( fc, t );
    }

    /* Pass 2: register struct/union/enum types (function ptr fields reference sig type_ids). */
    for ( int i = 0; i < data->type_count; i++ )
    {
        const decl_type_t* t = &data->types[ i ];
        if ( t->kind == RT_KIND_STRUCT || t->kind == RT_KIND_UNION )
            emit_struct_block( fc, t, data );
        else if ( t->kind != RT_KIND_FUNCTION )
            emit_enum_block( fc, t );
    }

    fprintf( fc, "}\n" );
}

/*----------------------------------------------------------------------------------------------
    Public
----------------------------------------------------------------------------------------------*/

int
output( const char* output_dir, const char* module_name, const parse_data_t* data )
{
    char h_path[ RT_MAX_PATH ];
    char c_path[ RT_MAX_PATH ];
    char guard[ RT_MAX_NAME ];

    snprintf( h_path, RT_MAX_PATH, "%s/%s.generated.h", output_dir, module_name );
    snprintf( c_path, RT_MAX_PATH, "%s/%s.generated.c", output_dir, module_name );

    to_upper_guard( guard, module_name, RT_MAX_NAME );
    str_cat( guard, "_GENERATED_H", RT_MAX_NAME );

    /* header */
    FILE* fh = fopen( h_path, "w" );
    if ( !fh )
    {
        fprintf( stderr, "[reflect_tool] error: cannot write %s\n", h_path );
        return 0;
    }
    fprintf( fh, "#ifndef %s\n", guard );
    fprintf( fh, "#define %s\n", guard );
    fprintf( fh, "%s\n\n", s_banner_open );
    if ( data->module_api.has_module && data->module_api.func_count > 0 )
        fprintf( fh, "    %s.generated.h -- reflection registration and module API declarations for %s.\n\n",
                 module_name, module_name );
    else
        fprintf( fh, "    %s.generated.h -- reflection registration declarations for %s.\n\n",
                 module_name, module_name );
    fprintf( fh, "    Auto-generated by reflect_tool. Do not edit.\n\n" );
    fprintf( fh, "%s\n\n", s_banner_close );

    fprintf( fh, "%s\n", s_banner_open );
    fprintf( fh, "    Reflection Registration\n" );
    fprintf( fh, "%s\n\n", s_banner_close );
    fprintf( fh, "typedef struct ref_reg_api_s ref_reg_api_t;\n\n" );
    fprintf( fh, "void %s_ref_register( const ref_reg_api_t* api );\n", module_name );

    if ( data->module_api.has_module && data->module_api.func_count > 0 )
    {
        fprintf( fh, "\n%s\n", s_banner_open );
        fprintf( fh, "    Module Code: Function API\n" );
        fprintf( fh, "%s\n\n", s_banner_close );
        emit_module_api_typedef_h( fh, module_name, &data->module_api );

        fprintf( fh, "\n%s\n", s_banner_open );
        fprintf( fh, "    Module Code: Gateway Accessor\n" );
        fprintf( fh, "%s\n\n", s_banner_close );
        emit_module_api_gateway_h( fh, module_name, &data->module_api );
    }

    fprintf( fh, "\n%s\n", s_strip );
    fprintf( fh, "#endif /* %s */\n", guard );
    fclose( fh );

    /* implementation */
    FILE* fc = fopen( c_path, "w" );
    if ( !fc )
    {
        fprintf( stderr, "[reflect_tool] error: cannot write %s\n", c_path );
        return 0;
    }
    fprintf( fc, "%s\n\n", s_banner_open );
    if ( data->module_api.has_module && data->module_api.func_count > 0 )
        fprintf( fc, "    %s.generated.c -- reflection registration data and module API for %s.\n\n",
                 module_name, module_name );
    else
        fprintf( fc, "    %s.generated.c -- reflection registration data for %s.\n\n",
                 module_name, module_name );
    fprintf( fc, "    Auto-generated by reflect_tool. Do not edit.\n\n" );
    fprintf( fc, "%s\n\n", s_banner_close );
    /* two-pass: measure longest include line, then emit with aligned comments */
    int col = (int)strlen( "#include \"orb.h\"" );
    {
        int n;
        char tmp[ RT_MAX_PATH + 16 ];
        n = (int)strlen( "#include \"engine/ref/ref_import.h\"" ); if ( n > col ) col = n;
        for ( int i = 0; i < data->header_count; i++ )
        {
            n = snprintf( tmp, sizeof tmp, "#include \"%s\"", data->headers[ i ] );
            if ( n > col ) col = n;
        }
        n = snprintf( tmp, sizeof tmp, "#include \"%s.generated.h\"", module_name );
        if ( n > col ) col = n;
    }
    col += 4;
    {
        char inc[ RT_MAX_PATH + 16 ];
        fprintf( fc, "%-*s  /* engine base */\n", col, "#include \"orb.h\"" );
        fprintf( fc, "%-*s  /* reflection registration api */\n", col, "#include \"engine/ref/ref_import.h\"" );
        for ( int i = 0; i < data->header_count; i++ )
        {
            snprintf( inc, sizeof inc, "#include \"%s\"", data->headers[ i ] );
            fprintf( fc, "%-*s  /* modules reflected types */\n", col, inc );
        }
        snprintf( inc, sizeof inc, "#include \"%s.generated.h\"", module_name );
        fprintf( fc, "%-*s  /* generated declarations */\n", col, inc );
    }
    fprintf( fc, "\n" );

    if ( data->module_api.has_module && data->module_api.func_count > 0 )
    {
        fprintf( fc, "%s\n", s_banner_open );
        fprintf( fc, "    Module Code\n" );
        fprintf( fc, "%s\n\n", s_banner_close );
        emit_module_api_c( fc, module_name, &data->module_api );
    }

    fprintf( fc, "%s\n", s_banner_open );
    fprintf( fc, "    Reflection Registration\n" );
    fprintf( fc, "%s\n\n", s_banner_close );

    if ( data->type_count == 0 )
    {
        fprintf( fc, "void\n%s_ref_register( const ref_reg_api_t* api )\n{\n", module_name );
        fprintf( fc, "    UNUSED( api );\n" );
        fprintf( fc, "}\n" );
    }
    else
    {
        emit_ref_register( fc, module_name, data );
    }

    fprintf( fc, "\n%s\n", s_strip );

    fclose( fc );
    return 1;
}
