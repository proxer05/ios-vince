/*
 * libdaisy-inject.dylib: inserted into backboardd (DYLD_INSERT_LIBRARIES
 * in its launchd job, or via bbd_wrap). When the kernel has published the
 * GPU service (DaisyGPU, VinceGPU, or RedwoodGPU; boot-arg gpu=1), it loads
 * the OpenGL ES library that stands behind EAGLContext, so CoreAnimation's
 * window server draws with the Adreno instead of its software renderer.
 * gl=0 in the boot-args: keep software renderer.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <execinfo.h>
#include <sys/ucontext.h>
#include <objc/runtime.h>
#include <mach/mach.h>
#include <stdint.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define DAISY_GL "/daisy/lib/libdaisy-gl.dylib"
#define TOPAZ_GL "/topaz/lib/libtopaz-gl.dylib"

/*
 * CoreAnimation's OpenGL ES renderer hands a "function"'s numbers (a colour matrix, ...) to
 * GLContext::function_uniform(); for the functions it has no shader for they go nowhere, and our
 * library draws those with programs of its own (eagl.c) once told them. Without that the lock
 * screen's vibrantColorMatrix layer is a red veil. (Scheme from darwin-redwood inject.c; addresses
 * are this iOS 16.7.16 cache's.)
 */
#define CACHE_BASE              0x180000000UL
#define VT_GLES_CONTEXT         0x1d5b2f6e8UL   /* vtable for CA::OGL::GLESContext */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))
#define FN_FUNCTION_SUPPORTED   0x1884b2164UL   /* CA::OGL::GLContext::function_supported() const */
#define FN_FUNCTION_UNIFORM     0x1884b21acUL   /* CA::OGL::GLContext::function_uniform(unsigned, unsigned long, unsigned long, float const*) */

static uintptr_t slide;

static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	void *original;

	if (*entry != expected + slide) {
		fprintf(stderr, "daisy-gl: %s: slot holds %#lx, expected %#lx: left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		fprintf(stderr, "daisy-gl: %s: table not writable\n", what);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

static int (*orig_function_supported)(void *context);
static void (*gl_function_refused)(unsigned function);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static unsigned
context_function(void *context)
{
	return *(*(uint8_t **)((char *)context + 0x10) + 0x10);
}

static int
my_function_supported(void *context)
{
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	unsigned function = context_function(context);

	if (!supported) {
		if (gl_function_refused != NULL) {
			gl_function_refused(function);
		}
		if (!said[function & 255]) {
			said[function & 255] = 1;
			fprintf(stderr, "daisy-gl: CoreAnimation's GL renderer has no function %u\n", function);
		}
	}
	return supported;
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	gl_function_values(context, context_function(context), index, components, count, values);
	orig_function_uniform(context, index, components, count, values);
}

static void
gl_hooks(void *lib)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (cache == NULL || lib == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	gl_function_refused = (void (*)(unsigned))dlsym(lib, "redwood_gl_function_refused");
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(lib,
	    "redwood_gl_function_values");
	orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), FN_FUNCTION_SUPPORTED,
	    my_function_supported);
	if (gl_function_values != NULL) {
		orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), FN_FUNCTION_UNIFORM,
	    my_function_uniform);
	}
	fprintf(stderr, "daisy-gl: GLESContext hooks: function_supported %s, function_uniform %s\n",
	    orig_function_supported ? "on" : "OFF", orig_function_uniform ? "on" : "OFF");
}

/* Where a crash happened, printed to stderr (the console): image + offset + symbol. */
static void
daisy_where(const char *what, void *addr)
{
	Dl_info info;

	if (dladdr(addr, &info) && info.dli_fname != NULL) {
		fprintf(stderr, "daisy-crash: %s %p = %s+0x%lx (%s)\n", what, addr, info.dli_fname,
		    (unsigned long)((char *)addr - (char *)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
	} else {
		fprintf(stderr, "daisy-crash: %s %p (no image)\n", what, addr);
	}
}

static void
daisy_crash(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = (ucontext_t *)uc_;
	void *frames[16];
	int n;

	fprintf(stderr, "daisy-crash: signal %d at address %p\n", sig, si ? si->si_addr : NULL);
	if (uc != NULL && uc->uc_mcontext != NULL) {
		daisy_where("pc", (void *)uc->uc_mcontext->__ss.__pc);
		daisy_where("lr", (void *)uc->uc_mcontext->__ss.__lr);
	}
	n = backtrace(frames, 16);
	for (int i = 0; i < n; i++) {
		daisy_where("bt", frames[i]);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
daisy_crash_handlers(void)
{
	struct sigaction sa;
	int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGTRAP, SIGFPE };

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = daisy_crash;
	sa.sa_flags = SA_SIGINFO;
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
		sigaction(sigs[i], &sa, NULL);
	}
}

/*
 * containermanagerd (both): they ask the user manager for the user's personas, of which this
 * system has none, again and again before giving up, and everything that needs a container
 * waits with them -- extensions (posters, widgets) are not registered at all. Told personas are
 * not supported, they do not ask. (darwin-redwood inject.c; address of this 16.7.16 cache.)
 */
#define FN_PERSONAS_ARE_SUPPORTED 0x1c4c699f8UL /* +[MCMUserIdentityCache personasAreSupported] */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static void
container_manager_fixes(void)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	Class cls = objc_getClass("MCMUserIdentityCache");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (cache == NULL || original == NULL ||
	    (uintptr_t)original != FN_PERSONAS_ARE_SUPPORTED + ((uintptr_t)cache - CACHE_BASE)) {
		fprintf(stderr, "daisy-cm: +[MCMUserIdentityCache personasAreSupported] not as expected: left alone\n");
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	fprintf(stderr, "daisy-cm: container manager: no personas\n");
	FILE *f = fopen("/var/mobile/daisy-logs/inject.log", "a");
	if (f == NULL) {
		f = fopen("/var/mobile/topaz-logs/inject.log", "a");
	}
	if (f != NULL) {
		fprintf(f, "%s[%d]: no personas\n", getprogname(), getpid());
		fclose(f);
	}
}

static io_service_t
find_gpu_service(void)
{
	const char *services[] = { "DaisyGPU", "VinceGPU", "RedwoodGPU" };
	for (size_t s = 0; s < sizeof(services) / sizeof(services[0]); s++) {
		io_service_t gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(services[s]));
		if (gpu != 0) {
			return gpu;
		}
	}
	return 0;
}

__attribute__((constructor))
static void
daisy_inject(void)
{
	const char *name = getprogname();
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *gl_path = NULL;
	io_service_t gpu = 0;
	void *lib;

	if (name != NULL && strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
		return;
	}

	if (access(DAISY_GL, R_OK) == 0) {
		gl_path = DAISY_GL;
	} else if (access(TOPAZ_GL, R_OK) == 0) {
		gl_path = TOPAZ_GL;
	}

	if (name == NULL || strcmp(name, "backboardd") != 0 || gl_path == NULL) {
		return;
	}
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, " gl=0") != NULL) {
		syslog(LOG_NOTICE, "daisy: gl=0: CoreAnimation keeps its software renderer");
		return;
	}
	/* the kernel starts the GPU once the root filesystem is there: give it a moment */
	for (int i = 0; i < 50; i++) {
		gpu = find_gpu_service();
		if (gpu != 0) {
			break;
		}
		usleep(100000);
	}
	if (gpu == 0) {
		syslog(LOG_NOTICE, "daisy: no GPU service: software renderer");
		return;
	}
	IOObjectRelease(gpu);
	daisy_crash_handlers();
	lib = dlopen(gl_path, RTLD_NOW);
	gl_hooks(lib);
	fprintf(stderr, "daisy-crash: handlers set, GL (%s) %s\n", gl_path, lib ? "loaded" : dlerror());
	syslog(LOG_NOTICE, "daisy: %s %s", gl_path, lib ? "loaded" : dlerror());
}

