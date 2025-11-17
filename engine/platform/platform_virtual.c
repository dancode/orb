/*==============================================================================================

    platform_virtual.c

    -- virtual memrory management implementation for Windows --

==============================================================================================*/

#include "orb.h"
#include "platform.h"

#ifdef PLATFORM_WINDOWS

#    include <windows.h>
#    include <memoryapi.h>    // For VirtualAlloc/VirtualFree

/*============================================================================================*/

static size_t g_page_size = 0; /* cached page size */

static size_t
win32_get_page_size( void )
{
    /* Helper to get and cache the system page size. */

    if ( g_page_size == 0 )
    {
        SYSTEM_INFO si;
        GetSystemInfo( &si );
        g_page_size = si.dwPageSize;
    }
    return g_page_size;
}

static size_t
win32_round_up_to_page_size( size_t size )
{
    /* Helper to round a size up to the nearest page size. */

    size_t page_size = win32_get_page_size();
    return ( ( size + page_size - 1 ) / page_size ) * page_size;
}

/*============================================================================================*/

size_t
vm_get_page_size( void )
{
    return win32_get_page_size();
}

void*
vm_reserve( size_t size )
{
    size_t rounded_size = win32_round_up_to_page_size( size );
    if ( rounded_size == 0 )
        return NULL;

    return VirtualAlloc( NULL, rounded_size, MEM_RESERVE, PAGE_NOACCESS );
}

bool
vm_commit( void* ptr, size_t size )
{
    size_t rounded_size = win32_round_up_to_page_size( size );
    if ( rounded_size == 0 )
        return true;    // Commit 0 bytes is a success

    return VirtualAlloc( ptr, rounded_size, MEM_COMMIT, PAGE_READWRITE ) != NULL;
}

void
vm_decommit( void* ptr, size_t size )
{
    size_t rounded_size = win32_round_up_to_page_size( size );
    if ( rounded_size == 0 )
        return;

    UNUSED( ptr );
    // VirtualFree( ptr, rounded_size, MEM_DECOMMIT );
}

void
vm_release( void* ptr, size_t size )
{
    // MEM_RELEASE requires size to be 0 and frees the *entire*
    // block that ptr belongs to, regardless of the size passed
    // to VirtualAlloc (which is why vm_reserve returns the base).
    // The 'size' parameter is unused here, but required by
    // the cross-platform vm_release API.
    ( void )size;
    VirtualFree( ptr, 0, MEM_RELEASE );
}

#endif    // PLATFORM_WINDOWS

#ifdef PLATFORM_LINUX

#include <sys/mman.h>
#include <unistd.h>

static size_t g_page_size = 0;

static size_t
linux_get_page_size(void)
{
    if (g_page_size == 0)
    {
        g_page_size = sysconf(_SC_PAGESIZE);
    }
    return g_page_size;
}

static size_t
linux_round_up_to_page_size(size_t size)
{
    size_t page_size = linux_get_page_size();
    return ((size + page_size - 1) / page_size) * page_size;
}

size_t
vm_get_page_size(void)
{
    return linux_get_page_size();
}

void*
vm_reserve(size_t size)
{
    size_t rounded_size = linux_round_up_to_page_size(size);
    if (rounded_size == 0)
        return NULL;

    void* ptr = mmap(NULL, rounded_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;

    return ptr;
}

bool
vm_commit(void* ptr, size_t size)
{
    size_t rounded_size = linux_round_up_to_page_size(size);
    if (rounded_size == 0)
        return true;

    return mprotect(ptr, rounded_size, PROT_READ | PROT_WRITE) == 0;
}

void
vm_decommit(void* ptr, size_t size)
{
    size_t rounded_size = linux_round_up_to_page_size(size);
    if (rounded_size == 0)
        return;

    madvise(ptr, rounded_size, MADV_DONTNEED);
    mprotect(ptr, rounded_size, PROT_NONE);
}

void
vm_release(void* ptr, size_t size)
{
    size_t rounded_size = linux_round_up_to_page_size(size);
    if (rounded_size == 0)
        return;
    munmap(ptr, rounded_size);
}

#endif // PLATFORM_LINUX