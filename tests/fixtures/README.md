# /proc/<pid>/maps fixtures for tests/test_discovery.c

The `/proc/<pid>/maps` format has no comment syntax, so fixture provenance
lives here. All fixtures are RECONSTRUCTED layouts (kept minimal), not raw
captures — modeled on the layouts documented in the #24 fix history
(commits a878c72, 9342c1b) and the parsing notes in
`src/discovery.c:pgwt_find_load_base_in_maps()`.

- `maps_el8_nonpie.txt` — Rocky 8 / RHEL 8 PGDG RPM (`/usr/pgsql-13/bin/
  postgres`), non-PIE (ET_EXEC): the binary's FIRST mapping is the `r-xp`
  text segment at the fixed base 0x400000; the `r--p` rodata mapping is far
  above it. This is the layout that broke the old "first r--p" match (#24):
  expected load base 0x400000.
- `maps_el9_pie.txt` — Rocky 9 PGDG RPM (`/usr/pgsql-18/bin/postgres`),
  PIE (ET_DYN): modern 5-mapping split, first mapping is the `r--p`
  ELF-header page. Expected load base 0x55d4c1000000.
- `maps_ubuntu_pie.txt` — Ubuntu PGDG deb (`/usr/lib/postgresql/17/bin/
  postgres`), PIE. Expected load base 0x5560d1a00000.
- `maps_ext_below_binary.txt` — ADVERSARIAL (CAP-4, owned by Phase T4):
  same Ubuntu layout, but `pg_stat_statements.so` is mapped BELOW the main
  binary. Its path `/usr/lib/postgresql/17/lib/...` contains the substring
  "postgres", so the current whole-line `strstr()` match returns the .so's
  base (0x4f2a80000000) instead of the binary's (0x5560d1a00000) — the #24
  class one directory layout away from recurring. test_discovery.c pins the
  current (wrong) behavior as an expected failure until T4 fixes it.
