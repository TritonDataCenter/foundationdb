---
name: build-illumos
version: 1.0.0
description: |
  Build, install, and smoke-test the FoundationDB illumos / SmartOS port.
  Use when the user asks to "build fdb", "compile foundationdb on illumos",
  "rebuild on the smartos box", "smoke test the port", "verify the build
  still works", or any variant of "does it still compile". Drives the
  cmake + ninja flow documented in BUILDING-illumos.md and runs the
  single-node memory-engine smoke test.
allowed-tools:
  - Bash
  - Read
  - Edit
  - Write
---

# Building FoundationDB on illumos

This skill produces a working `fdbserver` + `fdbcli` from the current
working tree and verifies that a single-node memory-engine cluster can
configure and round-trip set/get. It is the canonical "did I break
anything?" gate after touching anything under `flow/`, `cmake/`,
`fdbserver/`, or anywhere a SunOS guard exists.

## Preflight

The user's environment determines whether you build locally or on a
remote illumos host:

1. Check `uname -s`. If `SunOS`, build in place.
2. If anything else (e.g. `Darwin`, `Linux`), look for `.illumos-sync.sh`
   at the repo root. That script defines the remote `HOST`, `KEY`, and
   `DEST` for the cross-dev workflow. Run it to push the working tree,
   then `ssh` into `$HOST` to run cmake/ninja there.
3. If neither applies, ask the user where they want to build before
   proceeding — do not assume a remote host exists.

The remote sync script intentionally excludes `.git` and `build/`. Do
not work around those exclusions; they exist so local edits and remote
build artifacts don't trample each other.

## Required packages (one-time, on the build host)

```sh
pkgin install gcc13 cmake ninja-build python312 patch git libexecinfo
```

Do **not** install `mono` — the actor compiler runs as a Python module
on illumos. CMake's probe falls back to Python automatically when no
`mcs`/`mono` is on `PATH`.

## Build

Always use an out-of-tree build directory. Pick one once and stick with
it across runs so ninja's incremental graph stays warm.

```sh
SRC_DIR=/path/to/foundationdb        # the checkout
BUILD_DIR=$HOME/fdb-build            # anywhere with ~25 GB free

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release "$SRC_DIR"
ninja fdbserver fdbcli fdbdr backup_agent dr_agent fdbrestore
```

Notes:

- Pass an explicit target list. `ninja` with no target builds the
  default `all` graph, which still includes `fdbmonitor/` — that
  subdirectory has no real binary on illumos yet, but pulling it in
  wastes time.
- Building uses ~6–8 GB per parallel job for the heaviest TUs. On a
  small VM, drop to `ninja -j2` or `-j1`.
- The first build downloads and patches Boost; expect ~10 minutes of
  Boost compile before fdb sources start. Subsequent builds skip this.

If cmake fails:

- `Could NOT find Python3` — install `python312` via pkgin and rerun
  cmake from a fresh shell so `PATH` is picked up.
- `actor compiler not found` — same, but for `python3.12`. The
  C# fallback path is intentionally not used on illumos.
- Boost patch fails to apply — check that
  `cmake/boost-illumos-fallocate-fallback.patch` is unmodified and that
  `patch(1)` from pkgsrc is on `PATH`.

If ninja fails:

- Linker errors about `kstat_*`, `dladdr1`, `getloadavg`, `sendfilev`
  → a SunOS link-line entry has regressed. Check
  `flow/CMakeLists.txt` for the `kstat execinfo nsl socket elf sendfile`
  block.
- Compile errors mentioning `index_t` redefinition, `yield` ambiguity,
  `_X` redefinition, or `Traceable<int8_t>` SFINAE → the
  `IllumosPrelude.h` force-include is missing or the offending file is
  not seeing it. Verify `cmake/ConfigureCompiler.cmake` still has the
  SunOS prelude wiring.

## Smoke test

The minimum acceptance gate. Run from anywhere writable on the build
host:

```sh
TEST_DIR=$HOME/fdb-test
rm -rf "$TEST_DIR" && mkdir -p "$TEST_DIR" && cd "$TEST_DIR"
cat > fdb.cluster <<'EOF'
test:test@127.0.0.1:4500
EOF

"$BUILD_DIR/bin/fdbserver" \
    -p 127.0.0.1:4500 \
    -C fdb.cluster \
    -d "$TEST_DIR/data" \
    -L "$TEST_DIR/logs" \
    --memory 1GiB &
FDB_PID=$!
sleep 3

"$BUILD_DIR/bin/fdbcli" -C fdb.cluster --exec "configure new memory single"
"$BUILD_DIR/bin/fdbcli" -C fdb.cluster --exec "writemode on; set hello world; get hello"

kill "$FDB_PID"
```

Pass criterion: the second `fdbcli` prints `\`hello\` is \`world\``.
Anything else — a hang, an error string, an empty result — is a
regression to investigate, not paper over.

If `fdbserver` hangs at startup with no log output, the Boost
shared-memory issue may have regressed. As a last-resort diagnostic
(not a fix), set `FDB_SKIP_SHARED_MEMORY_MACHINE_ID=1` in the env
before launching `fdbserver`. If that unblocks it, the
`cmake/boost-illumos-fallocate-fallback.patch` did not apply during
the Boost build — rerun cmake from a clean `$BUILD_DIR`.

## After a successful build

- Report the produced binaries' sizes (`ls -lh "$BUILD_DIR/bin/"`) so
  the user can spot if `fdbserver` shrank/grew unexpectedly.
- If the user asked to install: copy to a `$PREFIX/bin/` of their
  choosing. There is no `ninja install` rule for illumos yet; do not
  attempt it.
- Mention any non-fatal warnings the user should know about (deprecated
  symbols, etc.), but do not flag third-party Boost warnings — they
  are noise.

## Out of scope

- The `bindingtester` and the simulator (`fdbserver -r simulation`)
  have not been smoke-tested end-to-end on illumos. If the user asks
  for those, surface that limitation rather than running them blind.
- `fdbmonitor` — not yet ported. Skip the target. Make sure they know

## References

- [BUILDING-illumos.md](../../BUILDING-illumos.md) — full build guide
  with provisioning, package list, and operator notes.
- [CLAUDE.md](../../CLAUDE.md) — project context, status, and
  conventions for the port.
