/* Actual kernel decoder/state machine and GPIO loop, including observed faults. */
static jmp_buf hid_loop_done;
static unsigned hid_loop_step;

static void
hid_report(uint8_t *d, unsigned count)
{
	memset(d, 0xff, FTS_REPORT_LEN);
	d[0] = d[1] = 0;
	d[2] = (uint8_t)count;
}

static void
hid_record(uint8_t *d, unsigned pos, unsigned slot, unsigned event, unsigned x, unsigned y)
{
	uint8_t *p = d + 3 + 6 * pos;
	p[0] = (uint8_t)((event << 6) | (x >> 8)); p[1] = (uint8_t)x;
	p[2] = (uint8_t)((slot << 4) | (y >> 8)); p[3] = (uint8_t)y;
	p[4] = 0x7f; p[5] = 0x90;
}

static void
hid_event(unsigned i, unsigned slot, unsigned down, unsigned x, unsigned y)
{
	assert(i < hid_count);
	assert(hid_events[i].slot == (int)slot && hid_events[i].down == (int)down);
	assert(hid_events[i].x == (int)x && hid_events[i].y == (int)y);
}

static void
hid_loop_script(void)
{
	switch (++hid_loop_step) {
	case 1:
		assert(hid_count == 1); hid_event(0, 0, 1, 330, 1335);
		hid_report(wire.report, 2);
		hid_record(wire.report, 0, 0, 2, 400, 1400);
		hid_record(wire.report, 1, 1, 3, 1079, 1326);
		break;
	case 2:
		assert(hid_count == 1); /* invalid second record cannot move slot0 */
		hid_report(wire.report, 2);
		hid_record(wire.report, 0, 9, 0, 800, 1800);
		hid_record(wire.report, 1, 0, 2, 400, 1400);
		break;
	case 3:
		assert(hid_count == 3);
		hid_event(1, 0, 1, 400, 1400); hid_event(2, 9, 1, 800, 1800);
		hid_report(wire.report, 1);
		hid_record(wire.report, 0, 9, 2, 810, 1800);
		hid_record(wire.report, 1, 0, 1, 400, 1400);
		break;
	case 4:
		assert(hid_count == 5);
		hid_event(3, 0, 0, 400, 1400); hid_event(4, 9, 1, 810, 1800);
		wire.fail_clock = wire.highs + 100; /* partial data, no publish */
		wire.stretch = FTS_GPIO_STRETCH_US + 1;
		break;
	case 5:
		assert(hid_count == 5);
		wire.stuck_scl = true;
		mock_time_ns += 200 * NSEC_PER_MSEC;
		break;
	case 6:
		assert(hid_count == 6); hid_event(5, 9, 0, 810, 1800);
		wire_init();
		hid_report(wire.report, 1); hid_record(wire.report, 0, 1, 0, 600, 1700);
		break;
	case 7:
		assert(hid_count == 7); hid_event(6, 1, 1, 600, 1700);
		hid_report(wire.report, 0);
		/* Continuous HID must keep running beyond the diagnostic five-minute cap. */
		mock_time_ns += 301000ull * NSEC_PER_MSEC;
		break;
	case 8:
		assert(hid_count == 8); hid_event(7, 1, 0, 600, 1700);
		assert(wire.sda && wire.scl && !wire.active);
		longjmp(hid_loop_done, 1);
	default: assert(false);
	}
}

static void
hid_identity_script(void)
{
	assert(++hid_loop_step == 1 && hid_count == 1);
	wire.bad_id = true;
	mock_time_ns += 30000ull * NSEC_PER_MSEC;
}

static void
test_gpio_hid(void)
{
	uint8_t d[FTS_REPORT_LEN];
	struct fts_hid_frame f, untouched;
	fresh(); allow_hid = true;
	struct fts_hid_state s = {0};
	/* All ten slots, reverse record order, inclusive vendor coordinate limits. */
	hid_report(d, 10);
	for (unsigned i = 0; i < 10; i++) { hid_record(d, i, 9 - i, 2, 1080, 2280); }
	assert(!fts_hid_accept(&s, d, sizeof(d), 10) && s.frame.active == 0x3ff);
	assert(hid_count == 10);
	for (unsigned i = 0; i < 10; i++) { hid_event(i, i, 1, 1080, 2280); }
	/* Repeated valid holds refresh the watchdog and retry late HID registration. */
	assert(!fts_hid_accept(&s, d, sizeof(d), 140));
	assert(hid_count == 20);
	fts_hid_expire(&s, 289); assert(hid_count == 20);
	fts_hid_expire(&s, 290); assert(hid_count == 30 && s.stale == 1 && !s.frame.active);
	fts_hid_expire(&s, 500); assert(hid_count == 30 && s.stale == 1);
	for (unsigned i = 0; i < 10; i++) { hid_event(i + 20, i, 0, 1080, 2280); }
	/* No output mutation on any decode failure, including short frames. */
	memset(&untouched, 0xa5, sizeof(untouched));
	for (unsigned fault = 0; fault < 9; fault++) {
		hid_report(d, 1); hid_record(d, 0, 0, 2, 500, 1500);
		unsigned len = sizeof(d); int expected;
		switch (fault) {
		case 0: len--; expected = 1; break;
		case 1: d[0] = 0x40; expected = 2; break;
		case 2: d[2] = 15; expected = 3; break;
		case 3: hid_record(d, 1, 0, 1, 500, 1500); expected = 4; break;
		case 4: hid_record(d, 1, 1, 3, 500, 1500); expected = 5; break;
		case 5: hid_record(d, 0, 0, 2, 1081, 1500); expected = 6; break;
		case 6: hid_record(d, 0, 0, 2, 500, 2281); expected = 6; break;
		case 7: d[2] = 0; expected = 7; break;
		default: d[5] = 0xa5; expected = 7; break;
		}
		f = untouched;
		assert(fts_hid_decode(d, len, &f) == expected && !memcmp(&f, &untouched, sizeof(f)));
	}
	assert(fts_hid_decode(NULL, 63, &f) == 1);
	/* Actual 0193 faults: event3 twice and header lagging a release. */
	static const uint8_t bad[][15] = {
		{0,0,2,0x81,0x4a,0x05,0x37,0x7f,0xa0,0xc4,0x37,0x15,0x2e,0x53,0x30},
		{0,0,2,0x81,0x4a,0x05,0x37,0x7f,0x90,0xc4,0x37,0x15,0x2c,0x49,0x30},
		{0,0,2,0x82,0xe5,0x14,0xff,0x7f,0x90,0x41,0x29,0x05,0x7a,0,0},
	};
	for (unsigned i = 0; i < 3; i++) {
		memset(d, 0xff, sizeof(d)); memcpy(d, bad[i], sizeof(bad[i]));
		assert(fts_hid_accept(&s, d, sizeof(d), 510 + i) == (i == 2 ? 7 : 5));
		assert(hid_count == 30 && s.last_valid == 140);
	}
	/* Sparse IDs and explicit UP plus implicit missing-slot release. */
	hid_report(d, 2); hid_record(d, 0, 9, 0, 100, 1000); hid_record(d, 1, 3, 2, 700, 1600);
	assert(!fts_hid_accept(&s, d, sizeof(d), 520));
	assert(s.frame.active == ((1u << 3) | (1u << 9)));
	hid_report(d, 1); hid_record(d, 0, 9, 1, 100, 1000); hid_record(d, 1, 1, 0, 800, 1700);
	assert(!fts_hid_accept(&s, d, sizeof(d), 550));
	assert(hid_count == 35);
	hid_event(32, 3, 0, 700, 1600); hid_event(33, 9, 0, 100, 1000); hid_event(34, 1, 1, 800, 1700);
	/* Single malformed frame holds state; repeated invalid/FF data expires it. */
	memset(d, 0xff, sizeof(d)); d[0] = 0;
	assert(fts_hid_accept(&s, d, sizeof(d), 580) == 3 && hid_count == 35);
	assert(fts_hid_accept(&s, d, sizeof(d), 699) == 3 && hid_count == 35);
	assert(fts_hid_accept(&s, d, sizeof(d), 700) == 3 && hid_count == 36);
	hid_event(35, 1, 0, 800, 1700);
	/* Configuration/power/identity faults cannot enter the continuous HID loop. */
	for (unsigned fault = 0; fault < 10; fault++) {
		fresh(); wire_init(); power_ready = true; mock_tick_ns = 100;
		boot_hid = boot_daisy = boot_gpio = boot_power = boot_busywait = 1;
		if (fault == 0) { boot_gpio = 0; }
		if (fault == 1) { boot_power = 0; }
		if (fault == 2) { boot_busywait = 0; }
		if (fault == 3) { boot_reports = 1; }
		if (fault == 4) { power_result = -7; }
		if (fault == 5) { power_ready = false; }
		if (fault == 6) { wire.bad_id = true; }
		if (fault == 7) { wire.nack_byte = 7; } /* firmware */
		if (fault == 8) { wire.nack_byte = 10; } /* vendor */
		if (fault == 9) { boot_hid = 2; } /* explicit opt-in is exactly 1 */
		vince_input_start(); started_fn(NULL, 0);
		assert(!hid_count && !wire.report_reads && !qup_accesses);
		if (fault < 6) { assert(!(gpio[GPIO_IO(GPIO_RST) / 4] & 2u)); }
	}
	/* Actual transport -> validation -> HID, through the normal startup gates. */
	fresh(); wire_init(); power_ready = allow_hid = true; mock_tick_ns = 100;
	boot_hid = boot_daisy = boot_gpio = boot_power = boot_busywait = 1;
	hid_report(wire.report, 1); hid_record(wire.report, 0, 0, 0, 330, 1335);
	hid_loop_step = 0; hid_sleep_hook = hid_loop_script;
	vince_input_start();
	if (!setjmp(hid_loop_done)) { started_fn(NULL, 0); assert(false); }
	assert(hid_loop_step == 8 && hid_count == 8 && power_calls == 1);
	assert(!qup_maps && !qup_accesses);
	assert(nsleeps == 3 && sleeps[0] == 20 && sleeps[1] == 20 && sleeps[2] == 200);
	hid_sleep_hook = NULL;
	/* Optional delayed identity loss exits only after releasing the active slot. */
	fresh(); wire_init(); power_ready = allow_hid = true; mock_tick_ns = 100;
	boot_hid = boot_daisy = boot_gpio = boot_power = boot_busywait = boot_probe = 1;
	hid_report(wire.report, 1); hid_record(wire.report, 0, 4, 2, 500, 1200);
	hid_loop_step = 0; hid_sleep_hook = hid_identity_script;
	vince_input_start(); started_fn(NULL, 0);
	assert(hid_loop_step == 1 && hid_count == 4);
	hid_event(3, 4, 0, 500, 1200);
	hid_sleep_hook = NULL;
}
