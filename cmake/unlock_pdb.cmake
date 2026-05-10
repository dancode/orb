# cmake/unlock_pdb.cmake
#
# Called as a PRE_LINK step for hot-reloadable module targets.
# Renames the existing PDB so the linker can write a fresh one while
# a debugger holds the old file open.
#
# Usage (from CMakeLists.txt):
#   cmake -D PDB_PATH=<path/to/target.pdb> -P cmake/unlock_pdb.cmake

if(NOT DEFINED PDB_PATH)
    message(FATAL_ERROR "unlock_pdb.cmake: PDB_PATH not set")
endif()

if(EXISTS "${PDB_PATH}")

    # Best-effort sweep of leftover .locked.* files from previous builds.
    # Files still held by a live debugger will silently refuse to delete.
    file(GLOB OLD_LOCKED "${PDB_PATH}.locked.*")
    foreach(F IN LISTS OLD_LOCKED)
        file(REMOVE "${F}")
    endforeach()

    # Rename the current PDB to a unique name.
    # The debugger keeps its handle on the renamed file; the linker
    # is now free to create a fresh PDB at the original path.
    string(RANDOM LENGTH 8 ALPHABET "abcdefghijklmnopqrstuvwxyz0123456789" SUFFIX)
    file(RENAME "${PDB_PATH}" "${PDB_PATH}.locked.${SUFFIX}")

endif()
# If the PDB doesn't exist yet (first build) this script is a no-op.