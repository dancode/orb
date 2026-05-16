/*==============================================================================================

    rs_gen_parse.c - parse annotated headers for RS_STRUCT / RS_ENUM / RS_BITSET markers

    Phase 1 stub: marker detection and counting only.
    Full declarator parsing (fields, modifiers, attributes) comes in a later phase.

==============================================================================================*/

#include "rs_gen_internal.h"

void
rg_parse( const rg_file_list_t* files, rg_parse_data_t* out )
{
    out->type_count = 0;
    out->enum_count = 0;

    for ( int i = 0; i < files->count; i++ )
    {
        FILE* f = fopen( files->paths[ i ], "rb" );
        if ( !f )
            continue;

        fseek( f, 0, SEEK_END );
        long size = ftell( f );
        fseek( f, 0, SEEK_SET );

        if ( size <= 0 || size > 1024 * 1024 )
        {
            fclose( f );
            continue;
        }

        char* buf = (char*)malloc( (size_t)( size + 1 ) );
        if ( !buf )
        {
            fclose( f );
            continue;
        }

        size_t read = fread( buf, 1, (size_t)size, f );
        fclose( f );
        buf[ read ] = '\0';

        // Count RS_STRUCT / RS_ENUM / RS_BITSET markers
        const char* cursor = buf;
        while ( *cursor )
        {
            if ( strncmp( cursor, "RS_STRUCT", 9 ) == 0 )
                out->type_count++;
            else if ( strncmp( cursor, "RS_ENUM", 7 ) == 0 )
                out->enum_count++;
            else if ( strncmp( cursor, "RS_BITSET", 9 ) == 0 )
                out->enum_count++;
            cursor++;
        }

        free( buf );
    }
}
