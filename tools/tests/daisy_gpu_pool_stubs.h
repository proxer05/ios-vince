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
#define PAGE_MIN_SIZE 4096u
static unsigned vm_page_size = PAGE_MIN_SIZE;
#define PAGE_SIZE vm_page_size
#define PAGE_MASK (PAGE_SIZE - 1)
#define GPU_BUFFER_MAX (256ull << 20)
#define GPU_ADDRESS_LIMIT 0x100000000ull
#define MIN(a,b) std::min(a,b)
#define MAX(a,b) std::max(a,b)
using IOByteCount = uint64_t;
using IOOptionBits = uint32_t;
using vm_size_t = uint64_t;
using mach_vm_address_t = uint64_t;
constexpr auto kernel_task = nullptr;
constexpr unsigned kIODirectionInOut = 3, kIOMemoryKernelUserShared = 4,
    kIOMemoryPhysicallyContiguous = 8, kIOMemoryMapperNone = 16,
    kIOMapWriteCombineCache = 0x400, kIOMemoryTypeVirtual64 = 32, kIOMemoryThreadSafe = 64,
    kIOMemoryAutoPrepare = 128, kIOMemoryPersistent = 256,
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
static bool failDescriptor, failSpan;
static unsigned preparations, completions;
struct Allocation { uintptr_t base; uint64_t size, pa; };
static std::vector<Allocation> allocations;
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
    void *data = nullptr;
public:
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(decltype(nullptr), unsigned options,
                                                          uint64_t size, uint64_t mask) {
        assert((options & kIOMapWriteCombineCache) && mask == 0xfffff000ull);
        if (++physicalCalls == failPhysicalAt) return nullptr;
        auto p = new IOBufferMemoryDescriptor;
        assert(posix_memalign(&p->data, PAGE_SIZE, size) == 0);
        nextPA = (nextPA + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
        p->size = size; p->pa = nextPA; nextPA += size;
        allocations.push_back({(uintptr_t)p->data, size, p->pa});
        return p;
    }
    void *getBytesNoCopy() { return data; }
    void free() override {
        allocations.erase(std::remove_if(allocations.begin(), allocations.end(),
            [this](const Allocation &a) { return a.base == (uintptr_t)data; }), allocations.end());
        std::free(data);
    }
};
class IOGeneralMemoryDescriptor : public IOMemoryDescriptor {
    bool prepared = false;
public:
    bool initWithOptions(IOAddressRange *range, unsigned count, unsigned offset,
                         decltype(nullptr), unsigned flags, decltype(nullptr)) {
        assert(count == 1 && offset == 0 && (flags & kIOMemoryTypeVirtual64));
        assert(flags & kIOMemoryMapperNone);
        assert((flags & kIOMemoryAutoPrepare) && (flags & kIOMemoryPersistent));
        cache = (flags >> kIOMemoryBufferCacheShift) << kIOMapCacheShift;
        assert(cache == kIOMapWriteCombineCache);
        if (failDescriptor) return false;
        for (const auto &a : allocations) {
            if (range->address >= a.base && range->address - a.base < a.size) {
                auto off = range->address - a.base;
                assert(range->length <= a.size - off);
                pa = a.pa + off + (failSpan ? PAGE_SIZE : 0); size = range->length;
                prepared = true; ++preparations; return true;
            }
        }
        assert(false); return false;
    }
    void free() override { if (prepared) { ++completions; prepared = false; } }
};
