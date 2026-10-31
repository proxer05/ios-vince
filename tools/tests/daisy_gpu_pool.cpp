static IOMemoryDescriptor *allocate(uint64_t size, uint8_t **bytes = nullptr) {
    uint8_t *kernel; uint64_t address;
    auto md = gpuGemAllocate(size, &kernel, &address);
    if (md) {
        IOByteCount length;
        assert(md->getPhysicalSegment(0, &length, kIOMemoryMapperNone) == address && length == size);
        assert(address % PAGE_SIZE == 0 && address + size <= GPU_ADDRESS_LIMIT);
        if (bytes) *bytes = kernel;
    }
    return md;
}

static void cleanup() {
    assert(gPoolUsed == 0);
    for (unsigned i = 0; i < gPoolCount; ++i) {
        for (auto bits : gPool[i].used) assert(bits == 0);
        gPool[i].memory->release(); gPool[i] = {};
    }
    delete gPoolLock; gPoolLock = nullptr;
    gPoolCount = gPoolPeak = gPoolMisses = 0;
}

static void exercise() {
    poolMB = 0;
    failPhysicalAt = 0;
    // Without the boot argument, use the original allocator.
    gpuPoolInit(); assert(gPoolCount == 0 && !gPoolLock);
    auto ordinary = allocate(PAGE_SIZE); assert(ordinary); ordinary->release();
    poolMB = 64; gpuPoolInit(); assert(gPoolCount == 2);
    unsigned calls = physicalCalls;
    constexpr uint64_t target = 12u << 20;
    auto a = allocate(target), b = allocate(target), c = allocate(target), d = allocate(target);
    assert(a && b && c && d);
    assert(allocate(target) == nullptr); // free 16 MiB, but no 12 MiB span
    assert(physicalCalls == calls); // exhaustion must not enter global VM scanning
    b->release(); d->release();

    // Reproduce the repeated 12 MiB target requests without losing physical runs.
    for (unsigned i = 0; i < 111; ++i) {
        uint8_t *bytes;
        auto targetMemory = allocate(target, &bytes); assert(targetMemory);
        for (size_t p = 0; p < target; ++p) assert(bytes[p] == 0);
        memset(bytes, 0xa5, target);
        targetMemory->release();
    }
    a->release(); c->release(); assert(gPoolUsed == 0 && physicalCalls == calls);

    // GEM_CLOSE releases the handle, but a live user mapping still owns the range.
    a = allocate(GPU_POOL_CHUNK); b = allocate(GPU_POOL_CHUNK); assert(a && b);
    auto map = a->map(); uint64_t oldPA = a->pa;
    a->release(); assert(gPoolUsed == 2 * GPU_POOL_PAGES && !allocate(PAGE_SIZE));
    map->release(); assert(gPoolUsed == GPU_POOL_PAGES);
    a = allocate(GPU_POOL_CHUNK); assert(a && a->pa == oldPA);
    a->release(); b->release();

    // Descriptor failure unwinds its lease; adjacent frees coalesce for a whole chunk.
    failDescriptor = true; assert(!allocate(target) && gPoolUsed == 0); failDescriptor = false;
    std::vector<IOMemoryDescriptor *> pieces;
    for (unsigned i = 0; i < 128; ++i) { a = allocate(512u << 10); assert(a); pieces.push_back(a); }
    for (unsigned parity = 0; parity < 2; ++parity)
        for (unsigned i = parity; i < pieces.size(); i += 2) pieces[i]->release();
    assert(gPoolUsed == 0);
    a = allocate(GPU_POOL_CHUNK); b = allocate(GPU_POOL_CHUNK); assert(a && b);
    a->release(); b->release();
    assert(!allocate(0) && !allocate(17) && !allocate(GPU_POOL_CHUNK + PAGE_SIZE));
    assert(physicalCalls == calls);
    cleanup();

    // A partial early reserve is still usable and does not retry failed chunks later.
    poolMB = 96; failPhysicalAt = physicalCalls + 2; gpuPoolInit(); assert(gPoolCount == 1);
    calls = physicalCalls;
    a = allocate(target); assert(a && physicalCalls == calls); a->release(); cleanup();
    printf("PASS (%u-byte runtime pages): GEM pool DMA32/WC, 111 target reuses, fragmentation, zero-fill, mapping lifetime, failure cleanup\n", PAGE_SIZE);
}

int main() {
    for (auto size : {4096u, 16384u}) {
        vm_page_size = size;
        exercise();
    }
}
