#!/bin/bash
#
# Release build of the FoundationDB illumos binaries. Runs ON the illumos
# builder (invoked over ssh by .github/workflows/illumos-build.yml after the
# workflow rsyncs the source tree into $SRC). Encodes the manual steps from
# BUILDING-illumos.md so CI and a human run the same path.
#
# Inputs (env):
#   SRC        source checkout (default: $PWD)
#   BUILD_DIR  out-of-tree build dir (default: /var/tmp/fdb-port/build)
#   OUT_DIR    staging + tarball dir (default: /var/tmp/fdb-port/out)
#   GIT_SHA    commit being built; stamps the tarball name
#   FDB_VER    override the version string (default: parsed from CMakeLists)
#
# Output: $OUT_DIR/foundationdb-<ver>-<shortsha>-illumos-amd64.tar.gz,
# echoed on the last line of stdout.

set -o errexit
set -o pipefail
set -o nounset

SRC="${SRC:-$PWD}"
BUILD_DIR="${BUILD_DIR:-/var/tmp/fdb-port/build}"
OUT_DIR="${OUT_DIR:-/var/tmp/fdb-port/out}"

# pkgsrc toolchain (gcc13, cmake, ninja, python3.12) + illumos system bins.
export PATH=/opt/local/bin:/opt/local/sbin:/usr/bin:/usr/sbin:/sbin:${PATH:-}

for c in cc cmake ninja python3 strip gtar digest; do
    command -v "$c" >/dev/null 2>&1 || { echo "missing required command: $c" >&2; exit 1; }
done
echo "cc: $(cc --version | head -1)"
echo "cmake: $(cmake --version | head -1)"

FDB_VER="${FDB_VER:-$(awk '/project\(foundationdb/{f=1} f&&/VERSION/{print $2; exit}' "$SRC/CMakeLists.txt")}"
[ -n "$FDB_VER" ] || { echo "could not parse FDB version from CMakeLists.txt" >&2; exit 1; }
SHORT_SHA="$(echo "${GIT_SHA:-unknown}" | cut -c1-9)"

# Compile parallelism: the heaviest flow/fdbserver TUs peak ~8 GB; the
# fdbserver link alone exceeds 10 GB but CMake already serializes linking to
# one job on SunOS, so only compile concurrency needs the memory cap.
ncpu=$(psrinfo -t 2>/dev/null || echo 4)
mem_mb=$(/usr/sbin/prtconf -m 2>/dev/null || echo 8192)
njobs=$(( mem_mb / 8192 )); (( njobs < 1 )) && njobs=1
(( njobs > ncpu )) && njobs=$ncpu
echo "build jobs: $njobs (cpus=$ncpu mem=${mem_mb}MB)"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
# No extra toggles: CMake on SunOS auto-selects the Python actor compiler,
# disables jemalloc, and skips the USDT/GNU-ld-specific bits.
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release "$SRC"
ninja -j "$njobs" fdbserver fdbcli fdbbackup fdb_c

# Stage bin + lib. Strip copies directly: upstream's strip_targets target
# pulls in the not-yet-ported fdbmonitor, so it can't run here. backup_agent
# / dr_agent / fdbdr / fdbrestore are symlinks to the fdbbackup multitool.
STAGE="$OUT_DIR/stage"
rm -rf "$OUT_DIR"
mkdir -p "$STAGE/bin" "$STAGE/lib"

cp "$BUILD_DIR/bin/fdbserver" "$BUILD_DIR/bin/fdbcli" "$BUILD_DIR/bin/fdbbackup" "$STAGE/bin/"
for t in backup_agent dr_agent fdbdr fdbrestore; do ln -sf fdbbackup "$STAGE/bin/$t"; done
for b in fdbserver fdbcli fdbbackup; do strip --strip-debug --strip-unneeded "$STAGE/bin/$b"; done

# libfdb_c.so is what tritond's foundationdb crate links against. Bundle it
# plus the non-system runtime deps (libstdc++/libfmt/libexecinfo from
# pkgsrc/gcc13) so the tarball is self-contained; the consumer sets
# LD_LIBRARY_PATH=<prefix>/lib, matching the deployed /opt/fdb layout.
cp "$BUILD_DIR/lib/libfdb_c.so" "$STAGE/lib/"
for so in $(ldd "$STAGE/bin/fdbserver" "$STAGE/lib/libfdb_c.so" 2>/dev/null | awk '/=>/ {print $3}' | sort -u); do
    case "$so" in
        /opt/local/*|*/gcc13/*) cp -p "$so" "$STAGE/lib/" ;;
    esac
done

# illumos: `digest -a sha256` (no sha256sum); emit the two-column format.
( cd "$STAGE" && for f in bin/* lib/*; do
    [ -L "$f" ] && continue
    echo "$(digest -a sha256 "$f")  $f"
done > SHA256SUMS )

TARBALL="$OUT_DIR/foundationdb-${FDB_VER}-${SHORT_SHA}-illumos-amd64.tar.gz"
# illumos tar has no GNU -C; use pkgsrc gtar from inside the stage dir.
( cd "$STAGE" && gtar czf "$TARBALL" bin lib SHA256SUMS )
rm -rf "$STAGE"

ls -lh "$TARBALL"
echo "$TARBALL"
