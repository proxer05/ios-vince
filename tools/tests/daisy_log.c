static void
append(const char *s, unsigned n)
{
	assert(irq_enabled && !console_ring.flush_lock);
	for (unsigned i = 0; i < n; i++) { vince_conlog[(vince_conlog_pos++) & (sizeof(vince_conlog) - 1)] = s[i]; }
}

static void
fresh(void)
{
	memset(&file, 0, sizeof(file)); file.regular = true;
	console_ready = irq_enabled = true; console_suspended = false;
	kernel_debugger_entry_count = console_ring.flush_lock = 0;
	vince_conlog_pos = 0; dl_started = false;
	locks = opens = closes = reads = writes = syncs = truncates = sleeps = starts = terminates = 0;
	open_error = read_error = write_error = sync_error = attr_error = thread_error = 0;
	short_write = false; arg_enabled = arg_cpus = arg_rw = real_ncpus = 1;
}

static void
mock_sleep(unsigned ms)
{
	io_context(); assert(ms == 1000 || ms == 250); assert(++sleeps <= 2);
	if (sleeps == 1) { append("later RPM reply\n", 16); }
	else { sync_error = EIO; }
}

int
main(void)
{
	char buf[64]; uint64_t cursor, lost; off_t offset; bool caught; vnode_t vp;
	/* Off by default and no disk/thread activity under invalid selectors. */
	for (unsigned fault = 0; fault < 5; fault++) {
		fresh();
		if (fault == 0) { arg_enabled = 0; }
		if (fault == 1) { arg_enabled = 2; }
		if (fault == 2) { arg_cpus = 0; }
		if (fault == 3) { real_ncpus = 2; }
		if (fault == 4) { arg_rw = 0; }
		daisy_log_start(); assert(!starts && !opens && !writes);
	}
	fresh(); daisy_log_start(); daisy_log_start(); assert(starts == 1 && !opens);
	fresh(); thread_error = 5; daisy_log_start(); assert(starts == 1 && !opens);
	/* Early replay, no duplicates, wrap across the physical end of the RAM ring. */
	fresh(); append("early\n", 6); cursor = 0;
	assert(daisy_console_read(&cursor, buf, sizeof(buf), &lost) == 6 && !lost);
	assert(!memcmp(buf, "early\n", 6) && cursor == 6 && locks == 1);
	assert(daisy_console_read(&cursor, buf, sizeof(buf), &lost) == 0);
	cursor = vince_conlog_pos = sizeof(vince_conlog) - 3; append("wrapped\n", 8);
	assert(daisy_console_read(&cursor, buf, sizeof(buf), &lost) == 8 && !lost && !memcmp(buf, "wrapped\n", 8));
	/* A RAM overrun reports the exact skipped interval; no out-of-bounds read. */
	fresh(); memset(vince_conlog, 'x', sizeof(vince_conlog)); vince_conlog_pos = sizeof(vince_conlog) + 99;
	cursor = 0;
	assert(daisy_console_read(&cursor, buf, sizeof(buf), &lost) == sizeof(buf) && lost == 99 && cursor == 99 + sizeof(buf));
	assert(buf[0] == 'x');
	cursor = vince_conlog_pos + 1;
	assert(daisy_console_read(&cursor, buf, sizeof(buf), &lost) == -1);
	console_suspended = true; assert(daisy_console_read(&cursor, buf, sizeof(buf), &lost) == 0);
	assert(irq_enabled && !console_ring.flush_lock);
	/* Open refuses unrelated files, symlinks, nonregular nodes, and unwritable roots. */
	fresh(); memcpy(file.data, "user data", 9); file.size = 9;
	assert(dl_open(&vp, &file) == EEXIST && !truncates && !writes && closes == 1);
	assert(file.size == 9 && !memcmp(file.data, "user data", 9));
	fresh(); file.regular = false;
	assert(dl_open(&vp, &file) == EINVAL && !truncates && closes == 1);
	for (unsigned i = 0; i < 3; i++) {
		fresh(); open_error = i == 0 ? ELOOP : (i == 1 ? EROFS : ENOENT);
		assert(dl_open(&vp, &file) == open_error && !truncates && !closes);
	}
	fresh(); memcpy(file.data, DL_MAGIC "old boot", sizeof(DL_MAGIC) + 7); file.size = sizeof(DL_MAGIC) + 7;
	assert(dl_open(&vp, &file) == 0 && truncates == 1 && file.size == sizeof(DL_MAGIC) - 1 && syncs == 1);
	/* Short and failed writes do not commit the console cursor or retry. */
	for (unsigned i = 0; i < 2; i++) {
		fresh(); assert(dl_open(&vp, &file) == 0); offset = file.size; cursor = 0;
		append("one log line\n", 13); short_write = i == 0; write_error = i ? ENOSPC : 0;
		assert(dl_drain(vp, &cursor, &offset, &caught, &file) == (i ? ENOSPC : EIO));
		assert(cursor == 0 && offset == sizeof(DL_MAGIC) - 1 && writes == 2);
	}
	fresh(); sync_error = EIO; assert(dl_open(&vp, &file) == EIO && closes == 1);
	fresh(); attr_error = EROFS; assert(dl_open(&vp, &file) == EROFS && !writes && closes == 1);
	/* Gap marker reaches the file; each drain is bounded to a one-MiB budget. */
	fresh(); assert(dl_open(&vp, &file) == 0); offset = file.size; cursor = 0;
	memset(vince_conlog, 'x', sizeof(vince_conlog)); vince_conlog_pos = sizeof(vince_conlog) + 99;
	assert(dl_drain(vp, &cursor, &offset, &caught, &file) == 0 && !caught);
	assert(cursor == 99 + DL_BATCH);
	assert(strstr((char *)file.data, "lost 99 console bytes"));
	/* File limit reserves a terminal marker, even when the next read has a gap. */
	fresh(); offset = DL_LIMIT - DL_RESERVE - 8; cursor = 0;
	memset(vince_conlog, 'x', sizeof(vince_conlog)); vince_conlog_pos = sizeof(vince_conlog) + 1;
	assert(dl_drain(&file, &cursor, &offset, &caught, &file) == EFBIG && !cursor && offset < DL_LIMIT);
	offset = DL_LIMIT; assert(dl_write(&file, &offset, "x", 1, &file) == EFBIG);
	/* Real worker replays early bytes, follows later bytes, flushes and closes on error. */
	fresh(); append("early RPM layout\n", 17); dl_thread(NULL, 0);
	assert(terminates == 1 && closes == 1 && syncs >= 3 && sleeps == 2);
	const char expected[] = DL_MAGIC "early RPM layout\nlater RPM reply\n";
	assert(file.size == sizeof(expected) - 1 && !memcmp(file.data, expected, sizeof(expected) - 1));
	puts("PASS: console replay/wrap/loss, opt-in gating, VFS isolation, file guards, short I/O, flush/size bounds");
	return 0;
}
