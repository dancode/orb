/*==============================================================================================

    sys.h - Platform primitive types and sys_api_t accessor.
    Include this in DLL modules that use sys through the vtable (sys()->tick_seconds() etc.).
    Include engine/sys/sys_host.h instead when you need direct function calls (host/tests).

    sys is always statically linked into the host executable — it is never in a DLL.

==============================================================================================*/
#ifndef SYS_H
#define SYS_H

#include "orb.h"

/*==============================================================================================

    Thread - Opaque handle and ID types. Stored in module state structs; creation and
             management functions are in sys_host.h.

==============================================================================================*/

#define THREAD_HANDLE_BYTES 8

typedef void ( *thread_fn_t )( void* arg );

typedef struct
{
    u8 _opaque[ THREAD_HANDLE_BYTES ];

} thread_t;

typedef u64 thread_id_t;

/*==============================================================================================

    Mutex - Opaque mutual exclusion lock. Stored in module state structs; init/destroy/lock
            functions are in sys_host.h.

==============================================================================================*/

#define MUTEX_BYTES 64

typedef struct
{
    u8 _opaque[ MUTEX_BYTES ];

} mutex_t;

/*==============================================================================================

    Semaphore - Opaque counting semaphore. Stored in module state structs; init/destroy/wait
                functions are in sys_host.h.

==============================================================================================*/

#define SEMA_BYTES 32

typedef struct
{
    u8 _opaque[ SEMA_BYTES ];

} sema_t;

/*==============================================================================================

    Socket - UDP socket handle and wire address. Value types stored by the net module;
             open/send/recv functions are in sys_host.h.

==============================================================================================*/

typedef enum sys_addr_type_e
{
    SYS_ADDR_NONE = 0,
    SYS_ADDR_IPV4,
    SYS_ADDR_IPV6,

} sys_addr_type_t;

/* Wire address. `ip` holds the address bytes in network order as delivered by the OS
   (IPv4 uses ip[0..3]); `port` is host byte order. Copy freely; compare with
   sys_addr_equal(), never memcmp (padding + unused ip bytes). */
typedef struct sys_addr_s
{
    u8  type;      // sys_addr_type_t
    u8  ip[ 16 ];  // ipv4 in ip[0..3]
    u16 port;      // host byte order

} sys_addr_t;

typedef u64 sys_socket_t;    // opaque OS socket handle

#define SYS_SOCKET_INVALID ( ( sys_socket_t ) ~0ull )

/*============================================================================================*/
#endif    // SYS_H
