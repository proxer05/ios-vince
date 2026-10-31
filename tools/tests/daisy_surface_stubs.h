/* Functional IOKit stand-ins for the actual SweetSurface.cpp regression test. */
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x, y)
#define APPLE_KEXT_OVERRIDE
#define __unused __attribute__((unused))
#define OSDynamicCast(t, p) dynamic_cast<t *>(p)
#define OSTypeAlloc(t) new t{}
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
#define IOMallocData(n) malloc(n)
#define IOMallocZeroData(n) calloc(1, n)
#define IOFreeData(p, n) std::free(p)
#define IOLog(...) ((void)0)
#define PAGE_MASK 4095ULL
#define trunc_page_64(n) ((n) & ~PAGE_MASK)
#define round_page_64(n) (((n) + PAGE_MASK) & ~PAGE_MASK)
#define round_page(n) round_page_64(n)
using task_t = void *;
using mach_port_name_t = uint32_t;
using user_addr_t = uintptr_t;
using IOOptionBits = uint32_t;
using UInt32 = uint32_t;
using SInt32 = int32_t;
using SInt64 = int64_t;
using IOByteCount = uint64_t;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnNoMemory = 2,
    kIOReturnVMError = 3, kIOReturnNoResources = 4, kIOReturnNotFound = 5,
    kIOReturnNoSpace = 6, kIOReturnUnsupported = 7, kIOReturnBusy = 8;
constexpr unsigned kIODirectionInOut = 0, kIOMemoryKernelUserShared = 0,
    kIOMapDefaultCache = 0, kIOMapWriteCombineCache = 1, kIOMapAnywhere = 2, kIOMapReadOnly = 4;
constexpr unsigned page_size = 4096;
constexpr task_t kernel_task = nullptr;
constexpr const char *kIOUserClientClassKey = "IOUserClientClass";
using IOLock = int;
static IOLock *IOLockAlloc() { return new int(0); }
static void IOLockFree(IOLock *p) { delete p; }
static void IOLockLock(IOLock *) {}
static void IOLockUnlock(IOLock *) {}
static bool OSCompareAndSwapPtr(void *old, IOLock *p, IOLock **at)
{ if (*at != old) return false; *at = p; return true; }
static int64_t OSAddAtomic64(int64_t n, volatile int64_t *p) { auto old = *p; *p += n; return old; }
static int32_t OSIncrementAtomic(volatile int32_t *p) { return (*p)++; }
static void proc_selfname(char *p, size_t n) { snprintf(p, n, "backboardd"); }
static int copyout(const void *p, user_addr_t to, size_t n) { memcpy((void *)to, p, n); return 0; }
static bool PE_parse_boot_argn(const char *, void *, size_t) { return false; }

class OSSerialize;
class OSObject {
    unsigned refs = 1;
public:
    virtual ~OSObject() = default;
    virtual bool init() { return true; }
    virtual void free() {}
    void retain() { ++refs; }
    void release() { assert(refs); if (!--refs) { free(); delete this; } }
    virtual bool serialize(OSSerialize *) { return true; }
};
class OSSerialize : public OSObject {
public:
    static OSSerialize *binaryWithCapacity(unsigned) { return new OSSerialize; }
    uint32_t getLength() { return 4; }
    const char *text() { return "bin"; }
};
class OSNumber : public OSObject {
    uint64_t n;
public:
    explicit OSNumber(uint64_t v) : n(v) {}
    uint32_t unsigned32BitValue() { return n; }
    uint64_t unsigned64BitValue() { return n; }
};
class OSString : public OSObject {
public:
    std::string str;
    explicit OSString(const char *s) : str(s) {}
};
class OSArray : public OSObject {
    std::vector<OSObject *> items;
public:
    static OSArray *withCapacity(unsigned) { return new OSArray; }
    unsigned getCount() { return items.size(); }
    OSObject *getObject(unsigned i) { return i < items.size() ? items[i] : nullptr; }
    bool setObject(OSObject *p) { p->retain(); items.push_back(p); return true; }
    void removeObject(unsigned i) { auto p = items.at(i); items.erase(items.begin() + i); p->release(); }
    void flushCollection() { while (!items.empty()) removeObject(0); }
    void free() override { flushCollection(); }
};
class OSDictionary : public OSObject {
    std::map<std::string, OSObject *> items;
public:
    static OSDictionary *withCapacity(unsigned) { return new OSDictionary; }
    unsigned getCount() { return items.size(); }
    OSObject *getObject(const char *key) { auto i = items.find(key); return i == items.end() ? nullptr : i->second; }
    bool setObject(const char *key, OSObject *p) {
        if (!p) return false;
        p->retain(); removeObject(key); items[key] = p; return true;
    }
    bool setObject(OSString *key, OSObject *p) { return setObject(key->str.c_str(), p); }
    bool merge(OSDictionary *other) { for (auto &i : other->items) setObject(i.first.c_str(), i.second); return true; }
    void removeObject(const char *key) { auto i = items.find(key); if (i != items.end()) { i->second->release(); items.erase(i); } }
    void flushCollection() { for (auto &i : items) i.second->release(); items.clear(); }
    void free() override { flushCollection(); }
};
static OSObject *decoded;
static OSObject *OSUnserializeXML(const char *, uint32_t) { if (decoded) decoded->retain(); return decoded; }

static bool failDMA, failOrdinary, failMapping, failPrepare;
static unsigned preparations, completions, dmaAttempts;
static uintptr_t clientBase;
static uint64_t clientSize;
class IOMemoryMap;
class IOMemoryDescriptor : public OSObject {
public:
    uintptr_t address = 0;
    uint64_t length = 0;
    bool caller = false;
    unsigned prepared = 0;
    static IOMemoryDescriptor *withAddressRange(uint64_t a, uint64_t n, unsigned, task_t) {
        auto p = new IOMemoryDescriptor; p->address = a; p->length = n; p->caller = true; return p;
    }
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t a, uint64_t n, unsigned) {
        auto p = new IOMemoryDescriptor; p->address = a; p->length = n; return p;
    }
    IOReturn prepare() {
        if (failPrepare || (caller && (address < clientBase || address - clientBase > clientSize ||
            length > clientSize - (address - clientBase)))) return kIOReturnVMError;
        ++prepared; ++preparations; return 0;
    }
    void complete() { assert(prepared); --prepared; ++completions; }
    void free() override { assert(prepared == 0); }
    uint64_t getLength() { return length; }
    IOByteCount writeBytes(uint64_t off, const void *p, uint64_t n) {
        assert(prepared && off + n <= length); memcpy((void *)(address + off), p, n); return n;
    }
    IOMemoryMap *map(unsigned);
    IOMemoryMap *createMappingInTask(task_t, uint64_t, unsigned);
};
class IOMemoryMap : public OSObject {
    IOMemoryDescriptor *md;
public:
    explicit IOMemoryMap(IOMemoryDescriptor *p) : md(p) { md->retain(); }
    uint64_t getAddress() { return md->address; }
    uint64_t getVirtualAddress() { return md->address; }
    void free() override { md->release(); }
};
IOMemoryMap *IOMemoryDescriptor::map(unsigned) { return failMapping ? nullptr : new IOMemoryMap(this); }
IOMemoryMap *IOMemoryDescriptor::createMappingInTask(task_t, uint64_t, unsigned) { return map(0); }
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    static IOBufferMemoryDescriptor *inTaskWithOptions(task_t, unsigned, uint64_t n, unsigned) {
        if (failOrdinary) return nullptr;
        auto p = new IOBufferMemoryDescriptor;
        void *mem = nullptr; assert(posix_memalign(&mem, page_size, n) == 0);
        p->address = (uintptr_t)mem; p->length = n; return p;
    }
    void *getBytesNoCopy() { return (void *)address; }
    void free() override { IOMemoryDescriptor::free(); std::free((void *)address); }
};

struct IOExternalMethodArguments {
    uint64_t *scalarInput = nullptr, *scalarOutput = nullptr;
    uint32_t scalarInputCount = 0, scalarOutputCount = 0;
    const void *structureInput = nullptr;
    void *structureOutput = nullptr;
    uint32_t structureInputSize = 0, structureOutputSize = 0, structureOutputDescriptorSize = 0;
    IOMemoryDescriptor *structureInputDescriptor = nullptr, *structureOutputDescriptor = nullptr;
};
struct IOExternalMethodDispatch {};
class IOService;
struct IOExternalTrap;
class IOService : public OSObject {
public:
    virtual bool start(IOService *) { return true; }
    void setProperty(const char *, const char *) {}
    void registerService() {}
    void terminate() {}
};
class IOUserClient : public IOService {
public:
    bool initWithTask(task_t, void *, UInt32) { return true; }
    static void copyPortNameForObjectInTask(task_t, OSObject *, mach_port_name_t *name) { *name = 1; }
    static IOReturn copyObjectForPortNameInTask(task_t, mach_port_name_t, OSObject **) { return kIOReturnNotFound; }
};
using IOTrap = IOReturn (IOUserClient::*)(void *, void *, void *, void *, void *, void *);
struct IOExternalTrap { IOService *object; IOTrap func; };
