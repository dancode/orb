/*==============================================================================================

    core/rs/rs_print.c - Diagnostics and human-readable type formatting.

==============================================================================================*/

/*==============================================================================================
    rs_field_describe

    Walk the packed modifier chain and emit a C-style type string into `buf`.
    Reading direction: base type on the left, modifiers wrap outward to the right.

    Example output:
        "vec3_t"            base struct
        "const vec3_t*"     base_const + PTR
        "vec3_t* const"     PTR with const-on-this-wrapper
        "vec3_t*[8]"        PTR then ARRAY, aux=8
        "vec3_t(*)[8]"      ARRAY then PTR, aux=8
==============================================================================================*/

static int
rs_str_append( char* buf, size_t cap, size_t pos, const char* s )
{
    while ( *s && pos + 1 < cap )
        buf[ pos++ ] = *s++;
    if ( pos < cap )
        buf[ pos ] = '\0';
    return (int)pos;
}

size_t
rs_field_describe( const rs_field_t* f, char* buf, size_t buf_size )
{
    if ( !buf || buf_size == 0 ) return 0;
    buf[ 0 ] = '\0';
    if ( !f ) return 0;

    size_t pos = 0;

    /* Base type, with leading const if base is const-qualified. */
    if ( f->base_const )
        pos = rs_str_append( buf, buf_size, pos, "const " );

    const rs_type_t* base = rs_get_type( f->type_id );
    pos = rs_str_append( buf, buf_size, pos, base ? sid_cstr( base->name_sid ) : "<unresolved>" );

    /* Detect whether any slot is a PTR-following-ARRAY, which needs parentheses
       in C syntax ("T(*)[N]" rather than "T*[N]"). We do a quick scan first to
       decide. */
    bool needs_parens   = false;
    int  array_after_ptr = 0;
    for ( int slot = 0; slot < 4; slot++ )
    {
        uint8_t s = RS_MOD_GET( f->mods, slot );
        if ( RS_MOD_OP( s ) == RS_MOD_NONE )
            break;
        if ( RS_MOD_OP( s ) == RS_MOD_PTR && array_after_ptr )
        {
            needs_parens = true;
            break;
        }
        if ( RS_MOD_OP( s ) == RS_MOD_ARRAY )
            array_after_ptr = 1;
    }

    if ( needs_parens )
        pos = rs_str_append( buf, buf_size, pos, "(" );

    /* Emit each slot in order. */
    for ( int slot = 0; slot < 4; slot++ )
    {
        uint8_t s = RS_MOD_GET( f->mods, slot );
        rs_mod_op_t op = RS_MOD_OP( s );
        if ( op == RS_MOD_NONE )
            break;

        switch ( op )
        {
            case RS_MOD_PTR:
                pos = rs_str_append( buf, buf_size, pos, "*" );
                if ( RS_MOD_IS_CONST( s ) )
                    pos = rs_str_append( buf, buf_size, pos, " const" );
                break;

            case RS_MOD_ARRAY:
            {
                if ( needs_parens && pos < buf_size )
                {
                    buf[ pos++ ] = ')';
                    if ( pos < buf_size ) buf[ pos ] = '\0';
                    needs_parens = false;
                }
                char tmp[ 24 ];
                snprintf( tmp, sizeof( tmp ), "[%u]", (unsigned)f->aux );
                pos = rs_str_append( buf, buf_size, pos, tmp );
                break;
            }

            case RS_MOD_FUNCTION:
            {
                /* aux holds the function-signature type_id. Emit its name in parens
                   so the field reads as e.g. "void(on_die_sig_t)". */
                const rs_type_t* sig = rs_get_type( f->aux );
                if ( sig && sig->kind == RS_KIND_FUNCTION )
                {
                    pos = rs_str_append( buf, buf_size, pos, "(" );
                    pos = rs_str_append( buf, buf_size, pos, sid_cstr( sig->name_sid ) );
                    pos = rs_str_append( buf, buf_size, pos, ")" );
                }
                else
                {
                    pos = rs_str_append( buf, buf_size, pos, "(...)" );
                }
                break;
            }

            default:
                break;
        }
    }

    if ( needs_parens )
        pos = rs_str_append( buf, buf_size, pos, ")" );

    return pos;
}

/*==============================================================================================
    rs_print_type
==============================================================================================*/

void
rs_print_type( uint16_t type_id )
{
    const rs_type_t* t = rs_get_type( type_id );
    if ( !t )
    {
        printf( "rs_print_type: invalid id %u\n", type_id );
        return;
    }

    const char* kind_str =
        ( t->kind == RS_KIND_PRIM     ) ? "prim"     :
        ( t->kind == RS_KIND_STRUCT   ) ? "struct"   :
        ( t->kind == RS_KIND_ENUM     ) ? "enum"     :
        ( t->kind == RS_KIND_UNION    ) ? "union"    :
        ( t->kind == RS_KIND_FUNCTION ) ? "function" : "?";

    const char* member_word =
        ( t->kind == RS_KIND_ENUM     ) ? "enumerators" :
        ( t->kind == RS_KIND_FUNCTION ) ? "params+1"    : "fields";

    printf( "type[%u] %s  kind=%s size=%u align=%u frame=%u schema=0x%08x %s=%u\n",
            type_id, sid_cstr( t->name_sid ), kind_str,
            t->size, t->align, t->frame_id, t->schema_hash, member_word, t->field_count );

    if ( t->kind == RS_KIND_ENUM )
    {
        for ( uint16_t i = 0; i < t->field_count; i++ )
        {
            uint16_t eid = (uint16_t)( t->field_index + i );
            const rs_enumerator_t* e = &g_rs.enums[ eid ];
            printf( "    enum[%u] %-24s = %lld\n",
                    eid, sid_cstr( e->name_sid ), (long long)e->value );
        }
        return;
    }

    if ( t->kind == RS_KIND_FUNCTION )
    {
        char typebuf[ 128 ];
        const rs_field_t* ret = rs_function_get_return( type_id );
        if ( ret )
        {
            rs_field_describe( ret, typebuf, sizeof( typebuf ) );
            printf( "    returns : %s\n", typebuf );
        }
        uint16_t np = rs_function_param_count( type_id );
        for ( uint16_t i = 0; i < np; i++ )
        {
            const rs_field_t* p = rs_function_get_param( type_id, i );
            rs_field_describe( p, typebuf, sizeof( typebuf ) );
            printf( "    param[%u] %-16s : %s\n", i, sid_cstr( p->name_sid ), typebuf );
        }
        return;
    }

    char typebuf[ 128 ];
    for ( uint16_t i = 0; i < t->field_count; i++ )
    {
        uint16_t          fid = (uint16_t)( t->field_index + i );
        const rs_field_t* f   = &g_rs.fields[ fid ];

        rs_field_describe( f, typebuf, sizeof( typebuf ) );

        printf( "    field[%u] %-16s : %-28s offset=%u size=%u",
                fid, sid_cstr( f->name_sid ), typebuf, f->offset, f->size );

        if ( f->attr_count > 0 )
            printf( "  attrs=%u", f->attr_count );
        printf( "\n" );

        for ( uint16_t a = 0; a < f->attr_count; a++ )
        {
            const rs_attrib_t* attr = &g_rs.attrs[ f->attr_index + a ];
            printf( "        @%s = ", sid_cstr( attr->name_sid ) );
            switch ( attr->type )
            {
                case RS_ATTR_INT:    printf( "%d\n",  attr->value.i32 ); break;
                case RS_ATTR_FLOAT:  printf( "%g\n",  attr->value.f32 ); break;
                case RS_ATTR_BOOL:   printf( "%s\n",  attr->value.u32 ? "true" : "false" ); break;
                case RS_ATTR_STRING: printf( "\"%s\"\n", sid_cstr( attr->value.str ) ); break;
                default:             printf( "(none)\n" ); break;
            }
        }
    }
}

/*==============================================================================================
    rs_print_types  -  table summary
==============================================================================================*/

void
rs_print_types( void )
{
    printf( "rs: %u types, %u fields, %u attrs across %u frames\n",
            g_rs.type_count, g_rs.field_count, g_rs.attr_count, g_rs.frame_count );

    for ( uint16_t i = 0; i < g_rs.type_count; i++ )
    {
        const rs_type_t* t = &g_rs.types[ i ];
        printf( "  [%3u] frame=%u %-20s size=%-4u fields=%u\n",
                i, t->frame_id, sid_cstr( t->name_sid ), t->size, t->field_count );
    }
}

/*==============================================================================================
    rs_print_frame
==============================================================================================*/

void
rs_print_frame( uint8_t frame_id )
{
    const rs_frame_t* f = rs_get_frame( frame_id );
    if ( !f )
    {
        printf( "rs_print_frame: invalid id %u\n", frame_id );
        return;
    }

    uint16_t type_end = ( frame_id + 1 == g_rs.frame_count )
                       ? g_rs.type_count
                       : g_rs.frames[ frame_id + 1 ].first_type;

    printf( "frame[%u] %s v%u  types[%u..%u) fields_start=%u attrs_start=%u\n",
            frame_id, sid_cstr( f->name_sid ), f->version,
            f->first_type, type_end, f->first_field, f->first_attr );

    for ( uint16_t i = f->first_type; i < type_end; i++ )
        rs_print_type( i );
}

/*============================================================================================*/
