#ifndef __MMAN_H__
#define __MMAN_H__

#include <stddef.h>
#include <sys/types.h>

#define MAP_FAILED (void *)-1

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define PROT_NONE 0x0

#define MAP_PRIVATE 0x1
#define MAP_SHARED 0x2
#define MAP_FIXED 0x4
#define MAP_ANON 0x8
#define MAP_ANONYMOUS MAP_ANON

void *mmap(void *addr, size_t length, int prot, int flags, int fd,
           off_t offset);
int munmap(void *addr, size_t length);

#endif
