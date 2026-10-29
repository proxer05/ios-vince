/* Actual GPIO edges: all 63 bytes in one combined transaction, atomic publish. */
static void
test_gpio_reports(void)
{
	uint8_t buf[65];
	struct fts_gpio_trace timing, base;
	unsigned stage, received, clocks = 0;
	/* Model an event-rounded legacy delay and verify actual local counter spin. */
	fresh(); mock_tick_ns = 100; mock_delay_factor = 5;
	uint64_t start = mach_absolute_time();
	fts_gpio_delay(5);
	assert(mach_absolute_time() - start >= 25000 && mock_delay_calls == 1);
	daisy_busywait = 1;
	start = mach_absolute_time();
	fts_gpio_delay(5);
	uint64_t elapsed = mach_absolute_time() - start;
	assert(elapsed >= 5000 && elapsed < 6000 && mock_delay_calls == 1);
	for (unsigned count = 1; count <= FTS_REPORT_LEN; count++) {
		fresh(); wire_init();
		for (unsigned i = 0; i < sizeof(wire.report); i++) { wire.report[i] = (uint8_t)(0xa5 ^ i); }
		memset(buf, 0xee, sizeof(buf));
		assert(fts_gpio_read_block(0, buf + 1, count, &stage, &received) == 0);
		assert(stage == 7 && received == count && wire.report_reads == 1);
		assert(wire.starts == 2 && wire.stops == 1 && wire.nbytes == 3);
		assert(!memcmp(wire.bytes, "\x70\x00\x71", 3));
		assert(wire.master_acks == count - 1 && wire.master_nacks == 1);
		assert(!memcmp(buf + 1, wire.report, count) && buf[0] == 0xee && buf[count + 1] == 0xee);
		assert(wire.sda && wire.scl && !wire.active && !qup_accesses);
		assert(!wire.irq_reads); /* tracing adds no IRQ samples when disabled */
		clocks = wire.highs - 1;
	}
	/* Byte-indexed IRQ observations, monotonic phases, STOP included in total. */
	fresh(); wire_init(); wire.irq_pattern = true;
	assert(fts_gpio_read_transfer(0, buf, 63, &stage, &received, &base) == 0);
	assert(base.low == 0xaaaaaaaaaaaaaaaaull && wire.irq_reads == 64);
	assert(base.setup < base.header && base.header < base.point0 && base.point0 < base.point1);
	assert(base.point1 < base.data && base.data < base.total && base.gap > 0);
	assert(wire.master_acks == 62 && wire.master_nacks == 1 && wire.starts == 2 && wire.stops == 1);
	/* Simulated scheduling pause within the second record is localized correctly. */
	fresh(); wire_init(); wire.pause_byte = 10; wire.pause_us = 12345;
	assert(fts_gpio_read_transfer(0, buf, 63, &stage, &received, &timing) == 0);
	assert(timing.setup == base.setup && timing.header == base.header && timing.point0 == base.point0);
	assert(timing.point1 == base.point1 + 12345 && timing.data == base.data + 12345);
	assert(timing.total == base.total + 12345 && timing.gap == base.gap + 12345);
	assert(timing.low == ~0ull && !memcmp(buf, wire.report, 63));
	/* Same bytes/ACKs with precise delays; no generic delay call on this path. */
	fresh(); wire_init(); daisy_busywait = 1; mock_tick_ns = 100; mock_delay_factor = 5;
	assert(fts_gpio_read_transfer(0, buf, 63, &stage, &received, &timing) == 0);
	assert(!mock_delay_calls && timing.total >= 6000 && timing.total < 10000);
	assert(!memcmp(buf, wire.report, 63) && wire.master_acks == 62 && wire.master_nacks == 1);
	assert(wire.sda && wire.scl && !wire.active);
	/* Every SCL high phase can time out, including final byte NACK and STOP. */
	for (unsigned fault = 1; fault <= clocks * 2; fault++) {
		fresh(); wire_init();
		wire.fail_clock = (fault - 1) % clocks + 1;
		if (fault > clocks) { daisy_busywait = 1; mock_tick_ns = 100; }
		wire.stretch = FTS_GPIO_STRETCH_US + 1;
		memset(buf, 0xee, sizeof(buf));
		assert(fts_gpio_read_transfer(0, buf + 1, 63, &stage, &received, fault > clocks ? &timing : NULL) == -4);
		for (unsigned i = 0; i < sizeof(buf); i++) { assert(buf[i] == 0xee); }
		assert(wire.sda && wire.scl && received <= 63);
		if (!(fault % clocks)) { assert(stage == 7 && received == 63); }
		if (fault > clocks) { assert(timing.total > 0 && timing.total >= timing.data); }
	}
	for (unsigned byte = 1; byte <= 3; byte++) {
		fresh(); wire_init(); wire.nack_byte = byte;
		memset(buf, 0xee, sizeof(buf));
		assert(fts_gpio_read_block(0, buf + 1, 63, &stage, &received) == -2);
		assert(received == 0 && buf[1] == 0xee && buf[63] == 0xee && wire.sda && wire.scl);
	}
	fresh(); wire_init();
	assert(fts_gpio_read_block(0, buf, 0, &stage, &received) == -1);
	assert(fts_gpio_read_block(0, buf, 64, &stage, &received) == -1);
	assert(fts_gpio_read_block(0, NULL, 63, &stage, &received) == -1);
	assert(fts_gpio_read_block(0, buf, 63, NULL, &received) == -1);
	assert(fts_gpio_read_block(0, buf, 63, &stage, NULL) == -1);
	assert(!wire.starts && !wire.highs);
	/* Finite stretching is accepted near the final byte. */
	wire.fail_clock = clocks - 5; wire.stretch = 4;
	assert(fts_gpio_read_block(0, buf, 63, &stage, &received) == 0 && received == 63);
	/* Capture requires GPIO mode, power success, and successful identity/FW reads. */
	for (unsigned fault = 0; fault < 6; fault++) {
		fresh(); wire_init();
		boot_busywait = boot_trace = boot_daisy = boot_gpio = boot_power = boot_reports = 1;
		mock_tick_ns = 100;
		power_ready = true;
		if (fault == 0) { boot_gpio = 0; }
		if (fault == 1) { boot_power = 0; }
		if (fault == 2) { power_result = -7; }
		if (fault == 3) { power_ready = false; }
		if (fault == 4) { wire.bad_id = true; }
		if (fault == 5) { wire.nack_byte = 7; } /* firmware read address NACK */
		vince_input_start(); started_fn(NULL, 0);
		assert(!wire.report_reads && !qup_accesses);
		if (fault < 4) { assert(!(gpio[GPIO_IO(GPIO_RST) / 4] & 2)); }
	}
	/* Full time-bounded run, including 30/60-second rail and identity checks. */
	fresh(); wire_init();
	boot_busywait = boot_trace = boot_daisy = boot_gpio = boot_power = boot_reports = boot_probe = 1;
	mock_tick_ns = 100; mock_delay_factor = 5;
	power_ready = true; capture_step_ms = 10000;
	vince_input_start(); started_fn(NULL, 0);
	assert(wire.report_reads == 30 && capture_sleeps == 30 && power_calls == 1);
	assert(wire.master_acks == 62 * 30 && wire.master_nacks == 30 + 12);
	assert(!qup_maps && !qup_accesses && wire.sda && wire.scl);
	assert(wire.irq_reads >= 64 * 30);
	assert(mock_delay_calls == observer_reads + 32); /* rail delays plus legacy calibration */
	/* A trace hint alone does not switch identity-only mode into capture. */
	fresh(); wire_init();
	boot_trace = boot_daisy = boot_gpio = boot_power = 1; power_ready = true;
	vince_input_start(); started_fn(NULL, 0);
	assert(!wire.report_reads && wire.irq_reads == 1);
	/* A continuously changing stream hits the raw-frame cap before the time cap. */
	fresh(); wire_init();
	capture_step_ms = 1; capture_change = true;
	gpio[GPIO_IO(GPIO_RST) / 4] = 2;
	fts_gpio_capture();
	assert(wire.report_reads == FTS_CAPTURE_LIMIT && capture_sleeps == FTS_CAPTURE_LIMIT - 1);
	assert(wire.sda && wire.scl);
	/* Bus failure aborts after eight failed frames, publishing no frame bytes. */
	fresh(); wire_init(); wire.absent = true;
	capture_step_ms = 1; gpio[GPIO_IO(GPIO_RST) / 4] = 2;
	fts_gpio_capture();
	assert(wire.nbytes == 8 && !wire.report_reads && capture_sleeps == 7);
	assert(wire.sda && wire.scl);
	/* Identity loss at a scheduled check ends the capture. */
	fresh(); wire_init();
	daisy_probe = 1; capture_step_ms = 10000; capture_bad_id = true;
	gpio[GPIO_IO(GPIO_RST) / 4] = 2;
	fts_gpio_capture();
	assert(wire.report_reads == 4 && capture_sleeps == 3);
	assert(wire.sda && wire.scl && !qup_accesses);
}
