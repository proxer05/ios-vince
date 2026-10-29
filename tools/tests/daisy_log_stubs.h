/* Minimal VFS/thread/console model; every I/O asserts it is outside console locks. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#define __unused __attribute__((unused))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define FALSE false
#define FREAD 1
#define FWRITE 2
#define UIO_READ 0
#define UIO_WRITE 1
#define UIO_SYSSPACE 1
#define IO_NODELOCKED 1
#define IO_SYNC 2
#define MNT_WAIT 1
#define NULLVP NULL
#define KERN_SUCCESS 0
typedef bool boolean_t;
typedef int wait_result_t;
typedef int kern_return_t;
typedef void *thread_t;
typedef void *vfs_context_t;
typedef struct mock_file *vnode_t;
struct mock_file { bool regular; off_t size; unsigned char data[32 * 1024 * 1024]; };
struct vnode_attr { off_t va_data_size; unsigned va_mode; };
#define VATTR_INIT(va) memset((va), 0, sizeof(*(va)))
#define VATTR_SET(va, field, value) ((va)->field = (value))
static struct mock_file file;
static bool console_ready, console_suspended, irq_enabled;
static int kernel_debugger_entry_count;
static struct { int flush_lock; } console_ring;
static char vince_conlog[1 << 23];
static volatile uint64_t vince_conlog_pos;
static unsigned locks, opens, closes, reads, writes, syncs, truncates, sleeps, starts, terminates;
static int open_error, read_error, write_error, sync_error, attr_error, thread_error;
static bool short_write;
static int arg_enabled, arg_cpus, arg_rw;
unsigned int real_ncpus;

static bool is_console_initialized(void) { return console_ready; }
static void lck_mtx_lock(int *lock) { assert(irq_enabled && !*lock); *lock = 1; locks++; }
static void lck_mtx_unlock(int *lock) { assert(irq_enabled && *lock); *lock = 0; }
static bool ml_set_interrupts_enabled(bool value) { bool old = irq_enabled; irq_enabled = value; return old; }
static void io_context(void) { assert(irq_enabled && !console_ring.flush_lock); }
static vfs_context_t vfs_context_kernel(void) { return &file; }
static void *vfs_context_ucred(vfs_context_t ctx) { assert(ctx == &file); return ctx; }
static void *vfs_context_proc(vfs_context_t ctx) { assert(ctx == &file); return ctx; }
static int vnode_open(const char *path, int flags, int mode, int x, vnode_t *out, vfs_context_t ctx)
{
	io_context(); opens++;
	assert(!strcmp(path, "/private/var/log/daisy-console.log"));
	assert(flags == (FREAD | FWRITE | O_CREAT | O_NOFOLLOW) && mode == 0600 && !x && ctx == &file);
	if (open_error) { return open_error; }
	*out = &file; return 0;
}
static int vnode_isreg(vnode_t vp) { assert(vp == &file); return file.regular; }
static int vnode_size(vnode_t vp, off_t *size, vfs_context_t ctx)
{
	assert(vp == &file && ctx == &file); *size = file.size; return 0;
}
static int vnode_setattr(vnode_t vp, struct vnode_attr *va, vfs_context_t ctx)
{
	io_context(); assert(vp == &file && ctx == &file && !va->va_data_size && va->va_mode == 0600);
	if (attr_error) { return attr_error; }
	truncates++; file.size = 0; return 0;
}
static int vn_rdwr(int op, vnode_t vp, char *buf, int n, off_t off, int space, int flags,
    void *cred, int *resid, void *proc)
{
	io_context();
	assert(vp == &file && n > 0 && off >= 0 && (uint64_t)off + n <= sizeof(file.data));
	assert(space == UIO_SYSSPACE && cred == &file && proc == &file);
	*resid = 0;
	if (op == UIO_READ) {
		assert(flags == IO_NODELOCKED); reads++;
		if (read_error) { return read_error; }
		int count = (int)MIN(n, file.size - off);
		memcpy(buf, file.data + off, count); *resid = n - count;
	} else {
		assert(op == UIO_WRITE && flags == (IO_NODELOCKED | IO_SYNC)); writes++;
		if (write_error) { return write_error; }
		int count = n - (short_write ? 1 : 0);
		memcpy(file.data + off, buf, count); *resid = n - count;
		if (off + count > file.size) { file.size = off + count; }
	}
	return 0;
}
static int VNOP_FSYNC(vnode_t vp, int wait, vfs_context_t ctx)
{
	io_context(); assert(vp == &file && ctx == &file && wait == MNT_WAIT); syncs++; return sync_error;
}
static int vnode_close(vnode_t vp, int flags, vfs_context_t ctx)
{
	io_context(); assert(vp == &file && ctx == &file && flags == (FREAD | FWRITE)); closes++; return 0;
}
static bool PE_parse_boot_argn(const char *name, void *out, int n)
{
	assert(n == sizeof(int));
	if (!strcmp(name, "daisylog")) { *(int *)out = arg_enabled; }
	else if (!strcmp(name, "cpus")) { *(int *)out = arg_cpus; }
	else { assert(!strcmp(name, "mmcrw")); *(int *)out = arg_rw; }
	return true;
}
static kern_return_t kernel_thread_start(void (*fn)(void *, wait_result_t), void *arg, thread_t *thread)
{
	assert(fn && !arg); starts++; *thread = &file; return thread_error;
}
static void thread_deallocate(thread_t thread) { assert(thread == &file); }
static thread_t current_thread(void) { return &file; }
static kern_return_t thread_terminate(thread_t thread) { assert(thread == &file); terminates++; return KERN_SUCCESS; }
static void mock_sleep(unsigned);
