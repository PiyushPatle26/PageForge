#ifndef MY_TYPES_H
#define MY_TYPES_H

/*
 * my_types.h — basic types without pulling in libc headers.
 *
 * When compiled alongside Unity (which includes <stdint.h>), the standard
 * types are already defined. We guard each typedef with __has_include checks
 * so the test build doesn't conflict with the system stdint.h.
 */

#ifndef __uint8_t_defined
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef signed int          int32_t;
#define __uint8_t_defined
#endif

#ifndef __uint64_t_defined
/* Match the platform ABI: unsigned long on LP64, unsigned long long on LLP64 */
typedef unsigned long       uint64_t;
#define __uint64_t_defined
#endif

#ifndef __size_t_defined
typedef unsigned long       size_t;
#define __size_t_defined
#endif

#ifndef __uintptr_t_defined
typedef unsigned long       uintptr_t;
#define __uintptr_t_defined
#endif

#define NULL  ((void *)0)
#define TRUE  1
#define FALSE 0

/* Page size — same as the Linux kernel default */
#define PAGE_SIZE  4096UL

#endif /* MY_TYPES_H */
