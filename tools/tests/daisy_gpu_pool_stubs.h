#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x, y)
#define APPLE_KEXT_OVERRIDE override
#define OSTypeAlloc(t) new t{}
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
#define IOLog(...) ((void)0)
#define PAGE_SIZE 4096u
#define PAGE_MASK (PAGE_SIZE - 1)
#define GPU_BUFFER_MAX (256ull << 20)
#define GPU_ADDRESS_LIMIT 0x100000000ull
#define MIN(a,b) std::min(a,b)
#define MAX(a,b) std::max(a,b)
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using vm_size_t = uint64_t;
constexpr auto kernel_task = nullptr;
constexpr unsigned kIODirectionInOut = 3, kIOMemoryKernelUserShared = 4,
    kIOMemoryPhysicallyContiguous = 8, kIOMemoryMapperNone = 16,
    kIOMapWriteCombineCache = 0x400, kIOMemoryTypePhysical64 = 32, kIOMemoryThreadSafe = 64,
    kIOMapCacheShift = 8, kIOMemoryBufferCacheShift = 28;
using IOLock = std::mutex;
static IOLock *IOLockAlloc() { return new IOLock; }
static void IOLockLock(IOLock *p) { p->lock(); }
static void IOLockUnlock(IOLock *p) { p->unlock(); }
static int poolMB;
static void PE_parse_boot_argn(const char *key, void *out, size_t n) {
    assert(strcmp(key, "daisygpu_pool") == 0 && n == sizeof(poolMB)); memcpy(out, &poolMB, n);
}
static void gpuToMemory(void *, uint64_t) {}
struct IOAddressRange { uint64_t address, length; };
class OSObject {
    unsigned refs = 1;
public:
    virtual ~OSObject() = default;
    virtual void free() {}
    void retain() { ++refs; }
    void release() { assert(refs); if (!--refs) { free(); delete this; } }
};
class IOMemoryMap;
class IOMemoryDescriptor : public OSObject {
public:
    uint64_t pa = 0, size = 0;
    unsigned cache = 0;
    uint64_t getPhysicalSegment(uint64_t off, IOByteCount *length, unsigned options) {
        assert(options == kIOMemoryMapperNone && off < size); *length = size - off; return pa + off;
    }
    IOMemoryMap *map();
};
class IOMemoryMap : public OSObject {
public:
    IOMemoryDescriptor *md;
    explicit IOMemoryMap(IOMemoryDescriptor *p) : md(p) { p->retain(); }
    void free() override { md->release(); }
};
IOMemoryMap *IOMemoryDescriptor::map() { return new IOMemoryMap(this); }
static unsigned physicalCalls, failPhysicalAt;
static uint64_t nextPA = 0xa4000000;
static bool failDescriptor;
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
    void *data = nullptr;
public:
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(decltype(nullptr), unsigned options,
                                                          uint64_t size, uint64_t mask) {
        assert((options & kIOMapWriteCombineCache) && mask == 0xfffff000ull);
        if (++physicalCalls == failPhysicalAt) return nullptr;
        auto p = new IOBufferMemoryDescriptor;
        assert(posix_memalign(&p->data, PAGE_SIZE, size) == 0);
        p->size = size; p->pa = nextPA; nextPA += size; return p;
    }
    void *getBytesNoCopy() { return data; }
    void free() override { std::free(data); }
};
class IOGeneralMemoryDescriptor : public IOMemoryDescriptor {
public:
    bool initWithOptions(IOAddressRange *range, unsigned count, unsigned offset,
                         decltype(nullptr), unsigned flags, decltype(nullptr)) {
        assert(count == 1 && offset == 0 && (flags & kIOMemoryTypePhysical64));
        assert(flags & kIOMemoryMapperNone);
        cache = (flags >> kIOMemoryBufferCacheShift) << kIOMapCacheShift;
        assert(cache == kIOMapWriteCombineCache);
        pa = range->address; size = range->length; return !failDescriptor;
    }
};
