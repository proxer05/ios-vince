/* These call the actual externalMethod/lock-result/lifetime code. IOKit alone is mocked. */
bool vince_surface_enabled() { return true; }
IOBufferMemoryDescriptor *vince_surface_allocate(uint32_t n) {
    ++dmaAttempts;
    return failDMA ? nullptr : IOBufferMemoryDescriptor::inTaskWithOptions(nullptr, 0, n, page_size);
}
bool vince_surface_wait(SweetSurface *, uint32_t) { return true; }

static void number(OSDictionary *p, const char *key, uint64_t n) {
    auto value = new OSNumber(n); p->setObject(key, value); value->release();
}
static uint32_t word(const uint8_t *p, unsigned off) { uint32_t n; memcpy(&n, p + off, 4); return n; }
static uint64_t quad(const uint8_t *p, unsigned off) { uint64_t n; memcpy(&n, p + off, 8); return n; }
static IOReturn call(IOSurfaceRootUserClient *c, unsigned selector, IOExternalMethodArguments &a) {
    return c->externalMethod(selector, &a, nullptr, nullptr, nullptr);
}
static IOSurfaceRootUserClient *client() {
    auto c = new IOSurfaceRootUserClient{}; assert(c->initWithTask((task_t)1, nullptr, 0)); return c;
}
static void close(IOSurfaceRootUserClient *c) { c->clientClose(); c->release(); }

int main() {
    auto c = client();
    uint8_t result[SWEET_SURFACE_RESULT_SIZE];
    IOExternalMethodArguments a;
    a.structureOutput = result; a.structureOutputSize = sizeof(result);

    // The failed real boot requested this b3a8 target, with a separate alpha plane.
    auto props = new OSDictionary;
    number(props, "IOSurfaceWidth", 1125); number(props, "IOSurfaceHeight", 2436);
    number(props, "IOSurfacePixelFormat", 'b3a8'); number(props, "IOSurfaceBytesPerElement", 4);
    number(props, "IOSurfaceBytesPerRow", 4544); number(props, "IOSurfaceAllocSize", 13881344);
    auto planes = new OSArray;
    auto plane = new OSDictionary;
    planes->setObject(plane); planes->setObject(plane); plane->release();
    props->setObject("IOSurfacePlaneInfo", planes); planes->release();
    decoded = props; a.structureInput = "dict"; a.structureInputSize = 4;
    failDMA = true;
    assert(call(c, 0, a) == 0);
    uint32_t id = word(result, 0x18);
    assert(id && quad(result, 0) && quad(result, 8));
    assert(word(result, 0x30) == 'BGRA' && word(result, 0x38) == 0);
    assert(word(result, 0x28) == 4544 && word(result, 0x1c) == 11071488);
    assert(sweet_surface_table[id]->mapCache == kIOMapDefaultCache && dmaAttempts == 1);
    assert(sweet_surface_table[id]->allocSize < 13881344); // discarded alpha storage

    // Disabling shared surfaces bypasses DMA entirely, rather than merely retrying it.
    sweet_surface_copy_only = 1;
    assert(call(c, 0, a) == 0 && dmaAttempts == 1);
    sweet_surface_copy_only = 0;

    // Invalid/unknown creation calls must never report a successful zero-state result.
    assert(call(c, 6, a) == kIOReturnBadArgument);
    assert(call(c, 8, a) == kIOReturnUnsupported);

    void *pixels = nullptr;
    assert(posix_memalign(&pixels, page_size, 2 * page_size) == 0);
    clientBase = (uintptr_t)pixels; clientSize = 2 * page_size;
    memset(pixels, 0x73, clientSize);
    uint8_t image[32] = {};
    put64(image, 0, clientBase + 37);
    put32(image, 8, 13); put32(image, 12, 17); put32(image, 16, 'BGRA');
    put32(image, 20, 4); put32(image, 24, 64);
    a.structureInput = image; a.structureInputSize = sizeof(image);
    assert(call(c, 6, a) == 0);
    uint32_t wrapped = word(result, 0x18);
    assert(wrapped && quad(result, 0) == clientBase + 37 && quad(result, 8));
    assert(quad(result, 0x10) == quad(result, 8) + 0x800);
    assert(word(result, 0x1c) == 1088 && word(result, 0x28) == 64);
    auto s = sweet_surface_copy(wrapped);
    assert(s->wrapped && s->wrapDelta == 37 && s->kernelAddress() == clientBase + 37);
    assert(s->mapCache == kIOMapDefaultCache && s->memory->prepared == 1);
    assert(*(uint8_t *)s->kernelAddress() == 0x73);
    assert(quad((uint8_t *)s->state->getBytesNoCopy(), 0) == wrapped);

    // Bulk attachment reads now have a real page; updates preserve untouched fields.
    uint8_t bulk[0x88] = {};
    memset(bulk, 0x46, 0x70); put64(bulk, 0x70, 0x70); put64(bulk, 0x78, 1);
    put32(bulk, 0x80, wrapped);
    a.structureInput = bulk; a.structureInputSize = sizeof(bulk);
    assert(call(c, 27, a) == 0);
    auto state = (uint8_t *)s->state->getBytesNoCopy();
    for (unsigned i = 0; i < 0x70; ++i) assert(state[0x30 + i] == (i < 0x20 ? 0x46 : 0));
    put64(bulk, 0x70, 0x10); // partial field is not copied
    memset(bulk, 0xff, 0x20); assert(call(c, 27, a) == 0 && state[0x30] == 0x46);
    put64(bulk, 0x70, 0x71); assert(call(c, 27, a) == kIOReturnBadArgument);
    put64(bulk, 0x70, 0x70);
    auto stranger = client(); assert(call(stranger, 27, a) == kIOReturnNotFound); close(stranger);

    // The same client's values and matrix state are visible through the shared page.
    auto values = new OSArray;
    auto value = new OSNumber(42); auto key = new OSString("ColorSpace");
    values->setObject(value); values->setObject(key); value->release(); key->release();
    uint8_t request[32] = {}; put32(request, 0, wrapped);
    decoded = values; a.structureInput = request; a.structureInputSize = sizeof(request);
    assert(call(c, 9, a) == 0);
    uint32_t seed = word(result, 0);
    assert((seed & 1) && word(state, 0x20) == seed && word(state, 0x28) > 8);
    memcpy(request + 8, "ColorSpace", 11);
    a.structureOutputSize = sizeof(result);
    assert(call(c, 10, a) == 0 && word(result, 4) == seed && a.structureOutputSize > 8);
    assert(call(c, 11, a) == 0 && word(state, 0x20) != seed && word(state, 0x28) == 0);
    memset(request + 8, 'x', sizeof(request) - 8);
    assert(call(c, 10, a) == kIOReturnBadArgument);
    values->release(); decoded = props;
    a.structureOutputSize = sizeof(result);

    // Lookup uses the cached-client sentinel; it must not invent a second mapping hold.
    uint64_t scalar[] = { wrapped, 0 };
    a.scalarInput = scalar; a.scalarInputCount = 1;
    assert(call(c, 4, a) == 0 && quad(result, 0) == ~0ULL && quad(result, 8));
    assert(c->trapRelease((void *)(uintptr_t)wrapped, nullptr, nullptr, nullptr, nullptr, nullptr) == 0);
    assert(sweet_surface_copy(wrapped) == nullptr);
    assert(s->memory->prepared == 1); // retained kernel/GPU owner still has the pixels
    s->release();
    assert(preparations == completions);

    // Mapping failure unwires the newly wrapped range and removes its table entry.
    a.scalarInputCount = 0; a.structureInput = image; a.structureInputSize = sizeof(image);
    failMapping = true;
    assert(call(c, 6, a) == kIOReturnVMError);
    failMapping = false;
    assert(preparations == completions);
    failPrepare = true; assert(call(c, 6, a) == kIOReturnVMError); failPrepare = false;
    a.structureOutputSize = 16;
    assert(call(c, 6, a) == kIOReturnNoSpace && preparations == completions);
    a.structureOutputSize = sizeof(result);
    put64(image, 0, ~0ULL - 32); assert(call(c, 6, a) == kIOReturnBadArgument);
    put64(image, 0, clientBase); put32(image, 24, 4); assert(call(c, 6, a) == kIOReturnBadArgument);
    put32(image, 24, 64); put32(image, 8, 16385); assert(call(c, 6, a) == kIOReturnBadArgument);

    // IOSurfaceAddress uses caller storage without format conversion or rounded overread.
    props->flushCollection();
    number(props, "IOSurfaceWidth", 13); number(props, "IOSurfaceHeight", 17);
    number(props, "IOSurfacePixelFormat", 'w30r'); number(props, "IOSurfaceBytesPerRow", 64);
    number(props, "IOSurfaceAddress", clientBase + 37);
    a.structureInput = "dict"; a.structureInputSize = 4;
    assert(call(c, 0, a) == 0);
    assert(word(result, 0x30) == 'w30r' && quad(result, 0) == clientBase + 37 && word(result, 0x1c) == 1088);

    // Out-of-line result descriptors must be prepared and completed around copyout.
    auto output = IOBufferMemoryDescriptor::inTaskWithOptions(nullptr, 0, sizeof(result), page_size);
    a.structureOutputDescriptor = output;
    assert(call(c, 0, a) == 0 && a.structureOutputDescriptorSize == sizeof(result));
    assert(quad((uint8_t *)output->getBytesNoCopy(), 8) && output->prepared == 0);
    a.structureOutputDescriptor = nullptr; output->release();

    // Selector 7 has the same valid-state contract for a byte range.
    scalar[0] = clientBase + 51; scalar[1] = 130;
    a.scalarInputCount = 2;
    assert(call(c, 7, a) == 0 && quad(result, 0) == clientBase + 51 && quad(result, 8));
    close(c);
    assert(preparations == completions && sweet_surface_wired == 0);
    for (auto surface : sweet_surface_table) assert(surface == nullptr);
    props->release(); decoded = nullptr;
    IOLockFree(sweet_surface_lock); sweet_surface_lock = nullptr;
    std::free(pixels);
    puts("PASS: b3a8 target/fallback, selectors 6/7, shared state, attachment bounds, lookup/lifetime, client address");
}
