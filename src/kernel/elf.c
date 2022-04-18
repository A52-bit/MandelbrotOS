#include <elf.h>
#include <fb/fb.h>
#include <fs/vfs.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <printf.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <tasking/scheduler.h>

#define ALIGN_UP(__addr, __align) (((__addr) + (__align)-1) & ~((__align)-1))

#define ELF_RELOCATEABLE 1
#define ELF_EXECUTABLE 2

#define ELF_HEAD_LOAD 1

#define ELF_SECT_NOBITS 8

char elf_ident[4] = {0x7f, 'E', 'L', 'F'};

// TYSM OSDEV wiki
static inline elf_sect_header_t *elf_sheader(elf_header_t *hdr) {
  return (elf_sect_header_t *)((uintptr_t)(uint64_t)hdr + hdr->sect_head_off);
}

uint8_t elf_run_binary(char *path, proc_t *proc, int auto_enqueue) {
  fs_file_t *file = vfs_open(path);
  uint8_t *buffer = kmalloc(file->length);
  vfs_read(file, buffer, 0, file->length);

  elf_header_t *header = (elf_header_t *)buffer;
  if (header->type != ELF_EXECUTABLE)
    return 1;
  if (memcmp((void *)header->identifier, elf_ident, 4))
    return 1;

  elf_sect_header_t *sect_headers = elf_sheader(header);

  for (size_t i = 0; i < header->sect_head_count; i++) {
    elf_sect_header_t *sect_header = &sect_headers[i];
    if (!sect_header->size)
      continue;
    else if (sect_header->type == ELF_SECT_NOBITS) {
      uintptr_t mem =
        (uintptr_t)pmalloc(ALIGN_UP(sect_header->size, PAGE_SIZE) / PAGE_SIZE);
      for (size_t j = 0; j < ALIGN_UP(sect_header->size, PAGE_SIZE) / PAGE_SIZE;
           j += PAGE_SIZE)
        vmm_map_page(proc->pagemap, mem + j, sect_header->addr + j, 0b111);

      mmap_range_t *mmap_range = kmalloc(sizeof(mmap_range_t));
      *mmap_range = (mmap_range_t){
        .file = NULL,
        .flags = MAP_FIXED | MAP_ANON,
        .length = ALIGN_UP(sect_header->size, PAGE_SIZE),
        .offset = 0,
        .prot = PROT_READ | PROT_WRITE | PROT_EXEC,
        .phys_addr = mem,
        .virt_addr = sect_header->addr,
      };

      vec_push(&proc->pagemap->ranges, mmap_range);
    }
  }

  elf_prog_header_t *prog_header = (void *)(buffer + header->prog_head_off);

  for (size_t i = 0; i < header->prog_head_count; i++) {
    if (prog_header->type == ELF_HEAD_LOAD) {
      uintptr_t mem = (uintptr_t)pmalloc(
        ALIGN_UP(prog_header->mem_size, PAGE_SIZE) / PAGE_SIZE);

      for (size_t j = 0;
           j < ALIGN_UP(prog_header->mem_size, PAGE_SIZE) / PAGE_SIZE;
           j += PAGE_SIZE)
        vmm_map_page(proc->pagemap, mem + j, prog_header->virt_addr + j, 0b111);

      mmap_range_t *mmap_range = kmalloc(sizeof(mmap_range_t));
      *mmap_range = (mmap_range_t){
        .file = NULL,
        .flags = MAP_FIXED | MAP_ANON,
        .length = ALIGN_UP(prog_header->mem_size, PAGE_SIZE),
        .offset = 0,
        .prot = PROT_READ | PROT_WRITE | PROT_EXEC,
        .phys_addr = mem,
        .virt_addr = prog_header->virt_addr,
      };

      vec_push(&proc->pagemap->ranges, mmap_range);

      memset((void *)mem, 0, prog_header->mem_size);
      memcpy((void *)mem, (void *)((uint64_t)buffer + prog_header->offset),
             prog_header->file_size);
    }

    prog_header =
      (elf_prog_header_t *)((uint16_t *)prog_header + header->prog_head_size);
  }

  proc->regs.rip = header->entry;

  if (auto_enqueue)
    sched_enqueue_proc(proc);

  return 0;
}
