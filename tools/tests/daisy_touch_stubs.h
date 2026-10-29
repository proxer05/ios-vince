/* Host-only declarations; the actual kernel source is inserted after this. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int thread_t;
typedef int wait_result_t;
#define KERN_SUCCESS 0
#define THREAD_CONTINUE_NULL NULL
static uint32_t mock_rd(vm_offset_t, uint32_t);
static void mock_wr(vm_offset_t, uint32_t, uint32_t);
static void mock_sleep(unsigned);
static void delay(unsigned us) { (void)us; }
static thread_t current_thread(void) { return 1; }
static void thread_block(void *cont) { (void)cont; }
static void thread_deallocate(thread_t t) { (void)t; }
static int kernel_thread_start(void (*fn)(void *, wait_result_t), void *, thread_t *);
static bool PE_parse_boot_argn(const char *, void *, int);
