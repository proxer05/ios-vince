/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"
#include "console.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

/* the first 15 DRAM range pairs seen in /memory */
static uint64_t mem_pairs[15][2];
static int n_pairs;

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

static void mem_cb(uint64_t base, uint64_t size)
{
	con_hex("MEMB", base);
	con_hex("MEMS", size);
	if (n_pairs < 15) {
		mem_pairs[n_pairs][0] = base;
		mem_pairs[n_pairs][1] = size;
		n_pairs++;
	}
}

static void rmem_cb(uint64_t base, uint64_t size)
{
	con_hex("RMEMB", base);
	con_hex("RMEMS", size);
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

/*
 * Walk the tree; call back per node with its depth and name string.
 * Used for both: finding /memory reg and every /reserved-memory child's reg.
 * (u64 bodies are read as pairs of big-endian 32-bit words.)
 *
 * `walk` returns non-zero to stop the traversal early (before that node's
 * subtree is consumed).
 */
static void dump_node_reg(uintptr_t fdt, int want_depth, const char *want_name,
			  const char *prop,
			  void (*cb)(uint64_t base, uint64_t size))
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, match = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			match = depth == want_depth && match == 0 && streq(name, want_name);
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			depth--;
			match = 0;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (match && streq((const char *)(strings + nameoff), prop) &&
			    (len & 15) == 0) {
				for (uint32_t j = 0; j * 4 < len; j += 4) {
					uint64_t base = (uint64_t)be32(val + 4 * j) << 32 |
							be32(val + 4 * j + 4);
					uint64_t size = (uint64_t)be32(val + 4 * j + 8) << 32 |
							be32(val + 4 * j + 12);
					cb(base, size);
				}
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END */
			return;
		}
	}
}

/* reg of every depth-3 child of /reserved-memory (or /reserve-memory). */
static void dump_reserved_children(uintptr_t fdt)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, in_rm = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_rm = depth == 2 && streq(name, "reserved-memory") ||
				depth == 2 && streq(name, "reserve-memory") ||
				depth == 3 && in_rm;
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (depth == 2 && in_rm)
				in_rm = 0;
			depth--;
			if (depth == 2)
				in_rm = 0;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (depth == 3 && in_rm &&
			    streq((const char *)(strings + nameoff), "reg") &&
			    (len & 15) == 0) {
				for (uint32_t j = 0; j * 4 < len; j += 4) {
					uint64_t base = (uint64_t)be32(val + 4 * j) << 32 |
							be32(val + 4 * j + 4);
					uint64_t size = (uint64_t)be32(val + 4 * j + 8) << 32 |
							be32(val + 4 * j + 12);
					rmem_cb(base, size);
				}
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END */
			return;
		}
	}
}

int fdt_dump_memory(uintptr_t fdt, uint64_t *out, int max)
{
	(void)max;
	n_pairs = 0;
	for (int i = 0; i < 15; i++) {
		mem_pairs[i][0] = 0;
		mem_pairs[i][1] = 0;
	}
	dump_node_reg(fdt, 2, "memory", "reg", mem_cb);
	dump_reserved_children(fdt);
	out[0] = n_pairs;
	out[1] = 0;
	return n_pairs;
}
