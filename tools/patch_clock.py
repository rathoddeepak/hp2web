import sys, pathlib
p = pathlib.Path(sys.argv[1]) / "win32.c"; s = p.read_text()
a = "uint64_t host_us(void){\n"
assert a in s
s = s.replace(a, a + '''    /* HP2: real time. The NFSU2 virtual clock (frame-paced, capped within a frame) starves HP2's
     * busy-wait sleeps (sub_005d8710 waits on EA's timer-driven tick while no frame is drawn).
     * HP2_VCLOCK=1 restores it. */
    { static int vc = -1; if (vc < 0){ const char *e = getenv("HP2_VCLOCK"); vc = (e && *e == '1'); }
      if (!vc) return real_us(); }
''', 1)
p.write_text(s)
