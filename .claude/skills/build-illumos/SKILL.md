---
name: build-illumos
description: Builds and smoke-tests the FoundationDB illumos / SmartOS port via cmake + ninja, then verifies a single-node memory-engine cluster configures and round-trips set/get. Use when compiling fdbserver/fdbcli on illumos, verifying the port still builds after changes to flow/, cmake/, fdbserver/, or any SunOS-guarded code, or when the user asks to "build fdb", "rebuild on the SmartOS box", or "smoke-test the port".
allowed-tools:
  - Bash
  - Read
  - Edit
  - Write
---

# Building FoundationDB on illumos

Canonical "did I break anything?" gate after touching anything under
`flow/`, `cmake/`, `fdbserver/`, or anywhere a `__sun` / `SunOS` guard
exists. Produces working `fdbserver` + `fdbcli` and verifies a
single-node memory-engine cluster round-trips set/get.

For provisioning a fresh host, full operator notes, and the rationale
behind the SunOS CMake guards, see
[`BUILDING-illumos.md`](../../BUILDING-illumos.md). For setting up a
real multi-node cluster after a successful build, see
[`RUNNING-foundationdb.md`](../../RUNNING-foundationdb.md).

## Workflow

Copy this checklist and tick items off as you go:

```
Build progress:
- [ ] 1. Resolve build host (local SunOS vs. cross-dev sync)
- [ ] 2. Confirm pkgsrc deps are installed
- [ ] 3. Run cmake (out-of-tree)
- [ ] 4. Run ninja with explicit target list
- [ ] 5. Run smoke test
- [ ] 6. Report binary sizes; flag regressions
```

## 1. Resolve build host

Run `uname -s`:

- `SunOS` → build in place; skip to step 2.
- Anything else → cross-dev mode. Look for `.illumos-sync.sh` at the
  repo root; it defines `HOST`, `KEY`, `DEST`. Run it to push the
  working tree, then `ssh "$HOST"` and run the rest there. Do not
  work around its `.git` and `build/` exclusions.
- Neither applies → ask the user where to build. Do not assume a
  remote host exists.

## 2. Confirm pkgsrc dependencies

```sh
pkgin install gcc13 cmake ninja-build python312 patch git libexecinfo
```

`gcc13`, `cmake`, `ninja`, `python3` must all be on `PATH`. Do **not**
install `mono` — the actor compiler runs as a Python module on
illumos, and CMake's probe falls back to Python automatically when
`mcs`/`mono` is absent.

## 3. Configure (cmake, out-of-tree)

```sh
SRC_DIR=/path/to/foundationdb
BUILD_DIR=$HOME/fdb-build       # any path with ~25 GB free

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release "$SRC_DIR"
```

No flag overrides are needed; SunOS guards in CMake handle jemalloc,
USDT, GNU-ld linker flags, and actor-compiler selection.

If cmake fails:

- `Could NOT find Python3` → install `python312` and rerun from a
  fresh shell so `PATH` updates.
- `actor compiler not found` → same fix; the C# fallback path is
  intentionally not used on illumos.
- Boost patch fails to apply → confirm
  `cmake/boost-illumos-fallocate-fallback.patch` is unmodified and
  pkgsrc `patch(1)` is on `PATH`.

## 4. Build (ninja)

```sh
ninja fdbserver fdbcli fdbdr backup_agent dr_agent fdbrestore
```

Pass an explicit target list. The default `all` graph still pulls in
`fdbmonitor/` (not yet ported, builds vacuously, wastes time).

Drop to `-j2` or `-j1` on a small VM — heavy TUs peak around 6–8 GB
per parallel job. First build downloads and patches Boost; expect
~10 min before fdb sources start compiling. Subsequent builds skip it.

If ninja fails:

- Linker errors about `kstat_*`, `dladdr1`, `getloadavg`, `sendfilev`
  → a SunOS link-line entry has regressed. Check `flow/CMakeLists.txt`
  for the `kstat execinfo nsl socket elf sendfile` block.
- Compile errors mentioning `index_t` redefinition, `yield`
  ambiguity, `_X` redefinition, or `Traceable<int8_t>` SFINAE → the
  `IllumosPrelude.h` force-include is missing. Verify the SunOS
  prelude wiring in `cmake/ConfigureCompiler.cmake`.

## 5. Smoke test

Minimum acceptance gate. Run from anywhere writable on the build host:

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

Pass criterion: the second `fdbcli` prints `` `hello` is `world` ``.
A hang, error string, or empty result is a regression — investigate,
do not paper over.

If `fdbserver` hangs at startup with no log output, the Boost
shared-memory issue may have regressed. As a **diagnostic only** (not
a fix), set `FDB_SKIP_SHARED_MEMORY_MACHINE_ID=1` in the env before
launching `fdbserver`. If that unblocks it,
`cmake/boost-illumos-fallocate-fallback.patch` did not apply during
the Boost build — wipe `$BUILD_DIR` and rerun cmake from clean.

## 6. Report

After a successful smoke test:

- `ls -lh "$BUILD_DIR/bin/"` — surface the produced binary sizes so
  unexpected shrinks/grows of `fdbserver` are visible. Stripped
  release `fdbserver` is ~150 MB; unstripped is ~1 GB.
- If the user asked to install: `cp` the binaries to a `$PREFIX/bin/`
  of their choosing. There is no `ninja install` rule for illumos.
- Surface non-fatal warnings the user should know about. Ignore
  third-party Boost warnings — they are noise.

## Out of scope

- The `bindingtester` and the deterministic simulator
  (`fdbserver -r simulation`) have not been smoke-tested end-to-end
  on illumos. If asked to run them, surface that limitation rather
  than running them blind.
- `fdbmonitor` — not yet ported (uses kqueue/kevent; needs illumos
  event ports). Do not include it in the build target list.
- Multi-node cluster setup, SMF wrapping, operational recipes — see
  [`RUNNING-foundationdb.md`](../../RUNNING-foundationdb.md).
