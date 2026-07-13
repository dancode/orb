#ifndef FS_ZIP_H
#define FS_ZIP_H
/*==============================================================================================

    engine/fs/fs_zip.h -- shared miniz build configuration for the fs ZIP mount reader.

    Both the implementation TU (fs_zip_miniz.c, which compiles vendor/miniz.c) and the caller
    (fs.c, which includes vendor/miniz.h) include this FIRST so the two agree on how miniz is
    configured.  We keep stdio out of miniz -- the engine reads every byte through the sys layer
    (whole .zip into memory, then mz_zip_reader_init_mem), so miniz never touches the disk.
    Archive WRITING stays enabled: tests build zips in memory, and the cook track (asset_tool)
    will want it later.

==============================================================================================*/

#define MINIZ_NO_STDIO

/*============================================================================================*/
#endif    // FS_ZIP_H
