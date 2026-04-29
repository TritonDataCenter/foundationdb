# FoundationDB — illumos / SmartOS port

This fork carries an in-progress port of FoundationDB to illumos. The
work lives on the `illumos-port` branch; `main` tracks upstream Apple
FoundationDB unchanged.

## Build status

Working: `fdbserver`, `fdbcli`, `fdbdr`, `backup_agent`, `dr_agent`,
`fdbrestore`. Single-node memory-engine cluster passes
`configure new` + set/get round-trip via `fdbcli`.

Not yet ported: `fdbmonitor` (uses kqueue/kevent — needs illumos event
ports). `getDiskStatistics` is stubbed to zero pending a `disk:N:*`
kstat aggregator. USDT probes are off; real DTrace via `dtrace -G` is
follow-up work. Release binaries are not stripped (~1 GB each).

## Build and run

- [BUILDING-illumos.md](BUILDING-illumos.md) — the complete verified
  recipe (provisioning, packages, cmake invocation, smoke test).
- [RUNNING-foundationdb.md](RUNNING-foundationdb.md) — operator guide:
  cluster file mental model, single-node smoke, three-node setup,
  SMF wrapping, gotchas, and operational recipes.

The short build version, run on an illumos host with pkgsrc:

```sh
pkgin install gcc13 cmake ninja-build python312 patch git libexecinfo
mkdir -p $HOME/fdb-build && cd $HOME/fdb-build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release /path/to/foundationdb
ninja fdbserver fdbcli fdbdr backup_agent dr_agent fdbrestore
```

No flag overrides are needed — the SunOS guards in CMake handle
jemalloc, USDT, GNU-ld flags, and actor-compiler selection (Python,
not mono).

If you're cross-developing from another machine, `.illumos-sync.sh`
rsyncs the working tree to a remote build box; edit `HOST` / `KEY` /
`DEST` for your environment.

## Where the porting changes live

Two commits on `illumos-port` cover everything platform-specific:

- `978706fa7` — `*: initial port to illumos / SmartOS`
- `4e75fab16` — `cmake, fdbserver: patch boost posix_fallocate path for illumos`

`git log --stat 978706fa7^..4e75fab16` is the full diff against
upstream. Key files to know:

- `flow/include/flow/IllumosPrelude.h` — force-included on SunOS to
  rename libc symbols that collide with FDB globals (`yield`,
  `index_t`) before any system header sees them.
- `flow/include/flow/IllumosPlatform.h`, `flow/IllumosPlatform.cpp` —
  kstat / procfs / getloadavg helpers. Called from `flow/Platform.cpp`'s
  `__sun` arms.
- `cmake/CompileBoost.cmake` + `cmake/boost-illumos-fallocate-fallback.patch`
  — applied automatically on SunOS. Without this, the first
  `managed_shared_memory` creator throws `EINVAL` from
  `posix_fallocate(3C)` on `shm_open(3C)` fds and leaves a 0-byte
  `/tmp/.SHMD<name>` that subsequent opens spin on forever.
- `fdbrpc/libeio/config.h.SunOS` — autoconf config for the vendored
  libeio; routes through the existing `__solaris` `sendfilev(3EXT)`
  branch.

## Platform conventions

- **Compilers**: pkgsrc `gcc13` (`/opt/local/bin/cc`, `/opt/local/bin/c++`).
  CMake finds them via `PATH`; do not pass `CC`/`CXX` overrides unless
  you're testing a different toolchain.
- **Allocator**: jemalloc disabled at build time. libumem is the
  platform default; `LD_PRELOAD=/usr/lib/64/libumem.so.1` if you want
  it explicitly.
- **Time zones**: illumos `struct tm` has no `tm_gmtoff` and
  `strptime(3C)` doesn't honour `%z`. `BackupAgentBase.cpp` and
  `SpecialKeySpace.cpp` carry fallbacks; if you see UTC-offset bugs in
  backup timestamps, start there.
- **Last-resort shm bypass**: `FDB_SKIP_SHARED_MEMORY_MACHINE_ID=1` on
  `fdbserver` substitutes a random UID for the Boost.Interprocess
  machine ID. The Boost patch made this unnecessary in the normal
  path; do not enable in production — it disables the safety check
  that prevents two `fdbserver` processes on the same host from
  colliding.

## Workflow

- All illumos changes go on `illumos-port`. Keep the diff against
  upstream `main` minimal and gated on `defined(__sun) && defined(__SVR4)`
  (or `CMAKE_SYSTEM_NAME STREQUAL "SunOS"` in CMake) so the rest of the
  tree stays bit-identical for non-illumos platforms.
- When a third porting commit is needed, prefer extending the existing
  IllumosPrelude / IllumosPlatform files over scattering new `__sun`
  arms across the tree.
- When you finish a logical chunk, write tests if any apply, then
  commit. Use `BUILDING-illumos.md`'s smoke test as the minimum
  verification gate before claiming "still works".
