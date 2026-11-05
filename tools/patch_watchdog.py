import sys, pathlib
p = pathlib.Path(sys.argv[1]) / "watchdog.c"; s = p.read_text()
a = '    fprintf(stderr, "[watchdog] end of report\\n\\n");'
assert a in s
p.write_text(s.replace(a, '    { extern void hp2_ring_dump(void); hp2_ring_dump(); }\n' + a, 1))
