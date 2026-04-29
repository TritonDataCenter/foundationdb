# Building FoundationDB on illumos / SmartOS

This is a working build guide for the illumos port. It captures the exact
environment that has been verified end-to-end so a new contributor can
reproduce it without re-deriving the porting work.

## Status

Builds and runs:

- `fdbserver`
- `fdbcli`
- `fdbdr`
- `backup_agent`
- `dr_agent`
- `fdbrestore`

A single-node memory-engine cluster passes `configure new` and set/get
round-trips through `fdbcli`.

Not yet ported (tracked as follow-ups):

- `fdbmonitor` — uses kqueue/kevent; needs a port to illumos event ports.
- `getDiskStatistics` is stubbed to zero pending a `disk:N:*` kstat aggregator.
- USDT probes are disabled; real DTrace integration via `dtrace -G` is a
  separate piece of work.
- Release binaries are not yet stripped (`fdbserver` is ~1 GB with debug
  info, ~113 MB text).

## Verified environment

The port has been built end-to-end on:

| Component | Version |
| --- | --- |
| OS | SmartOS `20250512T201231Z` |
| Compiler | gcc 13.3.0 (pkgsrc `gcc13`) |
| CMake | 3.31.8 (pkgsrc `cmake`) |
| Ninja | 1.12.1 (pkgsrc `ninja-build`) |
| Python | 3.12.8 (pkgsrc `python312`) |
| Boost | 1.86, built from source by FDB's CMake; patched in-tree |

Other illumos distributions (OmniOS, OpenIndiana, Tribblix) should work — the
porting work targets illumos-gate, not anything SmartOS-specific — but have
not been smoke-tested. Older gcc (≤ 12) has not been tried; the code uses
C++20 features the port assumes are present.

## Provisioning a build host

Anything that can run pkgsrc and has the resources below is sufficient.
For SmartOS specifically, a `base-64-lts` zone (or any image that
includes `/opt/local`) is the path of least resistance.

Sizing:

- **Memory**: 8 GB minimum, 16 GB recommended. Several flow / fdbserver
  translation units peak around 6 GB during compile; OOMs on a 4 GB box
  are common.
- **Disk**: ~5 GB for the source tree, ~10 GB for an out-of-tree build
  with debug info, plus boost build artifacts. Plan for ~25 GB free.
- **CPU**: any. More cores cuts wall-clock time roughly linearly until
  memory becomes the bottleneck.

If your image does not already have `pkgin`, follow the SmartOS / pkgsrc
bootstrap instructions for your distribution; the rest of this guide
assumes `pkgin` is on `PATH`.

## Install build dependencies

```sh
pkgin install \
    gcc13 \
    cmake \
    ninja-build \
    python312 \
    patch \
    git \
    libexecinfo
```

After installing, confirm the toolchain is on your `PATH`:

```sh
cc --version    # should report gcc 13.x
cmake --version # >= 3.24
ninja --version
python3 --version
```

The actor compiler runs as a Python module (`flow/actorcompiler_py/`),
so **mono is not required** on illumos — CMake's actor-compiler probe
automatically falls back to Python when no `mcs`/`mono` is on `PATH`.
(The C# implementation still works if you install `mono`; both produce
equivalent `.cpp` output.)

The remaining system libraries ship with illumos itself and need no
separate package: `libkstat`, `libelf`, `libsendfile`, `libnsl`,
`libsocket`. They appear on the `flow` link line.

## Get the source

```sh
git clone --branch illumos-port https://github.com/<your-fork>/foundationdb.git
cd foundationdb
```

The illumos changes live on the `illumos-port` branch; once they land
upstream, plain `main` will work.

## Configure and build

Use an out-of-tree build directory. Anywhere with enough free space is
fine — the examples below use `$HOME/fdb-build`, but you can substitute
any path, or place `build/` next to the source checkout.

```sh
SRC_DIR=$PWD                       # from inside the checkout
BUILD_DIR=$HOME/fdb-build

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release "$SRC_DIR"
ninja fdbserver fdbcli fdbdr backup_agent dr_agent fdbrestore
```

That's the entire invocation that produces working binaries — no extra
toggles needed. CMake on SunOS automatically:

- selects the Python actor compiler when mono is absent;
- disables jemalloc (`USE_JEMALLOC=OFF`);
- skips the SystemTap USDT probes regardless of `USE_DTRACE`;
- skips the GNU-ld–specific link flags in `bindings/c/`.

Notes:

- `ninja` with no target builds the default `all` graph, which still
  includes `fdbmonitor/` — that subdirectory currently has no real
  binary on illumos, so the target succeeds vacuously. Passing an
  explicit target list is faster and cleaner until the port lands.
- Building uses ~8 GB of memory per parallel job for the heaviest TUs.
  Drop to `ninja -j2` (or `-j1`) on a small build VM.
- `fdbserver` lands at ~1 GB with debug info (text is ~113 MB); strip if
  you're shipping it.

## Cross-development from another host (optional)

If you'd rather edit on your laptop and only build on the illumos box,
`.illumos-sync.sh` at the repo root is a one-liner rsync helper:

```sh
#!/bin/sh
HOST=root@<your-build-host>
KEY=$HOME/.ssh/<your-key>
DEST=/var/tmp/fdb-port/foundationdb
rsync -az --delete -e "ssh -i $KEY" \
    --exclude='.git' --exclude='build/' \
    ./ "$HOST:$DEST/"
```

Edit `HOST`, `KEY`, and `DEST` for your environment. The script
intentionally excludes `.git` (so the remote is a flat working copy) and
`build/` (so local and remote build directories don't trample each
other). Then run the cmake / ninja steps above on the remote host.

### What CMake does for you on SunOS

You should not need to do any of the following manually — they're wired
into the build — but it's useful to know what's happening:

- `cmake/CompileBoost.cmake` applies
  `cmake/boost-illumos-fallocate-fallback.patch` to the bundled Boost
  source. This teaches `boost::interprocess::shared_memory_object::truncate()`
  to tolerate `EINVAL` from `posix_fallocate(3C)` on `shm_open(3C)` fds.
  Without it, the first creator of `/tmp/.SHMD<name>` throws and leaves a
  zero-byte file that subsequent opens spin on forever.
- `cmake/ConfigureCompiler.cmake` skips `-gz` (rejected by illumos
  `ld(1)`), disables jemalloc on SunOS (libumem is the platform allocator),
  and force-includes `flow/IllumosPrelude.h` so libc symbols that collide
  with FDB globals are renamed before any system header sees them.
- `flow/CMakeLists.txt` adds the SunOS link line: `kstat`, `execinfo`,
  `nsl`, `socket`, `elf`, `sendfile`.
- `bindings/c/CMakeLists.txt` skips GNU-ld `--version-script` and
  `-z noexecstack`; the illumos linker uses different mapfile syntax and
  marks stacks NX by default.

## Install (optional)

There is no install rule yet for illumos — `ninja install` does not
produce a usable layout. Copy the binaries to wherever you want them:

```sh
PREFIX=/opt/fdb     # or /usr/local, or anywhere on PATH
mkdir -p "$PREFIX/bin"
cp "$BUILD_DIR/bin"/{fdbserver,fdbcli,fdbdr,backup_agent,dr_agent,fdbrestore} \
   "$PREFIX/bin/"
```

## Smoke test the build

```sh
TEST_DIR=$HOME/fdb-test       # any writable directory
mkdir -p "$TEST_DIR" && cd "$TEST_DIR"
cat > fdb.cluster <<'EOF'
test:test@127.0.0.1:4500
EOF

"$BUILD_DIR/bin/fdbserver" \
    -p 127.0.0.1:4500 \
    -C fdb.cluster \
    -d "$TEST_DIR/data" \
    -L "$TEST_DIR/logs" \
    --memory 1GiB &

"$BUILD_DIR/bin/fdbcli" -C fdb.cluster --exec "configure new memory single"
"$BUILD_DIR/bin/fdbcli" -C fdb.cluster --exec "writemode on; set hello world; get hello"
```

Expected: `configure new` returns success, the second `fdbcli` prints
`hello is world`.

For real multi-node clusters, SMF wrapping, and operational recipes,
see [RUNNING-foundationdb.md](RUNNING-foundationdb.md).

## Operator notes

- **Allocator**: jemalloc is disabled at build time. The process picks up
  libumem at runtime if you `LD_PRELOAD=/usr/lib/64/libumem.so.1` (or set
  `UMEM_OPTIONS`). The default libc allocator works fine for development
  and smoke tests.
- **Last-resort shared memory bypass**: `FDB_SKIP_SHARED_MEMORY_MACHINE_ID=1`
  in `fdbserver`'s environment substitutes a random UID for the
  Boost.Interprocess machine ID. The Boost patch above made this
  unnecessary, but the env var is retained as a one-line operator escape
  hatch if a future Boost upgrade reintroduces a similar issue. Do not
  ship it on by default — it disables the safety check that prevents two
  `fdbserver` instances on the same host from colliding.
- **Time zones**: `BackupAgentBase` and `SpecialKeySpace` carry illumos
  fallbacks because `struct tm` has no `tm_gmtoff` and `strptime(3C)` does
  not honour `%z`. If you see UTC-offset weirdness in backup timestamps,
  start there.

## Where to find the porting changes

Everything illumos-specific is contained in two commits on the
`illumos-port` branch:

- `978706fa7` — `*: initial port to illumos / SmartOS`
- `4e75fab16` — `cmake, fdbserver: patch boost posix_fallocate path for illumos`

`git log --stat 978706fa7^..4e75fab16` is the complete diff against
upstream `main`.
