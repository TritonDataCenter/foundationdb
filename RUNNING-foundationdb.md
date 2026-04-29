# Running a FoundationDB cluster

Operator companion to [`BUILDING-illumos.md`](./BUILDING-illumos.md).
That doc covers how to compile FDB on illumos; this one covers how to
stand up a working cluster — single-node smoke, then real multi-node —
and the gotchas that bite when other documentation makes assumptions
yours doesn't share.

Most of this is platform-agnostic; illumos-specific notes are called
out inline. If you've never operated FDB before, read the **mental
model** section first — most surprises come from misunderstanding what
the cluster file actually is.

## Mental model

FoundationDB is one binary (`fdbserver`) that plays multiple roles
(coordinator, storage, log, proxy, master, ...) decided at runtime by
the cluster's configuration, not by separate processes. A "cluster" is
just a set of `fdbserver` processes that all hold the same
**cluster file** — a one-line text file naming the *coordinators*
that bootstrap connectivity:

```
<description>:<id>@<host1>:<port1>,<host2>:<port2>,...
```

- `description` and `id` are **arbitrary strings** chosen at cluster
  init. They're sticky for the life of the cluster — a client that
  connects with the wrong pair gets `database_unavailable`. Common
  convention: pick a meaningful name (`prod`, `mantad`, `dev`) for
  both fields, or generate random IDs with `mktemp -u XXXXXXXX`.
- The `host:port` list is the **coordinator** addresses: a small,
  fixed quorum of fdbservers that arbitrate everything else. Storage
  servers, logs, etc., are *not* in this file — they discover each
  other via the coordinators.
- Every client (`fdbcli`, your application) needs to read the same
  cluster file. They're typically distributed by configuration
  management; the conventional path is `/etc/foundationdb/fdb.cluster`,
  but anywhere works as long as the file is byte-identical across
  consumers.

A cluster is **alive** when a majority of coordinators are reachable.
Add or remove coordinators with `coordinators ...` in `fdbcli`; the
cluster file on every node updates automatically (FDB rewrites it in
place when the coordinator set changes — confusing the first time).

## Single-node cluster (smoke / dev)

The fastest way to verify your `fdbserver` binary actually works.
Memory engine, single replica, one process — not durable across
restarts but excellent for testing client code.

```sh
# Pick a directory you can write to.
TEST_DIR=$HOME/fdb-smoke
BIN_DIR=/opt/fdb/bin            # wherever your binaries live
mkdir -p "$TEST_DIR" && cd "$TEST_DIR"

# 1. Cluster file — `test:test` is fine for development.
cat > fdb.cluster <<'EOF'
test:test@127.0.0.1:4500
EOF

# 2. Start a single fdbserver listening on the cluster-file address.
"$BIN_DIR/fdbserver" \
    -p 127.0.0.1:4500 \
    -C fdb.cluster \
    -d "$TEST_DIR/data" \
    -L "$TEST_DIR/logs" \
    --memory 1GiB &

# 3. Bootstrap. The first `configure new` is what actually creates
#    the cluster — fdbserver waits for it before serving anything.
"$BIN_DIR/fdbcli" -C fdb.cluster --exec "configure new memory single"

# 4. Smoke a write + read.
"$BIN_DIR/fdbcli" -C fdb.cluster --exec "writemode on; set hello world; get hello"
# expected: `hello is world`
```

Variants by `configure new` argument:
- `configure new memory single` — RAM-only, single-replica. Use for
  smoke tests; data vanishes when the process dies.
- `configure new ssd single` — disk-backed, single-replica. Survives
  restart; **never use single-replica in anything that matters** —
  any disk loss is data loss.
- `configure new ssd double` — two replicas, single host. Useful for
  testing replication code paths without provisioning more hosts.

You can shut the cluster down with `kill <fdbserver pid>` — there's
no graceful-stop API on the binary; the process is the cluster.

## Three-node cluster (production-shape minimum)

The minimum FDB recommends for anything important. Three machines
host one coordinator each; storage / log roles run on the same three
processes. Tolerates one node loss.

Per-host setup (do all three):

```sh
# 1. Install fdbserver + fdbcli + libfdb_c.so.
#    On illumos: build per BUILDING-illumos.md, then copy:
PREFIX=/opt/fdb
mkdir -p $PREFIX/bin $PREFIX/lib
cp .../bin/{fdbserver,fdbcli} $PREFIX/bin/
cp .../lib/libfdb_c.so       $PREFIX/lib/

# 2. Pick a stable storage path; create it owned by whatever uid will
#    run fdbserver.
mkdir -p /var/lib/foundationdb/{data,logs}
chown -R fdb:fdb /var/lib/foundationdb     # if you run as a service user

# 3. Drop the cluster file at a stable path. *Same content on every
#    host*. Pick any description/id; the coordinator IPs are the
#    storage-fabric IPs of the three machines:
mkdir -p /etc/foundationdb
cat > /etc/foundationdb/fdb.cluster <<'EOF'
prod:prod@10.0.0.10:4500,10.0.0.11:4500,10.0.0.12:4500
EOF

# 4. Start the local fdbserver pointing at the cluster file with its
#    own listen address (the IP for *this* host).
nohup /opt/fdb/bin/fdbserver \
    -p 10.0.0.10:4500 \
    -C /etc/foundationdb/fdb.cluster \
    -d /var/lib/foundationdb/data \
    -L /var/lib/foundationdb/logs \
    --memory 8GiB \
    >/var/lib/foundationdb/fdbserver.out 2>&1 &
```

Run that on all three hosts. They'll bind their respective `-p`
addresses, all read the same cluster file, and discover each other
via the coordinator entries.

Then exactly once, from any one host:

```sh
fdbcli -C /etc/foundationdb/fdb.cluster --exec "configure new ssd triple"
```

`triple` = three replicas. Combined with three hosts, that gives one
copy on each — survive a host loss without re-replication. Verify:

```sh
fdbcli -C /etc/foundationdb/fdb.cluster --exec "status minimal"
# expected: "The database is available."

fdbcli -C /etc/foundationdb/fdb.cluster --exec "status"
# expected: a multi-page report ending with redundancy mode = triple,
#           replication health = healthy, coordinators = 3
```

When the cluster shows `Healthy`, it's ready.

### SMF service wrapper (illumos)

Don't run `nohup ... &` in production. Wrap fdbserver in SMF so it
restarts on crash and survives reboots. Minimal manifest skeleton:

```xml
<?xml version='1.0'?>
<!DOCTYPE service_bundle SYSTEM '/usr/share/lib/xml/dtd/service_bundle.dtd.1'>
<service_bundle type='manifest' name='foundationdb'>
  <service name='application/foundationdb' type='service' version='0'>
    <create_default_instance enabled='false'/>
    <single_instance/>
    <dependency name='network' grouping='require_all' restart_on='error' type='service'>
      <service_fmri value='svc:/milestone/network:default'/>
    </dependency>
    <exec_method name='start' type='method' timeout_seconds='60'
        exec='/opt/fdb/bin/fdbserver -p $LISTEN_ADDR -C /etc/foundationdb/fdb.cluster -d /var/lib/foundationdb/data -L /var/lib/foundationdb/logs --memory $MEMORY'>
      <method_context>
        <method_credential user='fdb' group='fdb'/>
      </method_context>
    </exec_method>
    <exec_method name='stop' type='method' exec=':kill' timeout_seconds='30'/>
    <property_group name='startd' type='framework'>
      <propval name='duration' type='astring' value='child'/>
    </property_group>
    <property_group name='application' type='application'>
      <propval name='listen_addr' type='astring' value='10.0.0.10:4500'/>
      <propval name='memory' type='astring' value='8GiB'/>
    </property_group>
  </service>
</service_bundle>
```

`svccfg import foundationdb.xml`, then per-host
`svccfg -s svc:/application/foundationdb:default setprop application/listen_addr = "10.0.0.X:4500"` and
`svcadm enable svc:/application/foundationdb:default`. If FDB is the
only application on the host, hard-coding the listen_addr in the
manifest is fine; for multi-tenant infra it's worth the templating.

## Client connection from another host

Clients (anything using libfdb_c, including `fdbcli`) need:

1. The cluster file — *byte-identical* to the one on the servers.
2. `libfdb_c.so` reachable via `LD_LIBRARY_PATH` (or installed at a
   default ld.so search path).
3. **Network reachability to every coordinator on TCP/4500.** This is
   the single most common bring-up failure: ICMP ping works, the TCP
   handshake doesn't, you get `database_unavailable` and waste an
   afternoon. On Triton, this means `triton fwrule create 'FROM tag
   "role" = "<your-client-role>" TO tag "service" = "fdb" ALLOW tcp
   PORT 4500'` — security groups / cloud firewalls require an
   explicit rule.

```sh
LD_LIBRARY_PATH=/opt/fdb/lib \
  /opt/fdb/bin/fdbcli -C /etc/foundationdb/fdb.cluster \
  --exec "status minimal"
```

Or with the `triton-fdb` Rust crate convention:

```sh
TRITON_FDB_CLUSTER_FILE=/etc/foundationdb/fdb.cluster \
LD_LIBRARY_PATH=/opt/fdb/lib \
  cargo run --features fdb ...
```

## Gotchas (things that have cost time on this fleet)

### Stale cluster file at `/var/fdb/fdb.cluster`

Build hosts and old test zones sometimes carry a leftover
`/var/fdb/fdb.cluster` pointing at a defunct local fdbserver, with
the database long gone. Symptom: `fdbcli -C /var/fdb/fdb.cluster`
reports `The database is unavailable`. Fix: write the *real* cluster
file somewhere unambiguous (`/etc/foundationdb/fdb.cluster` is the
convention) and use `-C /etc/foundationdb/fdb.cluster` explicitly.
Don't rely on whatever file is at `/var/fdb/`.

### desc:id must match the live cluster

If your cluster was bootstrapped as `mantad:mantad@...` and you
write a client cluster file as `prod:prod@...`, the client connects
to the coordinators, mismatches the cluster ID, and refuses with
`database_unavailable`. Always copy the cluster file verbatim from a
working server. The desc/id are not auth — there's nothing secret
about them — they're a sanity check that the client is talking to
the cluster it thinks it is.

### `writemode on` is required for destructive fdbcli

Read commands work without it. Anything that mutates (`set`, `clear`,
`clearrange`, `coordinators auto`) needs `writemode on` first in the
same fdbcli session. Heredoc style works:

```sh
fdbcli -C /etc/foundationdb/fdb.cluster <<'EOF'
writemode on
clearrange \x02fs\x00 \x02ft\x00
EOF
```

### Tuple-encoded keys in `clearrange`

If your application uses the FDB tuple layer (the `tuple` package /
`triton_fdb::Subspace`), the on-disk key for `("fs", ...)` starts
with `\x02fs\x00`: byte 0x02 is the tuple-string tag, then UTF-8 of
"fs", then 0x00 terminator. To clear *everything* under that prefix
in fdbcli:

```
clearrange \x02fs\x00 \x02ft\x00
```

(`\x02ft\x00` is the lexically-next prefix.) Use `getrange \x02fs\x00
\x02ft\x00 5` to peek at the first few keys.

This bit me clearing the mantafs metadata after a buggy rebalancer
trashed a test cluster — **always** use a `getrange` with a small
limit before a `clearrange` to confirm the prefix scopes what you
expect.

### Long fdbcli commands hang silently

A `clearrange` that touches a few hundred million keys can take
minutes. fdbcli prints `WARNING: Long delay (Ctrl-C to interrupt)`
*once* and then sits there with no further output. It is making
progress; `Committed (<version>)` lands when it finishes. Don't
Ctrl-C unless you genuinely intend to abandon the operation —
half-applied clearranges are perfectly fine (FDB transactions are
atomic), but you'll have to issue the command again.

### Coordinators move when you `configure`

`coordinators auto` in fdbcli rotates coordinator selection (FDB
tries to spread them across fault domains). When it does, **it
rewrites the cluster file in place on every node it can reach.**
That's the desired behavior — clients pick up the change next time
they read the file — but it surprises people who expect the file to
be operator-managed. If you have config-management dropping
`fdb.cluster` from a template, decide who owns the file: either let
FDB manage it (don't templatize), or pin coordinators with
`coordinators <addr1> <addr2> <addr3>` and accept that auto-rebalance
is off.

### `disk: 0%` on illumos status

`fdbcli --exec "status"` reports `disk used: 0 GB` for storage
servers running on the illumos port. That's the
`getDiskStatistics` stub mentioned in `BUILDING-illumos.md`, not
actual data loss. Until a kstat-based replacement lands, use
`zfs list` and process RSS for capacity tracking.

### `fdbserver` binaries aren't stripped on illumos

A release `fdbserver` weighs ~1 GB on disk. Text section is ~113 MB;
the rest is debug info. `strip(1)` works:

```sh
/opt/local/bin/strip /opt/fdb/bin/fdbserver
```

After strip the binary is ~150 MB. Strip in your packaging step,
not at install time on busy hosts.

### One file, three uses

The cluster file goes in three places that are easy to confuse:

| Use | Path convention | Notes |
|---|---|---|
| `fdbserver -C` flag | `/etc/foundationdb/fdb.cluster` | Each fdbserver reads it on start, and rewrites it if coordinators move |
| `fdbcli -C` flag | same path on the same host, or copied to a client host | Read-only from the client's POV |
| application config | application-specific (e.g., `MasterConfig::fdb_cluster_file` for tritonfs/mantad) | Distribute via cfg management |

When you "update the cluster file", you mean the path the *server* is
using — that's the canonical copy. Clients should re-read after a
coordinator change.

## Operational recipes

### Inspect cluster health

```sh
fdbcli -C $CLUSTER --exec "status minimal"   # one-liner
fdbcli -C $CLUSTER --exec "status"           # full report
fdbcli -C $CLUSTER --exec "status json"      # machine-readable
```

`status` is human-readable but verbose. Pipe `status json` to `jq`
when you want specific fields:

```sh
fdbcli -C $CLUSTER --exec "status json" | \
  jq '.cluster.processes | length'           # process count
fdbcli -C $CLUSTER --exec "status json" | \
  jq '.cluster.workload.bytes.written.hz'    # write throughput
```

### Add or remove coordinators

```sh
fdbcli -C $CLUSTER --exec "coordinators auto"
# or pin explicitly:
fdbcli -C $CLUSTER --exec "coordinators 10.0.0.10:4500 10.0.0.11:4500 10.0.0.13:4500"
```

You almost always want `auto` unless you have a specific reason —
it picks a quorum that's spread across fault domains the cluster
knows about.

### Wipe a database (development only)

There's no fdbcli command for "drop everything and start over"; you
either `clearrange` the whole keyspace or destroy the cluster and
reinit. To clear:

```sh
fdbcli -C $CLUSTER <<'EOF'
writemode on
clearrange "" \xff
EOF
```

`\xff` is the upper bound of the user-visible keyspace; FDB's
internal metadata lives at `\xff\x00` and above and isn't touched.
Be careful — this is irreversible and applies to every keyspace
prefix in the cluster.

To full-reset including config: stop every fdbserver, blow away
their data directories, restart, and `configure new ...` again.

### Inspect a subspace

```sh
fdbcli -C $CLUSTER --exec 'getrange \x02myapp\x00 \x02myapp\xff 20'
```

`getrange BEGIN END LIMIT` — first 20 keys lexicographically between
the bounds. Useful for confirming a prefix is non-empty before you
write more, or checking what cleared after a `clearrange`.

### Force-replace a downed coordinator

If one of three coordinators is permanently dead and you've already
provisioned a replacement at a new address:

```sh
# replace 10.0.0.12 (dead) with 10.0.0.13 (new)
fdbcli -C $CLUSTER --exec "coordinators 10.0.0.10:4500 10.0.0.11:4500 10.0.0.13:4500"
```

The cluster file rewrites in place across all live nodes. The dead
coordinator's address is dropped from the active set; clients
fetching the new file via config management pick up the change.

### Clean shutdown

There isn't one. Send `SIGTERM` to every `fdbserver` process; FDB is
crash-only, transactions are durable on commit, restart is the
recovery path.

## When something is wrong

`fdbcli` `status` is the first stop. It tells you:

- Which processes are reachable.
- Which roles each process is playing.
- Replication health (under-replicated keys = danger).
- Disk and memory usage per process.
- Workload throughput.
- Backup status (if running).

If `status` itself hangs, your `fdbcli` can't talk to a
coordinator — check the cluster file and TCP/4500 reachability with
`nc -z host 4500` from the client host.

If you've imported a non-FDB cluster file by accident, you'll get
"The database is unavailable" and `status` hangs. Confirm by
matching the cluster file content against a known-good server.

## Putting a fresh cluster behind an application

Concrete recipe for adding FDB to a Rust application using the
`triton-fdb` workspace crate:

1. **Build** `fdbserver`, `fdbcli`, and `libfdb_c.so` per
   `BUILDING-illumos.md`.
2. **Stand up the cluster** per the three-node section above.
3. **Distribute the cluster file** to every host that runs the
   application: `/etc/foundationdb/fdb.cluster`. (Templated config
   management is fine — just make sure all consumers see the same
   bytes.)
4. **Distribute `libfdb_c.so`** to those same hosts at a stable path
   (`/opt/fdb/lib/libfdb_c.so` is the convention used by the rest of
   the fleet; the `triton-fdb` crate's startup checks `LD_LIBRARY_PATH`
   so the binary can find it at runtime).
5. **Open the firewall** from app role → fdb service on TCP/4500. On
   Triton, it's a single `triton fwrule create`.
6. **Smoke test** the path before the application starts: from one
   app host, `fdbcli -C /etc/foundationdb/fdb.cluster --exec "status
   minimal"`. If that returns "available", the application's startup
   should succeed.

When the app starts, the `triton-fdb` `FdbClient::open` call reads
the cluster file, dynamically loads `libfdb_c.so`, opens a connection
to the first reachable coordinator, and parks until the cluster is
ready. The first transaction commits when the cluster is healthy; if
the cluster is unavailable, transactions block (they don't fail
synchronously) — set a sensible timeout in your application code or
the runtime budget on every `transact`.
