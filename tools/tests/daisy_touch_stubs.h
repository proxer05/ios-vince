/* Host-only declarations; the actual kernel source is inserted after this. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int thread_t;
typedef int wait_result_t;
#define KERN_SUCCESS 0
#define THREAD_CONTINUE_NULL NULL
#define NSEC_PER_MSEC 1000000u
#define NSEC_PER_USEC 1000u
static uint64_t mock_time_ns;
static unsigned mock_tick_ns, mock_delay_factor, mock_delay_calls;
static uint64_t mach_absolute_time(void) { uint64_t t = mock_time_ns; mock_time_ns += mock_tick_ns; return t; }
static void absolutetime_to_nanoseconds(uint64_t time, uint64_t *ns) { *ns = time; }
static void clock_interval_to_absolutetime_interval(uint32_t interval, uint32_t scale, uint64_t *ticks)
{ *ticks = (uint64_t)interval * scale; }
static uint32_t mock_rd(vm_offset_t, uint32_t);
static void mock_wr(vm_offset_t, uint32_t, uint32_t);
static void mock_sleep(unsigned);
static void delay(unsigned us) { mock_delay_calls++; mock_time_ns += (uint64_t)us * 1000 * mock_delay_factor; }
static thread_t current_thread(void) { return 1; }
static void thread_block(void *cont) { (void)cont; }
static void thread_deallocate(thread_t t) { (void)t; }
static int kernel_thread_start(void (*fn)(void *, wait_result_t), void *, thread_t *);
static bool PE_parse_boot_argn(const char *, void *, int);
static void test_gpio_reports(void);
static void test_gpio_hid(void);
