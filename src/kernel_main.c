#include <boot/stivale2.h>
#include <fb/fb.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <kernel/irq.h>
#include <kernel/isr.h>
#include <printf.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <drivers/pit.h>
#include <drivers/kbd.h>
#include <klog.h>

void *stivale2_get_tag(struct stivale2_struct *stivale2_struct, uint64_t id) {
  struct stivale2_tag *current_tag = (void *)stivale2_struct->tags;
  while (1) {
    if (!current_tag) {
      return NULL;
    } else if (current_tag->identifier == id) {
      return current_tag;
    }
    current_tag = (void *)current_tag->next;
  }
}

void kernel_main(struct stivale2_struct *bootloader_info) {
  init_gdt();
  init_idt();
  init_isr();
  init_irq();
  asm volatile("sti");
  struct stivale2_struct_tag_framebuffer *framebuffer_info =
      (struct stivale2_struct_tag_framebuffer *)stivale2_get_tag(
          bootloader_info, STIVALE2_STRUCT_TAG_FRAMEBUFFER_ID);

  if (init_fb(framebuffer_info) == 0) klog(0, "Framebuffer");
  if (init_pit() == 0) klog(0, "PIT");
  if (init_kbd() == 0) klog(0, "Keyboard");

  sleep(1000);

  printf("\n\n\nHello, world!\r\n");

  int cmd = 0;
  while (1) {
    char echo[100] = "";
    printf("$ ");
    getline(echo, 100);
    printf("\r");
    printf("You typed: %s\n", echo);
    printf("\r");
  }
}