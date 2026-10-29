/* Edge-driven open-drain FT slave for the actual patched GPIO transport. */
static struct {
	bool enabled, sda, scl, slave, clock, active, address, read, ack_clock;
	bool stuck_sda, stuck_scl, absent, master_ack, bad_id;
	unsigned state, bits, value, reg, starts, stops, nbytes, master_nacks, master_acks;
	unsigned highs, fail_clock, stretch, remaining, nack_byte;
	uint8_t bytes[128];
	uint8_t report[FTS_REPORT_LEN];
	unsigned report_reads;
} wire;
enum { W_RX, W_ACK, W_TX, W_NACK, W_IDLE };

static uint8_t
wire_register(unsigned reg)
{
	if (reg < sizeof(wire.report)) { return wire.report[reg]; }
	switch (reg) {
	case 0xa3: return wire.bad_id ? 0 : 0x54;
	case 0x9f: return 0x22;
	case 0xa6: return 0x0c;
	case 0xa8: return 0x51;
	default: return 0xff;
	}
}

static void
wire_clock(void)
{
	bool high = wire.scl && !wire.stuck_scl && !wire.remaining;
	if (high == wire.clock) { return; }
	wire.clock = high;
	if (!wire.active) { return; }
	if (high) {
		switch (wire.state) {
		case W_RX:
			wire.value = (wire.value << 1) | wire.sda;
			wire.bits++;
			break;
		case W_ACK: wire.ack_clock = true; break;
		case W_NACK:
			wire.master_ack = !wire.sda;
			if (wire.sda) { wire.master_nacks++; }
			else { wire.master_acks++; }
			break;
		}
	} else {
		switch (wire.state) {
		case W_RX:
			if (wire.bits != 8) { break; }
			if (wire.nbytes < sizeof(wire.bytes)) { wire.bytes[wire.nbytes] = wire.value; }
			wire.nbytes++;
			if (wire.address) {
				wire.read = wire.value & 1;
				if (wire.read && wire.reg == 0) { wire.report_reads++; }
				wire.address = false;
			} else {
				wire.reg = wire.value;
			}
			wire.slave = wire.absent || wire.nbytes == wire.nack_byte;
			wire.state = W_ACK;
			wire.ack_clock = false;
			break;
		case W_ACK:
			if (!wire.ack_clock) { break; }
			wire.bits = wire.value = 0;
			wire.state = wire.read ? W_TX : W_RX;
			wire.slave = !wire.read || (wire_register(wire.reg) & 0x80);
			break;
		case W_TX:
			if (++wire.bits == 8) {
				wire.state = W_NACK;
				wire.slave = true;
			} else {
				wire.slave = (wire_register(wire.reg) >> (7 - wire.bits)) & 1;
			}
			break;
		case W_NACK:
			if (wire.master_ack) {
				wire.reg++;
				wire.bits = 0;
				wire.state = W_TX;
				wire.slave = (wire_register(wire.reg) & 0x80) != 0;
			} else { wire.state = W_IDLE; }
			break;
		}
	}
}

static void
wire_update(unsigned pin, uint32_t cfg, uint32_t io, bool config_write)
{
	bool released = !(cfg & 0x200);
	assert((cfg & 0x3f) == 0); /* GPIO, no internal pull */
	assert(released || !(io & 2)); /* never drive a bus line high */
	if (pin == GPIO_SCL) {
		wire.scl = released;
		if (config_write && released) {
			if (++wire.highs == wire.fail_clock) { wire.remaining = wire.stretch; }
		} else if (!released) {
			wire.remaining = 0;
		}
		wire_clock();
	} else {
		bool old = wire.sda;
		wire.sda = released;
		if (old != released && wire.clock && wire.slave && !wire.stuck_sda) {
			if (!released) {
				wire.starts++;
				wire.active = wire.address = true;
				wire.read = false;
				wire.state = W_RX;
				wire.bits = wire.value = 0;
			} else {
				wire.stops++;
				wire.active = false;
			}
		}
	}
}

static bool
wire_level(unsigned pin)
{
	if (pin == GPIO_SCL) {
		bool high = wire.clock;
		if (wire.remaining && --wire.remaining == 0) { wire_clock(); }
		return high;
	}
	return wire.sda && wire.slave && !wire.stuck_sda;
}

static void
wire_init(void)
{
	memset(&wire, 0, sizeof(wire));
	wire.enabled = wire.sda = wire.scl = wire.slave = wire.clock = true;
	memset(wire.report, 0xff, sizeof(wire.report));
	memset(wire.report, 0, 3); /* work mode, no gesture, no contacts */
}
