/* engine/rs/rs_print.c - Diagnostics: type description strings and console dump.

   These are debug/editor utilities -- not used in shipping builds. rs_field_describe
   reconstructs a C-style declaration string (e.g. "const vec3_t*") from packed mods.
   rs_print_type and rs_print_frame dump human-readable type info to stdout. */

/*==============================================================================================
    String Helpers

    Safe bounded append -- writes characters into buf[pos..cap-2], always null-terminates.
    Returns the new position (not the length), so the caller can chain calls without
    tracking intermediate string lengths. Does not use printf/sprintf to avoid format-string
    overhead for simple string assembly.
==============================================================================================*/

static int
rs_str_append( char* buf, size_t cap, size_t pos, const char* s )
{
    while ( *s && pos + 1 < cap ) buf[ pos++ ] = *s++;
    if ( pos < cap ) buf[ pos ] = '\0';
    return (int)pos;
}

/*==============================================================================================
    Type Description

    Reconstructs a C-style type declaration string for a field by reading its mods value.
    Examples: "const vec3_t*", "vec3_t*[8]", "vec3_t(*)[8]", "float[4]".
    Used by the editor inspector and by rs_print_type for diagnostic output.
    Returns the number of characters written (not counting the null terminator).
==============================================================================================*/

size_t
rs_field_describe( const rs_field_t* f, char* buf, size_t buf_size )
{
    if ( !buf || buf_size == 0 ) return 0;
    buf[ 0 ] = '\0';
    if ( !f ) return 0;

    size_t pos = 0;

    /* Prepend "const " if the base type is const-qualified (covers both const T and const T*). */
    if ( f->mods == RS_MODS_CONST_VALUE || f->mods == RS_MODS_PTR_TO_CONST )
        pos = rs_str_append( buf, buf_size, pos, "const " );

    /* Emit the base type name; fall back to "<unresolved>" for fields that failed finalization. */
    const rs_type_t* base = rs_get_type( f->type_id );
    pos = rs_str_append( buf, buf_size, pos, base ? rs_cstr( base->name_id ) : "<unresolved>" );

    char tmp[ 24 ];
    switch ( (rs_mods_t)f->mods )
    {
        case RS_MODS_VALUE:
        case RS_MODS_CONST_VALUE:
            break;
        case RS_MODS_PTR:
        case RS_MODS_PTR_TO_CONST:
            pos = rs_str_append( buf, buf_size, pos, "*" ); break;
        case RS_MODS_PTR_PTR:      pos = rs_str_append( buf, buf_size, pos, "**" ); break;
        case RS_MODS_CONST_PTR:    pos = rs_str_append( buf, buf_size, pos, "* const" ); break;
        case RS_MODS_ARRAY:
            snprintf( tmp, sizeof( tmp ), "[%u]", (unsigned)f->aux );
            pos = rs_str_append( buf, buf_size, pos, tmp );
            break;
        case RS_MODS_PTR_ARRAY:
            snprintf( tmp, sizeof( tmp ), "*[%u]", (unsigned)f->aux );
            pos = rs_str_append( buf, buf_size, pos, tmp );
            break;
        case RS_MODS_ARRAY_PTR:
            snprintf( tmp, sizeof( tmp ), "(*)[%u]", (unsigned)f->aux );
            pos = rs_str_append( buf, buf_size, pos, tmp );
            break;
        case RS_MODS_FUNCTION:
        {
            const rs_type_t* sig = rs_get_type( f->aux );
            if ( sig && sig->kind == RS_KIND_FUNCTION )
            {
                pos = rs_str_append( buf, buf_size, pos, "(" );
                pos = rs_str_append( buf, buf_size, pos, rs_cstr( sig->name_id ) );
                pos = rs_str_append( buf, buf_size, pos, ")" );
            }
            else
                pos = rs_str_append( buf, buf_size, pos, "(...)" );
            break;
        }
    }

    return pos;
}

/*==============================================================================================
    Diagnostic Printing

    Console dumps of the registry for debugging and development. rs_print_type dispatches
    on kind so that enums, function signatures, and structs each display in the most readable
    format for their content. These functions are intentionally simple -- clarity over brevity.
==============================================================================================*/

void
rs_print_type( uint16_t type_id )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t ) { printf( "rs_print_type: invalid id %u\n", type_id ); return; }

    const char* kind_str =
        ( t->kind == RS_KIND_PRIM     ) ? "prim"     :
        ( t->kind == RS_KIND_STRUCT   ) ? "struct"   :
        ( t->kind == RS_KIND_ENUM     ) ? "enum"     :
        ( t->kind == RS_KIND_BITSET   ) ? "bitset"   :
        ( t->kind == RS_KIND_UNION    ) ? "union"    :
        ( t->kind == RS_KIND_FUNCTION ) ? "function" : "?";

    const char* member_word =
        rs_kind_is_enum( (rs_kind_t)t->kind ) ? "enumerators" :
        ( t->kind == RS_KIND_FUNCTION )        ? "params+1"    : "fields";

    printf( "type[%u] %s  kind=%s size=%u align=%u frame=%u schema=0x%08x %s=%u\n",
            type_id, rs_cstr( t->name_id ), kind_str,
            t->size, t->align, t->frame_id, t->schema_hash, member_word, t->field_count );

    if ( rs_kind_is_enum( (rs_kind_t)t->kind ) )
    {
        for ( uint16_t i = 0; i < t->field_count; i++ )
        {
            uint16_t eid = (uint16_t)( t->field_index + i );
            const rs_enum_t* e = &g_rs.enums[ eid ];
            printf( "    enum[%u] %-24s = %lld\n", eid, rs_cstr( e->name_id ), (long long)e->value );
        }
        return;
    }

    if ( t->kind == RS_KIND_FUNCTION )
    {
        char typebuf[ 128 ];
        const rs_field_t* ret = rs_function_get_return( type_id );
        if ( ret ) { rs_field_describe( ret, typebuf, sizeof( typebuf ) ); printf( "    returns : %s\n", typebuf ); }
        uint16_t np = rs_function_param_count( type_id );
        for ( uint16_t i = 0; i < np; i++ )
        {
            const rs_field_t* p = rs_function_get_param( type_id, i );
            rs_field_describe( p, typebuf, sizeof( typebuf ) );
            printf( "    param[%u] %-16s : %s\n", i, rs_cstr( p->name_id ), typebuf );
        }
        return;
    }

    char typebuf[ 128 ];
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t          fid = (uint16_t)( t->field_index + i );
        const rs_field_t* f   = &g_rs.fields[ fid ];

        rs_field_describe( f, typebuf, sizeof( typebuf ) );
        printf( "    field[%u] %-16s : %-28s offset=%u size=%u", fid, rs_cstr( f->name_id ), typebuf, f->offset, f->size );
        if ( f->attr_count > 0 ) printf( "  attrs=%u", f->attr_count );
        printf( "\n" );

        for ( uint16_t a = 0; a < f->attr_count; a++ )
        {
            const rs_attrib_t* attr = &g_rs.attrs[ f->attr_index + a ];
            printf( "        @%s = ", rs_cstr( attr->name_id ) );
            switch ( attr->type )
            {
                case RS_ATTR_INT:    printf( "%d\n",     attr->value.i32 ); break;
                case RS_ATTR_FLOAT:  printf( "%g\n",     attr->value.f32 ); break;
                case RS_ATTR_BOOL:   printf( "%s\n",     attr->value.u32 ? "true" : "false" ); break;
                case RS_ATTR_STRING: printf( "\"%s\"\n", rs_cstr( attr->value.str ) ); break;
                default:             printf( "(none)\n" ); break;
            }
        }
    }
}

void
rs_print_types( void )
{
    printf( "rs: %u types, %u fields, %u attrs across %u frames\n",
            g_rs.type_count, g_rs.field_count, g_rs.attr_count, g_rs.frame_count );
    for ( uint16_t i = 0; i < g_rs.type_count; i++ )
    {
        const rs_type_t* t = &g_rs.types[ i ];
        printf( "  [%3u] frame=%u %-20s size=%-4u fields=%u\n", i, t->frame_id, rs_cstr( t->name_id ), t->size, t->field_count );
    }
}

void
rs_print_frame( uint16_t frame_id )
{
    const rs_frame_t* f = rs_get_frame( frame_id );
    if ( !f ) { printf( "rs_print_frame: invalid id %u\n", frame_id ); return; }

    uint16_t type_end = ( frame_id + 1 == g_rs.frame_count )
                       ? g_rs.type_count
                       : g_rs.frames[ frame_id + 1 ].first_type;

    printf( "frame[%u] %s  types[%u..%u) fields_start=%u attrs_start=%u\n",
            frame_id, rs_cstr( f->name_id ),
            f->first_type, type_end, f->first_field, f->first_attr );

    for ( uint16_t i = f->first_type; i < type_end; i++ ) rs_print_type( i );
}

/*============================================================================================*/
