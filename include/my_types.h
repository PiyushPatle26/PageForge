#ifndef MY_TYPES_H
#define MY_TYPES_H

/*
 * my_types.h — basic types without pulling in libc headers.
 * We only use compiler-provided built-ins here.
 */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed int          int32_t;

typedef unsigned long       size_t;
typedef unsigned long       uintptr_t;

#define NULL  ((void *)0)
#define TRUE  1
#define FALSE 0

/* Page size — same as the Linux kernel default */
#define PAGE_SIZE  4096UL

#endif /* MY_TYPES_H */
