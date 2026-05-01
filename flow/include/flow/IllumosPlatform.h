/*
 * IllumosPlatform.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2026 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef FLOW_ILLUMOS_PLATFORM_H
#define FLOW_ILLUMOS_PLATFORM_H
#pragma once

#if defined(__sun) && defined(__SVR4)

#include <cstdint>
#include <string>

namespace illumos {

// Read pr_rssize (KB) from /proc/self/psinfo and return bytes.
uint64_t getResidentMemoryBytes();
// Read pr_size (KB) from /proc/self/psinfo and return bytes.
uint64_t getVirtualMemoryBytes();

// Return total physical RAM in bytes (sysconf).
uint64_t getTotalMemoryBytes();
// Return available (free+cache) memory in bytes via kstat unix:0:system_pages.
// Returns 0 on failure.
uint64_t getAvailableMemoryBytes();

// Aggregate idle/user/nice/system ticks across all CPUs from kstat cpu:N:sys.
// Individual counters are monotonically increasing tick counts (cf. sys/sysinfo.h
// CPU_*).  Returns true on success.
struct CpuTicks {
	uint64_t idle = 0;
	uint64_t user = 0;
	uint64_t nice = 0; // illumos has no `nice` class; kept for schema parity.
	uint64_t system = 0;
	uint64_t iowait = 0; // cpu_ticks_wait (I/O wait)
};
bool readCpuTicks(CpuTicks& out);

// Return the number of online CPUs (sysconf _SC_NPROCESSORS_ONLN).
int32_t getCpuCount();

// Return the machine's 1/5/15 min load averages.  Uses getloadavg(3C).
// Returns true on success.
bool getLoadAvg(double* out3);

// Sum rbytes64/obytes64 across all link:*:* kstats whose `name` matches
// the given interface name (or across all links if ifName is empty).
// Fields left untouched on failure (callers typically preserve prior value).
struct LinkCounters {
	uint64_t bytesSent = 0;
	uint64_t bytesReceived = 0;
};
bool readLinkCounters(const char* ifName, LinkCounters& out);

// Aggregate kstat tcp:0:tcp:outSegs / retransSegs across all zones visible
// in the current zone.  Returns true on success.
struct TcpCounters {
	uint64_t outSegs = 0;
	uint64_t retransSegs = 0;
};
bool readTcpCounters(TcpCounters& out);

// Aggregate KSTAT_TYPE_IO counters across every disk-class kstat instance.
// illumos exposes per-device IO stats as kstat_io_t, which is shaped
// differently from the named-data kstats used elsewhere in this file (the
// data is a single struct, not a name->value map).  We filter by
// ks_class == "disk" so every backend driver is covered: `sd` (SCSI/ATA),
// `nvme` (data queues), `blkdev` (virtio-block on SmartOS zones / KVM),
// `cmdk` (IDE).  Per-directory filtering would require resolving directory
// -> minor device -> kstat instance and is not done here; on a host with
// one disk or pool this system-wide aggregate matches what the Linux
// per-device path returns.
// Times are in nanoseconds (kstat hrtime_t); convert to ms at the call site.
struct DiskIo {
	uint64_t reads = 0; // count
	uint64_t writes = 0; // count
	uint64_t bytesRead = 0;
	uint64_t bytesWritten = 0;
	uint64_t inFlight = 0; // wait + run queue depth
	uint64_t serviceTimeNs = 0; // cumulative kstat rtime
};
bool readDiskIo(DiskIo& out);

} // namespace illumos

#endif // __sun && __SVR4

#endif
