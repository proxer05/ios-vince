#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Print (via con_hex) every base/size pair of:
 *   - the /memory reg (the bootloader's own DRAM map), and
 *   - every child of /reserved-memory.
 * `out`/`max` kept for API stability; out[0] = pair count.
 */
int fdt_dump_memory(uintptr_t fdt, uint64_t *out, int max);
