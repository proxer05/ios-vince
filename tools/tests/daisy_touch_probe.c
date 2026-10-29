/* Deterministic MMIO model. Block input stalls until IN_SVC is acknowledged. */
static uint32_t regs[0x500 / 4], gpio[TLMM_SIZE / 4], tags[2];
static unsigned tag_count, resets, delivered, released, limit, svc_acks;
static unsigned sleeps[8], nsleeps, starts;
static bool running, nack, late_nack, stuck_bus, missing_done, bad_state;
static int boot_vince, boot_daisy, boot_probe;
static void (*started_fn)(void *, wait_result_t);

static void
fresh(void)
{
	memset(regs, 0, sizeof(regs));
	memset(gpio, 0, sizeof(gpio));
	tag_count = resets = delivered = released = svc_acks = nsleeps = starts = 0;
	limit = 255;
	running = nack = late_nack = stuck_bus = missing_done = bad_state = false;
	boot_vince = boot_daisy = boot_probe = 0;
	started_fn = NULL;
	input_started = false;
	daisy_touch = daisy_probe = 0;
	qup = 1;
	tlmm = 2;
	gcc = 3;
	qup_in_fifo = qup_out_fifo = 32;
}

static unsigned
length(void)
{
	return regs[QUP_MX_IN_CNT / 4] + regs[QUP_MX_RD_CNT / 4];
}

static uint32_t
mock_rd(vm_offset_t base, uint32_t off)
{
	if (base == tlmm) {
		return gpio[off / 4];
	}
	if (base != qup) {
		return 0; /* GCC: branch clock already running */
	}
	if (off == QUP_STATE) {
		return bad_state ? 0 : regs[off / 4] | ST_VALID | ST_MAST_GEN;
	}
	if (off == QUP_I2C_STATUS) {
		if (nack || (late_nack && running && delivered == length())) {
			return I2C_NACK;
		}
		return running && (stuck_bus || delivered < length()) ? I2C_BUS_ACTIVE : 0;
	}
	if (off == QUP_OPERATIONAL) {
		if (!running) {
			return 0;
		}
		uint32_t op = 0;
		if (delivered < length() && delivered < limit) {
			if (delivered < released) {
				op |= OP_IN_NOT_EMPTY;
			} else {
				op |= OP_IN_SVC;
			}
		}
		if (delivered == length() && !missing_done) {
			op |= OP_MX_IN_DONE;
		}
		return op;
	}
	if (off == QUP_IN_FIFO) {
		assert(delivered < released);
		uint32_t v = delivered & 0xffu;
		delivered++;
		if (delivered < length()) {
			assert(delivered < released);
			v |= (delivered & 0xffu) << 16;
			delivered++;
		}
		return v;
	}
	return regs[off / 4];
}

static void
mock_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	if (base == tlmm) {
		gpio[off / 4] = val;
		return;
	}
	if (base != qup) {
		return;
	}
	if (off == QUP_SW_RESET) {
		resets++;
		running = false;
		tag_count = delivered = released = 0;
		regs[QUP_STATE / 4] = ST_RESET;
		return;
	}
	if (off == QUP_OUT_FIFO) {
		assert(tag_count < 2);
		tags[tag_count++] = val;
		return;
	}
	if (off == QUP_STATE) {
		running = val == ST_RUN && tag_count == 2;
		if (running && !(regs[QUP_IO_MODE / 4] & IO_IN_BLOCK)) {
			released = length();
		}
	}
	if (off == QUP_OPERATIONAL) {
		if (running && (val & OP_IN_SVC) && delivered < length()) {
			svc_acks++;
			released = delivered + 8; /* four packed FIFO words per block */
			if (released > length()) {
				released = length();
			}
		}
		return;
	}
	regs[off / 4] = val;
}

static void
mock_sleep(unsigned ms)
{
	assert(nsleeps < 8);
	sleeps[nsleeps++] = ms;
	if (ms == 20) {
		assert(!(gpio[GPIO_IO(GPIO_RST) / 4] & 2u));
	} else if (ms == 200) {
		assert(gpio[GPIO_IO(GPIO_RST) / 4] & 2u);
	}
}

vm_offset_t ml_io_map(vm_offset_t addr, vm_size_t size)
{
	(void)size;
	return addr == QUP3_PA ? 1 : addr == TLMM_PA ? 2 : 3;
}
kern_return_t thread_terminate(thread_t t) { (void)t; return 0; }
void sweet_hid_key(int k, int d) { (void)k; (void)d; }
void sweet_hid_touch(int f, int d, int x, int y)
{
	(void)f; (void)d; (void)x; (void)y;
	assert(false && "probe must not dispatch HID");
}
void vince_adreno_init(void) {}
static bool PE_parse_boot_argn(const char *key, void *p, int size)
{
	assert(size == sizeof(int));
	int *out = p;
	if (!strcmp(key, "vincekeys")) { *out = 0; }
	else if (!strcmp(key, "vincetouch")) { *out = boot_vince; }
	else if (!strcmp(key, "daisytouch")) { *out = boot_daisy; }
	else if (!strcmp(key, "daisytouch_probe")) { *out = boot_probe; }
	else { return false; }
	return true;
}
static int kernel_thread_start(void (*fn)(void *, wait_result_t), void *a, thread_t *t)
{
	(void)a;
	*t = 1;
	starts++;
	started_fn = fn;
	return 0;
}

int main(void)
{
	uint8_t buf[257];
	const unsigned sizes[] = {1, 2, 31, 32, 63, 255};
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		unsigned n = sizes[i];
		fresh();
		memset(buf, 0xee, sizeof(buf));
		assert(qup_reg_read(0x38, 0xa3, buf + 1, n) == 0);
		assert(tags[0] == ((TAG_START | 0x70) | ((TAG_DATA | 0xa3) << 16)));
		assert(tags[1] == ((TAG_START | 0x71) | ((TAG_REC | n) << 16)));
		assert(regs[QUP_MX_WR_CNT / 4] == 4);
		assert(regs[QUP_MX_IN_CNT / 4] == (n >= 32 ? n : 0));
		assert(regs[QUP_MX_RD_CNT / 4] == (n >= 32 ? 0 : n));
		assert(regs[QUP_IO_MODE / 4] == (IO_REPACK | (n >= 32 ? IO_IN_BLOCK : 0)));
		assert(n < 32 || svc_acks >= (n + 7) / 8);
		for (unsigned j = 0; j < n; j++) { assert(buf[j + 1] == j); }
		assert(buf[0] == 0xee && buf[n + 1] == 0xee);
		assert(!running && resets == 1);
	}
	fresh();
	assert(qup_reg_read(0x80, 0, buf, 1) == -5);
	assert(qup_reg_read(0x38, 0, NULL, 1) == -5);
	assert(qup_reg_read(0x38, 0, buf, 0) == -5);
	assert(qup_reg_read(0x38, 0, buf, 256) == -5);
	qup_out_fifo = 3;
	assert(qup_reg_read(0x38, 0, buf, 1) == -5);
	assert(resets == 0);
	for (unsigned i = 0; i < 6; i++) {
		fresh();
		if (i == 0) { nack = true; }
		if (i == 1) { late_nack = true; }
		if (i == 2) { stuck_bus = true; }
		if (i == 3) { limit = 16; }
		if (i == 4) { missing_done = true; }
		if (i == 5) { bad_state = true; }
		int r = qup_reg_read(0x38, 0, buf, 63);
		assert(r == (i < 2 ? -2 : i == 5 ? -1 : -4));
		assert(!running && resets == 2);
		nack = late_nack = stuck_bus = missing_done = bad_state = false;
		limit = 255;
		assert(qup_reg_read(0x38, 0, buf, 63) == 0);
	}
	fresh();
	boot_probe = 1; /* hints alone never turn touch on */
	vince_input_start();
	assert(starts == 0);
	fresh();
	boot_vince = 1;
	vince_input_start();
	assert(starts == 1 && started_fn == touch_thread);
	fresh();
	boot_vince = boot_daisy = boot_probe = 1;
	vince_input_start();
	vince_input_start();
	assert(starts == 1 && started_fn == fts_probe_thread);
	fresh();
	daisy_touch = 1;
	assert(touch_hw_init());
	assert(nsleeps == 2 && sleeps[0] == 20 && sleeps[1] == 200);
	puts("PASS: combined tags, FIFO/block reads, bounds, failures, recovery, selection, reset");
	return 0;
}
