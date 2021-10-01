#include <boot/stivale2.h>
#include <cpu_locals.h>
#include <drivers/apic.h>
#include <drivers/pcspkr.h>
#include <drivers/pit.h>
#include <drivers/serial.h>
#include <lock.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <printf.h>
#include <registers.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/gdt.h>
#include <sys/irq.h>
#include <sys/types.h>
#include <tasking/scheduler.h>
#include <vec.h>

#define ALIVE 0
#define DEAD 1

static volatile int sched_started = 0;

size_t current_tid = 0;
size_t current_pid = 0;

static vec_t(thread_t *) threads = {};
static vec_t(proc_t *) processes = {};

static volatile lock_t sched_lock = {0};

extern void switch_and_run_stack(uintptr_t stack);

void k_idle() {
  while (1)
    ;
}

void enqueue_thread(thread_t *thread) {
  LOCK(sched_lock);

  if (thread->enqueued) {
    UNLOCK(sched_lock);
    return;
  }

  vec_push(&threads, thread);
  vec_push(&thread->mother_proc->threads, thread);
  thread->mother_proc->thread_count++;
  thread->enqueued = 1;

  UNLOCK(sched_lock);
}

thread_t *create_thread_template(char *name, uintptr_t addr, size_t time_slice,
                                 uint64_t code_segment, uint64_t data_segment,
                                 proc_t *mother_proc, int auto_enqueue) {
  thread_t *new_thread = kmalloc(sizeof(thread_t));

  *new_thread = (thread_t){
      .tid = current_tid++,
      .exit_state = -1,
      .state = ALIVE,
      .name = name,
      .mother_proc = mother_proc,
      .enqueued = 0,
      .registers =
          (registers_t){
              .cs = code_segment,
              .ss = data_segment,
              .rip = (uint64_t)addr,
              .rflags = 0x202,
              .rsp = (uint64_t)pcalloc(1) + PAGE_SIZE + PHYS_MEM_OFFSET,
          },
      .time_slice = (time_slice == 0) ? 5000 : time_slice,
  };

  if (auto_enqueue)
    enqueue_thread(new_thread);

  return new_thread;
}

thread_t *create_kernel_thread(char *name, uintptr_t addr, size_t time_slice,
                               proc_t *mother_proc) {
  return create_thread_template(name, addr, time_slice, GDT_SEG_KCODE,
                                GDT_SEG_KDATA, mother_proc, 1);
}

void enqueue_proc(proc_t *proc) {
  LOCK(sched_lock);

  if (proc->enqueued) {
    UNLOCK(sched_lock);
    return;
  }

  vec_push(&processes, proc);
  proc->enqueued = 1;

  UNLOCK(sched_lock);
}

proc_t *create_proc(char *name) {
  proc_t *new_proc = kmalloc(sizeof(proc_t));

  *new_proc = (proc_t){
      .name = name,
      .thread_count = 0,
      .pid = current_pid++,
      .pagemap = (uint64_t)get_kernel_pagemap(),
      .enqueued = 0,
  };

  new_proc->threads.data = kmalloc(sizeof(thread_t *));

  enqueue_proc(new_proc);

  return new_proc;
}

int dequeue_thread(thread_t *thread) {
  if (!thread->enqueued)
    return 0;

  LOCK(sched_lock);

  while (1)
    if (LOCK_ACQUIRE(thread->lock)) {
      thread->enqueued = 0;
      vec_remove(&threads, thread);
      UNLOCK(sched_lock);
      return 1;
    }

  UNLOCK(sched_lock);

  return 0;
}

void destroy_thread(thread_t *thread) {
  if (thread->enqueued)
    dequeue_thread(thread);

  kfree(thread);
}

int dequeue_proc(proc_t *proc) {
  if (!proc->enqueued)
    return 0;

  LOCK(sched_lock);

  for (size_t i = 0; i < proc->thread_count; i++)
    dequeue_thread(proc->threads.data[i]);

  vec_remove(&processes, proc);

  UNLOCK(sched_lock);

  return 1;
}

void destroy_proc(proc_t *proc) {
  if (proc->enqueued)
    dequeue_proc(proc);

  for (size_t i = 0; i < proc->thread_count; i++)
    destroy_thread(proc->threads.data[i]);
}

size_t get_next_thread(size_t orig_i) {
  size_t index = orig_i + 1;

  while (1) {
    if (index >= (size_t)threads.length)
      index = 0;

    thread_t *thread = LOCKED_READ(threads.data[index]);

    if (thread && LOCK_ACQUIRE(thread->lock))
      return index;

    if (index == orig_i)
      break;

    index++;
  }

  return -1;
}

void schedule(uint64_t rsp) {
  lapic_timer_stop();

  cpu_locals_t *locals = get_locals();
  thread_t *current_thread = locals->current_thread;

  if (!LOCK_ACQUIRE(sched_lock)) {
    size_t slice = current_thread ? current_thread->time_slice : 20000;
    lapic_eoi();
    lapic_timer_oneshot(SCHEDULE_REG, slice);
    switch_and_run_stack((uintptr_t)rsp);
  }

  size_t new_index = get_next_thread(locals->last_run_thread_index);

  if (current_thread) {
    current_thread->registers = *((registers_t *)rsp);
    UNLOCK(current_thread->lock);
  }

  if (new_index == (size_t)-1) {
    printf("Bump");
    locals->last_run_thread_index = 0;
    locals->current_thread = NULL;
    UNLOCK(sched_lock);
    lapic_eoi();
    await();
  }

  current_thread = LOCKED_READ(threads.data[new_index]);
  locals->current_thread = current_thread;
  locals->last_run_thread_index = new_index;

  asm volatile("mov %0, %%cr3" : : "r"(current_thread->mother_proc->pagemap));

  lapic_eoi();
  lapic_timer_oneshot(SCHEDULE_REG, current_thread->time_slice);

  UNLOCK(sched_lock);

  switch_and_run_stack((uintptr_t)&current_thread->registers);
}

void await() {
  asm volatile("cli");

  lapic_timer_oneshot(SCHEDULE_REG, 20000);

  asm volatile("sti\n"
               "1:\n"
               "hlt\n"
               "jmp 1b\n");
}

void await_sched_start() {
  while (!LOCKED_READ(sched_started))
    ;

  await();
}

void scheduler_init(uintptr_t addr, struct stivale2_struct_tag_smp *smp_info) {
  processes.data = kcalloc(sizeof(proc_t *));
  threads.data = kcalloc(sizeof(thread_t *));

  create_proc("k_proc");

  create_kernel_thread("k_init", (uint64_t)addr, 5000, processes.data[0]);

  for (size_t i = 0; i < smp_info->cpu_count; i++)
    create_kernel_thread("k_idle", (uintptr_t)k_idle, 5000, processes.data[0]);

  LOCKED_WRITE(sched_started, 1);

  await();
}
