/* engine/ref/ref_print.c - Diagnostics: type description strings and console dump.

   These are debug/editor utilities -- not used in shipping builds. ref_field_describe
   reconstructs a C-style declaration string (e.g. "const vec3_t*") from packed mods.
   ref_print_type and ref_print_frame dump human-readable type info to stdout. */

/*==============================================================================================
    String Helpers

    Safe bounded append -- writes characters into buf[pos..cap-2], always null-terminates.
    Returns the new position (not the length), so the caller can chain calls without
    tracking intermediate string lengths. Does not use printf/sprintf to avoid format-string
    overhead for simple string assembly.
==============================================================================================*/

static int
ref_str_append( char* buf, size_t cap, size_t pos, const char* s )
{
    while ( *s && pos + 1 < cap ) buf[ pos++ ] = *s++;
    if ( pos < cap ) buf[ pos ] = '\0';
    return (int)pos;
}

/*==============================================================================================
    Type Description

    Reconstructs a C-style type declaration string for a field by reading its mods value.
    Examples: "const vec3_t*", "vec3_t*[8]", "vec3_t(*)[8]", "float[4]".
    Used by the editor inspector and by ref_print_type for diagnostic output.
    Returns the number of characters written (not counting the null terminator).
==============================================================================================*/

size_t
ref_field_describe( const ref_field_t* f, char* buf, size_t buf_size )
{
    if ( !buf || buf_size == 0 ) return 0;
    buf[ 0 ] = '\0';
    if ( !f ) return 0;

    size_t pos = 0;

    /* Prepend "const " if the base type is const-qualified (covers both const T and const T*). */
    if ( f->mods == REF_MODS_CONST_VALUE || f->mods == REF_MODS_PTR_TO_CONST )
        pos = ref_str_append( buf, buf_size, pos, "const " );

    /* Emit the base type name; fall back to "<unresolved>" for fields that failed finalization. */
    const ref_type_t* base = ref_get_type( f->type_id );
    pos = ref_str_append( buf, buf_size, pos, base ? ref_cstr( base->name_id ) : "<unresolved>" );

    char tmp[ 24 ];
    switch ( (ref_mods_t)f->mods )
    {
        case REF_MODS_VALUE:
        case REF_MODS_CONST_VALUE:
            break;
        case REF_MODS_PTR:
        case REF_MODS_PTR_TO_CONST:
            pos = ref_str_append( buf, buf_size, pos, "*" ); break;
        case REF_MODS_PTR_PTR:      pos = ref_str_append( buf, buf_size, pos, "**" ); break;
        case REF_MODS_CONST_PTR:    pos = ref_str_append( buf, buf_size, pos, "* const" ); break;
        case REF_MODS_ARRAY:
            snprintf( tmp, sizeof( tmp ), "[%u]", (unsigned)f->aux );
            pos = ref_str_append( buf, buf_size, pos, tmp );
            break;
        case REF_MODS_PTR_ARRAY:
            snprintf( tmp, sizeof( tmp ), "*[%u]", (unsigned)f->aux );
            pos = ref_str_append( buf, buf_size, pos, tmp );
            break;
        case REF_MODS_ARRAY_PTR:
            snprintf( tmp, sizeof( tmp ), "(*)[%u]", (unsigned)f->aux );
            pos = ref_str_append( buf, buf_size, pos, tmp );
            break;
        case REF_MODS_FUNCTION:
        {
            const ref_type_t* sig = ref_get_type( f->aux );
            if ( sig && sig->kind == REF_KIND_FUNCTION )
            {
                pos = ref_str_append( buf, buf_size, pos, "(" );
                pos = ref_str_append( buf, buf_size, pos, ref_cstr( sig->name_id ) );
                pos = ref_str_append( buf, buf_size, pos, ")" );
            }
            else
                pos = ref_str_append( buf, buf_size, pos, "(...)" );
            break;
        }
    }

    return pos;
}

/*==============================================================================================
    Diagnostic Printing

    Console dumps of the registry for debugging and development. ref_print_type dispatches
    on kind so that enums, function signatures, and structs each display in the most readable
    format for their content. These functions are intentionally simple -- clarity over brevity.
==============================================================================================*/

void
ref_print_type( uint16_t type_id )
{
    const ref_type_t* t = ref_get_type( type_id );
    if ( !t ) { printf( "ref_print_type: invalid id %u\n", type_id ); return; }

    const char* kind_str =
        ( t->kind == REF_KIND_PRIM     ) ? "prim"     :
        ( t->kind == REF_KIND_STRUCT   ) ? "struct"   :
        ( t->kind == REF_KIND_ENUM     ) ? "enum"     :
        ( t->kind == REF_KIND_BITSET   ) ? "bitset"   :
        ( t->kind == REF_KIND_UNION    ) ? "union"    :
        ( t->kind == REF_KIND_FUNCTION ) ? "function" : "?";

    const char* member_word =
        ref_kind_is_enum( (ref_kind_t)t->kind ) ? "enumerators" :
        ( t->kind == REF_KIND_FUNCTION )        ? "params+1"    : "fields";

    printf( "type[%u] %s  kind=%s size=%u align=%u frame=%u schema=0x%08x %s=%u\n",
            type_id, ref_cstr( t->name_id ), kind_str,
            t->size, t->align, t->frame_id, t->schema_hash, member_word, t->field_count );

    if ( ref_kind_is_enum( (ref_kind_t)t->kind ) )
    {
        for ( uint16_t i = 0; i < t->field_count; i++ )
        {
            uint16_t eid = (uint16_t)( t->field_index + i );
            const ref_enum_t* e = &g_ref.enums[ eid ];
            printf( "    enum[%u] %-24s = %lld\n", eid, ref_cstr( e->name_id ), (long long)e->value );
        }
        return;
    }

    if ( t->kind == REF_KIND_FUNCTION )
    {
        char typebuf[ 128 ];
        const ref_field_t* ret = ref_function_get_return( type_id );
        if ( ret ) { ref_field_describe( ret, typebuf, sizeof( typebuf ) ); printf( "    returns : %s\n", typebuf ); }
        uint16_t np = ref_function_param_count( type_id );
        for ( uint16_t i = 0; i < np; i++ )
        {
            const ref_field_t* p = ref_function_get_param( type_id, i );
            ref_field_describe( p, typebuf, sizeof( typebuf ) );
            printf( "    param[%u] %-16s : %s\n", i, ref_cstr( p->name_id ), typebuf );
        }
        return;
    }

    char typebuf[ 128 ];
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t          fid = (uint16_t)( t->field_index + i );
        const ref_field_t* f   = &g_ref.fields[ fid ];

        ref_field_describe( f, typebuf, sizeof( typebuf ) );
        printf( "    field[%u] %-16s : %-28s offset=%u size=%u", fid, ref_cstr( f->name_id ), typebuf, f->offset, f->size );
        if ( f->attr_count > 0 ) printf( "  attrs=%u", f->attr_count );
        printf( "\n" );

        for ( uint16_t a = 0; a < f->attr_count; a++ )
        {
            const ref_attrib_t* attr = &g_ref.attrs[ f->attr_index + a ];
            printf( "        @%s = ", ref_cstr( attr->name_id ) );
            switch ( attr->type )
            {
                case REF_ATTR_INT:    printf( "%d\n",     attr->value.i32 ); break;
                case REF_ATTR_FLOAT:  printf( "%g\n",     attr->value.f32 ); break;
                case REF_ATTR_BOOL:   printf( "%s\n",     attr->value.u32 ? "true" : "false" ); break;
                case REF_ATTR_STRING: printf( "\"%s\"\n", ref_cstr( attr->value.str ) ); break;
                default:             printf( "(none)\n" ); break;
            }
        }
    }
}

void
ref_print_types( void )
{
    printf( "ref: %u types, %u fields, %u attrs across %u frames\n",
            g_ref.type_count, g_ref.field_count, g_ref.attr_count, g_ref.frame_count );
    for ( uint16_t i = 0; i < g_ref.type_count; i++ )
    {
        const ref_type_t* t = &g_ref.types[ i ];
        printf( "  [%3u] frame=%u %-20s size=%-4u fields=%u\n", i, t->frame_id, ref_cstr( t->name_id ), t->size, t->field_count );
    }
}

void
ref_print_frame( uint16_t frame_id )
{
    const ref_frame_t* f = ref_get_frame( frame_id );
    if ( !f ) { printf( "ref_print_frame: invalid id %u\n", frame_id ); return; }

    uint16_t type_end = ( frame_id + 1 == g_ref.frame_count )
                       ? g_ref.type_count
                       : g_ref.frames[ frame_id + 1 ].first_type;

    printf( "frame[%u] %s  types[%u..%u) fields_start=%u attref_start=%u\n",
            frame_id, ref_cstr( f->name_id ),
            f->first_type, type_end, f->first_field, f->first_attr );

    for ( uint16_t i = f->first_type; i < type_end; i++ ) ref_print_type( i );
}

/*============================================================================================*/
