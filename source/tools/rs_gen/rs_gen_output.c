/*==============================================================================================

    rs_gen_output.c - write <module>.generated.h and <module>.generated.c

    Output shape: ONE `void rs_register( uint16_t frame, rs_intern_fn intern )` per module,
    emitted as imperative registration calls using the rs_ low-level API directly.
    The DLL exports the well-known symbol:

        void rs_register( uint16_t frame, rs_intern_fn intern );

    which rs_load_module_dll() discovers via sys_library_get_symbol and calls directly.
    No descriptor types, no string tables, no secondary structs.

==============================================================================================*/

#include "rs_gen_internal.h"

/*----------------------------------------------------------------------------------------------
    Helpers
----------------------------------------------------------------------------------------------*/

static void
to_upper_guard( char* dst, const char* src, int max )
{
    int i = 0;
    for ( ; i < max - 1 && src[ i ]; i++ )
    {
        char c = src[ i ];
        if ( c >= 'a' && c <= 'z' )
            c = ( char )( c - 32 );
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
        case RG_ATTR_TAG: return "RS_ATTR_BOOL";
        case RG_ATTR_INT: return "RS_ATTR_INT";
        case RG_ATTR_FLOAT: return "RS_ATTR_FLOAT";
        case RG_ATTR_STRING: return "RS_ATTR_STRING";
    }
    return "RS_ATTR_NONE";
}

static const char*
kind_macro( int kind )
{
    switch ( kind )
    {
        case RG_KIND_STRUCT: return "RS_KIND_STRUCT";
        case RG_KIND_ENUM: return "RS_KIND_ENUM";
        case RG_KIND_BITSET: return "RS_KIND_BITSET";
    }
    return "RS_KIND_PRIM";
}

/*----------------------------------------------------------------------------------------------
    Emit one rs_attrib_t inline and call the given add function.
----------------------------------------------------------------------------------------------*/

static void
emit_attr_call( FILE* fc, const rg_attr_t* a, const char* add_fn, const char* id_expr, const char* indent )
{
    fprintf( fc, "%s{ rs_attrib_t _a = { .name_id = intern( \"%s\" ), .name_hash = rs_hash_str( \"%s\" ), .type = %s, .value = { ",
             indent, a->name, a->name, attr_kind_macro( a->kind ) );
    switch ( a->kind )
    {
        case RG_ATTR_TAG:    fprintf( fc, ".u32 = 1" ); break;
        case RG_ATTR_INT:    fprintf( fc, ".i32 = (int32_t)(%s)", a->value[ 0 ] ? a->value : "0" ); break;
        case RG_ATTR_FLOAT:  fprintf( fc, ".f32 = (float)(%s)", a->value[ 0 ] ? a->value : "0" ); break;
        case RG_ATTR_STRING: fprintf( fc, ".str = intern( \"%s\" )", a->value ); break;
    }
    fprintf( fc, " } }; api->%s( %s, &_a ); }\n", add_fn, id_expr );
}

/*----------------------------------------------------------------------------------------------
    Emit one type block for a struct/union type.
----------------------------------------------------------------------------------------------*/

static void
emit_struct_block( FILE* fc, const rg_decl_type_t* t )
{
    int has_field_attrs = 0;
    for ( int fi = 0; fi < t->field_count; fi++ )
        if ( t->fields[ fi ].attr_count > 0 ) { has_field_attrs = 1; break; }

    int needs_tid = ( t->attr_count > 0 || has_field_attrs );

    fprintf( fc, "    { /* %s */\n", t->name );

    fprintf( fc, "        rs_type_t _t = {\n" );
    fprintf( fc, "            .name_id  = intern( \"%s\" ),\n", t->name );
    fprintf( fc, "            .hash     = rs_hash_str( \"%s\" ),\n", t->name );
    fprintf( fc, "            .size     = RS_SIZEOF( %s ),\n", t->name );
    fprintf( fc, "            .align    = RS_ALIGNOF( %s ),\n", t->name );
    fprintf( fc, "            .kind     = %s,\n", kind_macro( t->kind ) );
    fprintf( fc, "        };\n" );

    if ( t->field_count > 0 )
    {
        fprintf( fc, "        rs_field_t _fields[] = {\n" );
        for ( int fi = 0; fi < t->field_count; fi++ )
        {
            const rg_decl_field_t* f = &t->fields[ fi ];
            fprintf( fc, "            { .name_id = intern( \"%s\" ), .name_hash = rs_hash_str( \"%s\" ),\n", f->name, f->name );
            fprintf( fc, "              .type_hash = rs_hash_str( \"%s\" ), .type_id = RS_TYPE_INVALID,\n", f->base_type );
            fprintf( fc, "              .offset = RS_OFFSETOF( %s, %s ), .size = RS_FIELD_SIZE( %s, %s )",
                     t->name, f->name, t->name, f->name );
            if ( f->mods )
                fprintf( fc, ",\n              .mods = (uint16_t)0x%04X", ( unsigned )f->mods );
            if ( f->array_count )
                fprintf( fc, ",\n              .aux = %u", ( unsigned )f->array_count );
            if ( f->base_const )
                fprintf( fc, ",\n              .base_const = 1" );
            fprintf( fc, " },\n" );
        }
        fprintf( fc, "        };\n" );

        if ( needs_tid )
            fprintf( fc, "        uint16_t tid = api->rs_register_type( &_t, _fields, %d );\n", t->field_count );
        else
            fprintf( fc, "        api->rs_register_type( &_t, _fields, %d );\n", t->field_count );
    }
    else
    {
        if ( needs_tid )
            fprintf( fc, "        uint16_t tid = api->rs_register_type( &_t, NULL, 0 );\n" );
        else
            fprintf( fc, "        api->rs_register_type( &_t, NULL, 0 );\n" );
    }

    for ( int ai = 0; ai < t->attr_count; ai++ )
        emit_attr_call( fc, &t->attrs[ ai ], "rs_type_add_attr", "tid", "        " );

    if ( has_field_attrs )
    {
        fprintf( fc, "        { const rs_type_t* _rt = api->rs_get_type( tid );\n" );
        for ( int fi = 0; fi < t->field_count; fi++ )
        {
            const rg_decl_field_t* f = &t->fields[ fi ];
            if ( f->attr_count == 0 )
                continue;
            char fid_expr[ 64 ];
            snprintf( fid_expr, sizeof fid_expr, "_rt->field_index + %d", fi );
            for ( int ai = 0; ai < f->attr_count; ai++ )
                emit_attr_call( fc, &f->attrs[ ai ], "rs_field_add_attr", fid_expr, "          " );
        }
        fprintf( fc, "        }\n" );
    }

    fprintf( fc, "    }\n\n" );
}

/*----------------------------------------------------------------------------------------------
    Emit one type block for an enum or bitset type.
----------------------------------------------------------------------------------------------*/

static void
emit_enum_block( FILE* fc, const rg_decl_type_t* t )
{
    int needs_tid = ( t->attr_count > 0 );
    const char* reg_fn = ( t->kind == RG_KIND_BITSET ) ? "rs_register_bitset" : "rs_register_enum";

    fprintf( fc, "    { /* %s */\n", t->name );

    fprintf( fc, "        rs_type_t _t = {\n" );
    fprintf( fc, "            .name_id  = intern( \"%s\" ),\n", t->name );
    fprintf( fc, "            .hash     = rs_hash_str( \"%s\" ),\n", t->name );
    fprintf( fc, "            .size     = RS_SIZEOF( %s ),\n", t->name );
    fprintf( fc, "            .align    = RS_ALIGNOF( %s ),\n", t->name );
    fprintf( fc, "            .kind     = %s,\n", kind_macro( t->kind ) );
    fprintf( fc, "        };\n" );

    if ( t->enum_count > 0 )
    {
        fprintf( fc, "        rs_enum_t _enums[] = {\n" );
        for ( int i = 0; i < t->enum_count; i++ )
        {
            const rg_enumerator_t* e = &t->enumerators[ i ];
            fprintf( fc, "            { .name_id = intern( \"%s\" ), .name_hash = rs_hash_str( \"%s\" ), .value = (int64_t)( %s ) },\n",
                     e->name, e->name, e->has_value ? e->value_expr : e->name );
        }
        fprintf( fc, "        };\n" );

        if ( needs_tid )
            fprintf( fc, "        uint16_t tid = api->%s( &_t, _enums, %d );\n", reg_fn, t->enum_count );
        else
            fprintf( fc, "        api->%s( &_t, _enums, %d );\n", reg_fn, t->enum_count );
    }
    else
    {
        if ( needs_tid )
            fprintf( fc, "        uint16_t tid = api->%s( &_t, NULL, 0 );\n", reg_fn );
        else
            fprintf( fc, "        api->%s( &_t, NULL, 0 );\n", reg_fn );
    }

    for ( int ai = 0; ai < t->attr_count; ai++ )
        emit_attr_call( fc, &t->attrs[ ai ], "rs_type_add_attr", "tid", "        " );

    fprintf( fc, "    }\n\n" );
}

/*----------------------------------------------------------------------------------------------
    Emit the rs_register function.
----------------------------------------------------------------------------------------------*/

static void
emit_rs_register( FILE* fc, const char* module_name, const rg_parse_data_t* data )
{
    fprintf( fc, "void\n%s_rs_register( rs_intern_fn intern, const rs_reg_api_t* api )\n{\n", module_name );

    for ( int i = 0; i < data->type_count; i++ )
    {
        const rg_decl_type_t* t = &data->types[ i ];
        if ( t->kind == RG_KIND_STRUCT )
            emit_struct_block( fc, t );
        else
            emit_enum_block( fc, t );
    }

    fprintf( fc, "}\n\n" );
    fprintf( fc, "RS_DEFINE_EXPORTS( %s )\n", module_name );
}

/*----------------------------------------------------------------------------------------------
    Public
----------------------------------------------------------------------------------------------*/

int
rg_output( const char* output_dir, const char* module_name, const rg_parse_data_t* data )
{
    char h_path[ RG_MAX_PATH ];
    char c_path[ RG_MAX_PATH ];
    char guard[ RG_MAX_NAME ];

    rg_str_copy( h_path, output_dir, RG_MAX_PATH );
    rg_str_cat( h_path, "/", RG_MAX_PATH );
    rg_str_cat( h_path, module_name, RG_MAX_PATH );
    rg_str_cat( h_path, ".generated.h", RG_MAX_PATH );

    rg_str_copy( c_path, output_dir, RG_MAX_PATH );
    rg_str_cat( c_path, "/", RG_MAX_PATH );
    rg_str_cat( c_path, module_name, RG_MAX_PATH );
    rg_str_cat( c_path, ".generated.c", RG_MAX_PATH );

    to_upper_guard( guard, module_name, RG_MAX_NAME );
    rg_str_cat( guard, "_GENERATED_H", RG_MAX_NAME );

    /* header */
    FILE* fh = fopen( h_path, "w" );
    if ( !fh )
    {
        fprintf( stderr, "[build_reflect] error: cannot write %s\n", h_path );
        return 0;
    }
    fprintf( fh, "#ifndef %s\n", guard );
    fprintf( fh, "#define %s\n", guard );
    fprintf( fh, "/* Generated by build_reflect -- do not edit. */\n\n" );
    fprintf( fh, "#include \"engine/rs/rs.h\"\n\n" );
    fprintf( fh, "void %s_rs_register( rs_intern_fn intern, const rs_reg_api_t* api );\n\n", module_name );
    fprintf( fh, "#endif /* %s */\n", guard );
    fclose( fh );

    /* implementation */
    FILE* fc = fopen( c_path, "w" );
    if ( !fc )
    {
        fprintf( stderr, "[build_reflect] error: cannot write %s\n", c_path );
        return 0;
    }
    fprintf( fc, "/* Generated by build_reflect -- do not edit. */\n\n" );
    fprintf( fc, "#include \"orb.h\"\n" );
    fprintf( fc, "#include \"engine/rs/rs.h\"\n" );
    for ( int i = 0; i < data->header_count; i++ ) fprintf( fc, "#include \"%s\"\n", data->headers[ i ] );
    fprintf( fc, "#include \"%s.generated.h\"\n\n", module_name );

    if ( data->type_count == 0 )
    {
        fprintf( fc, "void\n%s_rs_register( rs_intern_fn intern, const rs_reg_api_t* api )\n{\n", module_name );
        fprintf( fc, "    UNUSED( intern );\n" );
        fprintf( fc, "    UNUSED( api );\n" );
        fprintf( fc, "}\n\n" );
        fprintf( fc, "RS_DEFINE_EXPORTS( %s )\n", module_name );
    }
    else
    {
        emit_rs_register( fc, module_name, data );
    }

    fclose( fc );
    return 1;
}
