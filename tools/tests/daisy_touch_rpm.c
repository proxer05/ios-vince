/* Deterministic RPM peer; bounds-check all MMIO and every outbound vote. */
static uint32_t smem[DR_SIZE / 4], ipc[0x1000 / 4];
static unsigned notifications, packets, waits, ring_writes, channel_writes;
static unsigned fail_open, fail_map, bad_ack, stale_left, error_ldo;
static bool no_ack, full_fifo, delay_reply;
static unsigned delayed;
static uint32_t pending_id;

#define TABLE_OFF 0x3000u
#define INFO_OFF 0x4000u
#define FIFO_OFF 0x5000u
#define CID 7u
#define TX(off) smem[(INFO_OFF + (off)) / 4]
#define RX(off) smem[(INFO_OFF + DR_HALF + (off)) / 4]

static void
item(unsigned id, uint32_t off, uint32_t n)
{
	unsigned at = (208 + 16 * id) / 4;
	smem[at] = 1;
	smem[at + 1] = off;
	smem[at + 2] = n;
	smem[at + 3] = 0;
}

static void
fresh_rpm(void)
{
	memset(smem, 0, sizeof(smem));
	memset(ipc, 0, sizeof(ipc));
	notifications = packets = waits = ring_writes = channel_writes = 0;
	fail_open = fail_map = bad_ack = stale_left = error_ldo = delayed = 0;
	no_ack = full_fifo = delay_reply = false;
	dr_smem = dr_ipc = dr_tx = dr_rx = dr_out = dr_in = 0;
	dr_half = dr_id = 0;
	dr_attempted = false;
	dr_result = -1;
	item(13, TABLE_OFF, 64 * 32);
	item(14 + CID, INFO_OFF, 88);
	item(338 + CID, FIFO_OFF, 1024);
	memcpy((char *)smem + TABLE_OFF, "rpm_requests", sizeof("rpm_requests"));
	smem[(TABLE_OFF + 20) / 4] = CID;
	smem[(TABLE_OFF + 24) / 4] = 0x20f;
	smem[(TABLE_OFF + 28) / 4] = 1;
}

static void
reply(uint32_t id)
{
	uint32_t p[] = {20, 0, 0, 0, 0, DR_REQ, 12, DR_MSGID, 4, id,
	    DR_ERR, 4, 0x646162};
	unsigned words = 10;
	if (error_ldo == packets) { p[0] = 32; p[6] = 24; words = 13; }
	if (bad_ack == 1) { p[0] = 0; }
	if (bad_ack == 2) { p[0] = 260; }
	if (bad_ack == 3) { p[0] = 21; }
	if (bad_ack == 4) { p[5] ^= 1; }
	if (bad_ack == 5) { p[6] = 100; }
	if (bad_ack == 6) { p[8] = 0xfffffffc; }
	if (bad_ack == 7) { p[7] = 0; }
	unsigned used = (RX(DR_HEAD) - RX(DR_TAIL)) & (dr_half - 1);
	assert(dr_half - 4 - used >= words * 4);
	for (unsigned i = 0; i < words; i++) {
		smem[(FIFO_OFF + dr_half + RX(DR_HEAD)) / 4] = p[i];
		RX(DR_HEAD) = (RX(DR_HEAD) + 4) & (dr_half - 1);
	}
	RX(DR_FHEAD) = 1;
	if (id == pending_id) { pending_id = 0; }
}

static void
peer(void)
{
	if (TX(DR_STATE) == 0) {
		RX(DR_STATE) = 0;
		TX(DR_TAIL) = RX(DR_HEAD) = 0;
		return;
	}
	if (TX(DR_STATE) == DR_OPENING) {
		if (fail_open != 1) { RX(DR_STATE) = DR_OPENING; }
		return;
	}
	if (fail_open != 2) { RX(DR_STATE) = DR_OPENED; }
	if (TX(DR_HEAD) != TX(DR_TAIL) && !full_fifo) {
		unsigned available = (TX(DR_HEAD) - TX(DR_TAIL)) & (dr_half - 1);
		assert(available == 72 && TX(DR_FHEAD));
		uint32_t p[18];
		for (unsigned i = 0; i < 18; i++) {
			p[i] = smem[(FIFO_OFF + TX(DR_TAIL)) / 4];
			TX(DR_TAIL) = (TX(DR_TAIL) + 4) & (dr_half - 1);
		}
		packets++;
		assert(p[0] == 52 && !(p[1] | p[2] | p[3] | p[4]));
		assert(p[5] == DR_REQ && p[6] == 44 && p[7] == dr_id && p[8] == 0);
		assert(p[9] == 0x616f646c && p[11] == 24);
		assert(p[12] == 0x6e657773 && p[13] == 4 && p[14] == 1);
		assert(p[15] == 0x7675 && p[16] == 4);
		assert((p[10] == 10 && p[17] == 2850000) || (p[10] == 6 && p[17] == 1800000));
		pending_id = p[7];
		if (no_ack) { return; }
		if (delay_reply) { delayed = 2; return; }
		if (stale_left) { stale_left--; reply(pending_id + 99); }
		else { reply(pending_id); }
	} else if (RX(DR_FTAIL) && pending_id && !no_ack && !delayed) {
		RX(DR_FTAIL) = 0;
		if (stale_left) { stale_left--; reply(pending_id + 99); }
		else if (RX(DR_HEAD) == RX(DR_TAIL)) { reply(pending_id); }
	}
}

static uint32_t *
checked(vm_offset_t base, uint32_t off)
{
	uintptr_t at = base + off;
	assert(!(at & 3));
	assert((at >= (uintptr_t)smem && at <= (uintptr_t)smem + sizeof(smem) - 4) ||
	    (at >= (uintptr_t)ipc && at <= (uintptr_t)ipc + sizeof(ipc) - 4));
	return (uint32_t *)at;
}

static uint32_t
mock_rpm_rd(vm_offset_t base, uint32_t off)
{
	return *checked(base, off);
}

static void
mock_rpm_wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	uint32_t *p = checked(base, off);
	if (base == dr_ipc) {
		assert(off == 8 && value == 1); /* Daisy doorbell, not Pronto bit 17 */
		notifications++;
		*p = value;
		peer();
		return;
	}
	if (base == dr_tx || base == dr_rx) {
		channel_writes++;
		assert(off < DR_HALF);
		if (base == dr_rx) {
			assert(off == DR_TAIL || off == DR_FHEAD || off == DR_FTAIL || off == DR_FSTATE);
		} else { assert(off != DR_TAIL); }
	} else {
		assert(base == dr_out && off < dr_half);
		ring_writes++;
	}
	*p = value;
}

static void
mock_rpm_sleep(void)
{
	assert(++waits <= 700);
	if (delayed && --delayed == 0) { reply(pending_id); }
}

vm_offset_t
ml_io_map(vm_offset_t addr, vm_size_t n)
{
	assert(!(addr & 0xfff) && n && !(n & 0xfff));
	if (addr == (DR_IPC & ~0xfffu)) { return fail_map == 1 ? 0 : (vm_offset_t)ipc; }
	assert(addr >= DR_SMEM && addr - DR_SMEM <= DR_SIZE - n);
	if (fail_map == 2 || (fail_map == 3 && addr == DR_SMEM + FIFO_OFF)) { return 0; }
	return (vm_offset_t)smem + addr - DR_SMEM;
}

int
main(void)
{
	fresh_rpm();
	assert(daisy_touch_power_on() == 0 && packets == 2 && !waits);
	assert(daisy_touch_power_on() == 0 && packets == 2); /* no duplicate votes */
	for (unsigned fault = 0; fault < 15; fault++) {
		fresh_rpm();
		if (fault == 0) { item(13, DR_SIZE - 4, 2048); }
		if (fault == 1) { item(13, TABLE_OFF + 1, 2048); }
		if (fault == 2) { smem[(208 + 16 * 13 + 12) / 4] = 0x90000000; }
		if (fault == 3) { smem[TABLE_OFF / 4] = 0; }
		if (fault == 4) { smem[(TABLE_OFF + 20) / 4] = 64; }
		if (fault == 5) { smem[(TABLE_OFF + 24) / 4] = 0x206; }
		if (fault == 6) { smem[(TABLE_OFF + 28) / 4] = 0; }
		if (fault == 7) { item(14 + CID, INFO_OFF, 40); }
		if (fault == 8) { item(338 + CID, FIFO_OFF, 1000); }
		if (fault == 9) { item(338 + CID, FIFO_OFF, 128); }
		if (fault == 10) { fail_map = 1; }
		if (fault == 11) { fail_map = 2; }
		if (fault == 12) { fail_map = 3; }
		if (fault == 13) { item(13, 0, 2048); }
		if (fault == 14) { item(13, TABLE_OFF, 2047); }
		assert(daisy_touch_power_on() == -1);
		assert(!packets && !notifications && !channel_writes && !ring_writes);
	}
	for (unsigned fault = 1; fault <= 2; fault++) {
		fresh_rpm(); fail_open = fault;
		assert(daisy_touch_power_on() == -2 && !packets && !ring_writes);
		assert(waits <= 200);
	}
	fresh_rpm();
	assert(dr_open() == 0);
	TX(DR_HEAD) = dr_half;
	assert(dr_vote(10, 2850000) == -3 && !ring_writes);
	TX(DR_HEAD) = 1;
	assert(dr_vote(10, 2850000) == -3 && !ring_writes);
	TX(DR_HEAD) = 0; RX(DR_STATE) = 0;
	assert(dr_vote(10, 2850000) == -3 && !ring_writes);
	fresh_rpm();
	assert(dr_open() == 0);
	TX(DR_HEAD) = dr_half - 4; full_fifo = true;
	assert(dr_vote(10, 2850000) == -4 && !ring_writes && TX(DR_MASK) == 1);
	fresh_rpm(); no_ack = true;
	assert(daisy_touch_power_on() == -5 && packets == 1 && waits == 400);
	assert(daisy_touch_power_on() == -5 && packets == 1);
	for (unsigned fault = 1; fault <= 7; fault++) {
		fresh_rpm(); bad_ack = fault;
		assert(daisy_touch_power_on() == -6 && packets == 1);
	}
	for (unsigned rail = 1; rail <= 2; rail++) {
		fresh_rpm(); error_ldo = rail;
		assert(daisy_touch_power_on() == -7 && packets == rail);
	}
	fresh_rpm(); stale_left = 3;
	assert(daisy_touch_power_on() == 0 && packets == 2);
	fresh_rpm(); stale_left = 9;
	assert(daisy_touch_power_on() == -8 && packets == 1);
	fresh_rpm(); delay_reply = true;
	assert(daisy_touch_power_on() == 0 && packets == 2 && waits >= 4);
	/* Wrap both FIFOs across the end; verify pointer math and the full packet. */
	fresh_rpm();
	assert(dr_open() == 0);
	TX(DR_HEAD) = TX(DR_TAIL) = RX(DR_HEAD) = RX(DR_TAIL) = dr_half - 16;
	assert(dr_vote(10, 2850000) == 0 && packets == 1);
	assert(dr_vote(5, 1800000) == -9 && packets == 1);
	assert(dr_vote(10, 3300000) == -9 && packets == 1);
	/* Pure parser: bad lengths, duplicate IDs, truncated records, error replies. */
	uint32_t ack[] = {DR_REQ, 12, DR_MSGID, 4, 42, DR_MSGID, 4, 42};
	for (unsigned bytes = 0; bytes < 20; bytes++) { assert(dr_ack(ack, bytes, 42) == -6); }
	assert(dr_ack(ack, 20, 42) == 0 && dr_ack(ack, 20, 43) == 1);
	ack[1] = 24;
	assert(dr_ack(ack, 32, 42) == -6);
	ack[5] = DR_ERR;
	assert(dr_ack(ack, 32, 42) == -7);
	ack[6] = 5;
	assert(dr_ack(ack, 32, 42) == -6);
	puts("PASS: RPM SMEM bounds, exclusive fields, handshake, whitelisted votes, FIFO wrap/space, ACK ID/length/errors, timeouts, no retries");
	return 0;
}
