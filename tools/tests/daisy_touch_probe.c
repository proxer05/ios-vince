/* Deterministic MMIO model. Block input stalls until IN_SVC is acknowledged. */
static uint32_t regs[0x500 / 4], gpio[TLMM_SIZE / 4], tags[2];
static unsigned tag_count, resets, delivered, released, limit, svc_acks;
static unsigned sleeps[16], nsleeps, starts, qup_accesses, qup_maps;
static bool running, nack, late_nack, stuck_bus, missing_done, bad_state;
static int boot_vince, boot_daisy, boot_probe, boot_gpio, boot_power, boot_rpm_diag, boot_reports;
static unsigned capture_step_ms, capture_sleeps;
static bool capture_change, capture_bad_id;
static uint32_t arb_version, observer_status, observer_reg;
static unsigned observer_reads, observer_data_reads;
static bool observer_missing, observer_map_fail, arb_map_fail, observer_unstable;
static bool power_ready, power_bad_type;
static unsigned power_calls, layout_calls;
static int power_result;
static void (*started_fn)(void *, wait_result_t);

static void
fresh(void)
{
	memset(regs, 0, sizeof(regs));
	memset(gpio, 0, sizeof(gpio));
	tag_count = resets = delivered = released = svc_acks = nsleeps = starts = 0;
	qup_accesses = qup_maps = observer_reads = observer_data_reads = 0;
	arb_version = 0x20010000;
	observer_status = 1;
	observer_missing = observer_map_fail = arb_map_fail = observer_unstable = false;
	memset(fts_rail_obs, 0, sizeof(fts_rail_obs));
	memset(&wire, 0, sizeof(wire));
	limit = 255;
	running = nack = late_nack = stuck_bus = missing_done = bad_state = false;
	boot_vince = boot_daisy = boot_probe = boot_gpio = boot_power = boot_rpm_diag = 0;
	boot_reports = 0;
	capture_step_ms = capture_sleeps = 0;
	capture_change = capture_bad_id = false;
	mock_time_ns = 0;
	power_ready = power_bad_type = false;
	power_calls = layout_calls = 0;
	power_result = 0;
	started_fn = NULL;
	input_started = false;
	daisy_touch = daisy_probe = daisy_diag = daisy_gpio = daisy_power = daisy_rpm_diag = daisy_reports = 0;
	qup_last_bus_error = qup_last_error = 0;
	qup = 1;
	tlmm = 2;
	gcc = 3;
	qup_in_fifo = qup_out_fifo = 32;
}

static unsigned
length(void)
{
	return (regs[QUP_CONFIG / 4] & CFG_NO_INPUT) ? 0 :
	       regs[QUP_MX_IN_CNT / 4] + regs[QUP_MX_RD_CNT / 4];
}

static uint32_t
mock_rd(vm_offset_t base, uint32_t off)
{
	if (base == tlmm) {
		if (wire.enabled && (off == GPIO_IO(GPIO_SDA) || off == GPIO_IO(GPIO_SCL))) {
			return (gpio[off / 4] & ~1u) | wire_level(off / 0x1000);
		}
		return gpio[off / 4];
	}
	if (base == 4) {
		if (!off) { return arb_version; }
		assert(off >= 0x800 && off < 0x1000);
		if (observer_missing) { return 0; }
		/* Include one APID above the old Vince helper's 256-entry scan. */
		return off == 0x800 + 4 * 300 ? 0x14900 : off == 0x800 + 4 * 42 ? 0x14500 : 0;
	}
	if (base == 5 || base == 6) {
		if (off == 8) { return observer_status; }
		assert(off == 0x18);
		if (observer_unstable) { return observer_data_reads++ & 1u; }
		if (daisy_power) {
			if (observer_reg == 4) { return power_bad_type ? 0 : 0x21; }
			if (observer_reg == 8 || observer_reg == 0x46) { return power_ready ? 0x80 : 0; }
		}
		return observer_reg ^ (base == 5 ? 0x10 : 0x06);
	}
	qup_accesses++;
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
			op |= OP_MX_IN_DONE | OP_MX_OUT_DONE;
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
		unsigned pin = off / 0x1000;
		if (wire.enabled && (pin == GPIO_SDA || pin == GPIO_SCL)) {
			wire_update(pin, gpio[GPIO_CTL(pin) / 4], gpio[GPIO_IO(pin) / 4], !(off & 0xfff));
		}
		return;
	}
	if (base == 5 || base == 6) {
		assert(off == 0 && (val >> 27) == 1 && !(val & 15));
		observer_reg = (val >> 4) & 0xff;
		assert(observer_reg == 4 || observer_reg == 5 || observer_reg == 8 ||
		    observer_reg == 0x46 || observer_reg == 0x40 || observer_reg == 0x41);
		observer_reads++;
		return;
	}
	qup_accesses++;
	if (base != qup) {
		return;
	}
	if (off == QUP_SW_RESET) {
		resets++;
		running = false;
		tag_count = delivered = released = 0;
		memset(regs, 0, sizeof(regs));
		regs[QUP_STATE / 4] = ST_RESET;
		return;
	}
	if (off == QUP_OUT_FIFO) {
		assert(tag_count < 2);
		tags[tag_count++] = val;
		return;
	}
	if (off == QUP_STATE) {
		running = val == ST_RUN && tag_count > 0;
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
	mock_time_ns += (uint64_t)ms * NSEC_PER_MSEC;
	if (ms == 20 && capture_step_ms && (gpio[GPIO_IO(GPIO_RST) / 4] & 2u)) {
		capture_sleeps++;
		mock_time_ns += (uint64_t)capture_step_ms * NSEC_PER_MSEC;
		if (capture_change) { wire.report[1] ^= 1; }
		if (capture_bad_id) { wire.bad_id = true; }
		return;
	}
	assert(nsleeps < 16);
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
	if (addr == SPMI_CORE_PA) { return arb_map_fail ? 0 : 4; }
	if (addr == SPMI_OBS_PA + 0x8000u * 300) { return observer_map_fail ? 0 : 5; }
	if (addr == SPMI_OBS_PA + 0x8000u * 42) { return observer_map_fail ? 0 : 6; }
	if (addr == QUP3_PA || addr == GCC_QUP3_PA) { qup_maps++; }
	assert(addr == QUP3_PA || addr == GCC_QUP3_PA || addr == TLMM_PA);
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
int daisy_touch_power_on(void)
{
	assert(!(gpio[GPIO_IO(GPIO_RST) / 4] & 2));
	power_calls++;
	return power_result;
}
int daisy_touch_rpm_layout(void)
{
	assert(!(gpio[GPIO_IO(GPIO_RST) / 4] & 2));
	layout_calls++;
	return -1;
}
static bool PE_parse_boot_argn(const char *key, void *p, int size)
{
	assert(size == sizeof(int));
	int *out = p;
	if (!strcmp(key, "vincekeys")) { *out = 0; }
	else if (!strcmp(key, "vincetouch")) { *out = boot_vince; }
	else if (!strcmp(key, "daisytouch")) { *out = boot_daisy; }
	else if (!strcmp(key, "daisytouch_probe")) { *out = boot_probe; }
	else if (!strcmp(key, "daisytouch_gpio")) { *out = boot_gpio; }
	else if (!strcmp(key, "daisytouch_power")) { *out = boot_power; }
	else if (!strcmp(key, "daisytouch_rpm_diag")) { *out = boot_rpm_diag; }
	else if (!strcmp(key, "daisytouch_reports")) { *out = boot_reports; }
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
		if (i < 2) { assert(qup_last_bus_error & I2C_NACK); }
		assert(!running && resets == 2);
		nack = late_nack = stuck_bus = missing_done = bad_state = false;
		limit = 255;
		assert(qup_reg_read(0x38, 0, buf, 63) == 0);
	}
	fresh();
	/* Original Vince message framing is preserved at either slave address. */
	for (unsigned is_daisy = 0; is_daisy < 2; is_daisy++) {
		daisy_touch = is_daisy;
		uint8_t reg = 0xa3;
		assert(qup_msg(0, &reg, NULL, 1) == 0);
		assert(tag_count == 1);
		assert(tags[0] == ((TAG_START | ((is_daisy ? FTS_ADDR : RMI_ADDR) << 1)) |
		    ((TAG_STOP | reg) << 16)));
		assert(qup_msg(1, NULL, buf, 1) == 0);
		assert(tags[0] == ((TAG_START | ((is_daisy ? FTS_ADDR : RMI_ADDR) << 1) | 1u) |
		    ((TAG_REC | 1u) << 16)));
	}
	qup_clk_ctl = CLK_CTL_100K;
	fts_vince_crosscheck();
	assert(qup_clk_ctl == CLK_CTL_100K && !running);
	fresh();
	boot_reports = boot_power = boot_rpm_diag = boot_gpio = boot_probe = 1; /* hints alone never turn touch on */
	vince_input_start();
	assert(starts == 0 && !power_calls);
	fresh();
	boot_vince = boot_gpio = boot_power = boot_rpm_diag = 1; /* hints do not change Vince selection */
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
	/* GPIO transactions: decode the actual edges, address bytes, and final NACK. */
	fresh();
	wire_init();
	uint8_t value = 0xee;
	unsigned stage;
	assert(fts_gpio_read(0xa3, &value, &stage) == 0 && value == 0x54 && stage == 7);
	assert(wire.starts == 2 && wire.stops == 1 && wire.master_nacks == 1);
	assert(!wire.master_acks);
	assert(wire.nbytes == 3 && !memcmp(wire.bytes, "\x70\xa3\x71", 3));
	assert(wire.sda && wire.scl && !wire.active && !qup_accesses);
	unsigned clocks = wire.highs - 1; /* omit the final cleanup release */
	for (unsigned fault = 1; fault <= clocks; fault++) {
		fresh(); wire_init();
		wire.fail_clock = fault;
		wire.stretch = FTS_GPIO_STRETCH_US + 1;
		value = 0xee;
		assert(fts_gpio_read(0xa3, &value, &stage) == -4);
		assert(value == 0xee && wire.sda && wire.scl);
	}
	for (unsigned byte = 1; byte <= 3; byte++) {
		fresh(); wire_init();
		wire.nack_byte = byte;
		value = 0xee;
		assert(fts_gpio_read(0xa3, &value, &stage) == -2);
		assert(stage == (byte == 3 ? 5 : byte + 1));
		assert(value == 0xee && wire.sda && wire.scl);
	}
	fresh(); wire_init();
	wire.fail_clock = 30; /* finite stretching during receive */
	wire.stretch = 3;
	assert(fts_gpio_read(0x9f, &value, &stage) == 0 && value == 0x22);
	fresh(); wire_init(); wire.stuck_sda = true;
	value = 0xee;
	assert(fts_gpio_read(0xa3, &value, &stage) == -6 && stage == 1 && value == 0xee);
	assert(fts_gpio_recover() == -6 && wire.sda && wire.scl);
	assert(wire.highs < 20); /* bounded recovery */
	fresh(); wire_init(); wire.stuck_scl = true;
	assert(fts_gpio_read(0xa3, &value, &stage) == -4 && stage == 1);
	assert(fts_gpio_recover() == -4 && wire.sda && wire.scl);
	fresh(); wire_init();
	assert(fts_gpio_recover() == 0 && wire.sda && wire.scl);
	/* Rail map/read failures are explicit, and only observer READ commands occur. */
	fresh();
	fts_rail_init();
	assert(fts_rail_obs[0] == 5 && fts_rail_obs[1] == 6);
	fts_rail_summary();
	assert(observer_reads == 24 && !qup_accesses);
	value = 0xee;
	observer_status = 5; /* DENIED takes precedence over DONE */
	assert(fts_rail_read(5, 8, &value) == -5 && value == 0xee);
	observer_status = 0;
	assert(fts_rail_read(5, 8, &value) == -100 && value == 0xee);
	observer_status = 4; /* stale DENIED without DONE is not a completed error */
	assert(fts_rail_read(5, 8, &value) == -100 && value == 0xee);
	observer_status = 1;
	observer_unstable = true;
	assert(fts_rail_read(5, 8, &value) == -101 && value == 0xee);
	observer_unstable = false;
	assert(fts_rail_read(0, 8, &value) == -1);
	for (unsigned fault = 0; fault < 4; fault++) {
		fresh();
		if (fault == 0) { arb_version = 0x50000000; }
		if (fault == 1) { arb_map_fail = true; }
		if (fault == 2) { observer_missing = true; }
		if (fault == 3) { observer_map_fail = true; }
		fts_rail_init();
		fts_rail_summary();
		assert(!fts_rail_obs[0] && !fts_rail_obs[1] && !observer_reads);
	}
	/* Run the actual bounded GPIO thread, with all competing selectors set. */
	fresh(); wire_init();
	boot_vince = boot_daisy = boot_gpio = boot_probe = 1;
	vince_input_start();
	assert(starts == 1 && started_fn == fts_probe_thread && !qup_maps);
	started_fn(NULL, 0);
	assert(!qup_accesses && nsleeps == 4 && sleeps[0] == 20 && sleeps[1] == 200);
	assert(sleeps[2] == 30000 && sleeps[3] == 30000 && observer_reads == 72);
	assert(wire.nbytes == 36 && wire.sda && wire.scl);
	fresh(); wire_init(); wire.absent = true;
	daisy_gpio = daisy_probe = 1;
	fts_probe_thread(NULL, 0);
	assert(!qup_accesses && wire.nbytes == 9 && nsleeps == 10);
	assert(wire.sda && wire.scl && observer_reads == 72);
	/* The power option only runs in GPIO mode. Reset cannot release on errors. */
	for (unsigned fault = 0; fault < 5; fault++) {
		fresh(); wire_init();
		boot_daisy = boot_gpio = boot_power = boot_probe = 1;
		power_ready = true;
		if (fault == 1) { power_result = -7; }
		if (fault == 2) { power_ready = false; }
		if (fault == 3) { power_bad_type = true; }
		if (fault == 4) { observer_status = 5; }
		vince_input_start();
		started_fn(NULL, 0);
		assert(power_calls == (fault >= 3 ? 0 : 1));
		assert(!qup_maps && !qup_accesses && wire.sda && wire.scl);
		if (!fault) {
			assert(wire.nbytes == 36 && nsleeps == 5);
			assert(sleeps[0] == 20 && sleeps[1] == 20 && sleeps[2] == 200);
		} else {
			assert(!wire.nbytes && nsleeps == 3);
			assert(!(gpio[GPIO_IO(GPIO_RST) / 4] & 2));
		}
	}
	/* Layout diagnostics supersede power and identity and remain read-only. */
	fresh(); wire_init();
	boot_reports = boot_daisy = boot_gpio = boot_power = boot_rpm_diag = boot_probe = 1;
	vince_input_start();
	started_fn(NULL, 0);
	assert(layout_calls == 3 && !power_calls && !wire.nbytes);
	assert(!qup_maps && !qup_accesses && wire.sda && wire.scl);
	assert(!(gpio[GPIO_IO(GPIO_RST) / 4] & 2));
	assert(nsleeps == 2 && sleeps[0] == 30000 && sleeps[1] == 30000);
	test_gpio_reports();
	puts("PASS: QUP/GPIO atomic report transport, ACK/NACK/STOP faults, identity/power gating, bounded capture, no HID");
	return 0;
}
