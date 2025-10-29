#ifndef VIRTUAL_MEMORY_H
#define VIRTUAL_MEMORY_H

#include <stddef.h>     // For size_t
#include <stdbool.h>    // For bool
#include "orb.h"

/**
 * @brief Gets the OS's memory page size.
 *
 * All virtual memory operations are aligned to this size.
 */
size_t vm_get_page_size( void );

/**
 * @brief Reserves a contiguous block of virtual address space.
 *
 * This function only reserves the address range; it does not
 * allocate any physical memory. The reserved memory is
 * initially inaccessible.
 *
 * @param size The total size (in bytes) to reserve.
 * **Note:** This value will be rounded UP to the nearest
 * OS page size.
 * @return A pointer to the base of the reserved block, or NULL on failure.
 */
void* vm_reserve( size_t size );

/**
 * @brief Commits physical memory to a region of a reserved block.
 *
 * The memory region is made readable and writable.
 *
 * @param ptr A pointer to the start of the region to commit
 * (must be page-aligned, which the base from vm_reserve is).
 * @param size The size (in bytes) to commit.
 * **Note:** This value will be rounded UP to the nearest
 * OS page size.
 * @return true on success, false on failure.
 */
bool vm_commit( void* ptr, size_t size );

/**
 * @brief De-commits physical memory from a committed region.
 *
 * The memory is returned to the OS, and the region becomes
 * inaccessible again (but remains reserved).
 *
 * @param ptr A pointer to the start of the region to de-commit.
 * @param size The size (in bytes) to de-commit.
 * **Note:** This value will be rounded UP to the nearest
 * OS page size.
 */
void vm_decommit( void* ptr, size_t size );

/**
 * @brief Releases the entire reserved virtual address space.
 *
 * All memory (committed or not) in the block is returned to the OS.
 *
 * @param ptr The base pointer returned by vm_reserve.
 * @param size The total size of the reservation.
 * **Note:** This MUST be the same 'size' value
 * passed to vm_reserve(). The implementation will
 * handle rounding it correctly.
 */
void vm_release( void* ptr, size_t size );

#endif    // VIRTUAL_MEMORY_H