/*
 * IllumosPlatform.cpp
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

#include "flow/IllumosPlatform.h"

#if defined(__sun) && defined(__SVR4)

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <kstat.h>
#include <procfs.h>
#include <sys/loadavg.h>
#include <sys/time.h>
#include <unistd.h>

namespace illumos {

namespace {

// Read a structured psinfo_t snapshot of the caller. Returns true on success.
bool readPsinfo(psinfo_t& out) {
	int fd = ::open("/proc/self/psinfo", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	ssize_t n = ::read(fd, &out, sizeof(out));
	::close(fd);
	return n == static_cast<ssize_t>(sizeof(out));
}

// RAII wrapper for a kstat_ctl handle so we always close on scope exit.
struct KstatCtl {
	kstat_ctl_t* ctl = nullptr;
	KstatCtl() : ctl(::kstat_open()) {}
	~KstatCtl() {
		if (ctl) ::kstat_close(ctl);
	}
	KstatCtl(const KstatCtl&) = delete;
	KstatCtl& operator=(const KstatCtl&) = delete;
	explicit operator bool() const { return ctl != nullptr; }
};

// Look up a named kstat named-data value.
// Returns the uint64 value on success (all numeric fields coerced), else 0.
bool readNamedU64(kstat_ctl_t* ctl,
                  const char* module,
                  int instance,
                  const char* name,
                  const char* stat,
                  uint64_t& out) {
	kstat_t* ks = ::kstat_lookup(ctl, const_cast<char*>(module), instance, const_cast<char*>(name));
	if (!ks || ks->ks_type != KSTAT_TYPE_NAMED) {
		return false;
	}
	if (::kstat_read(ctl, ks, nullptr) == -1) {
		return false;
	}
	kstat_named_t* kn = static_cast<kstat_named_t*>(::kstat_data_lookup(ks, const_cast<char*>(stat)));
	if (!kn) {
		return false;
	}
	switch (kn->data_type) {
	case KSTAT_DATA_INT32: out = static_cast<uint64_t>(kn->value.i32); return true;
	case KSTAT_DATA_UINT32: out = kn->value.ui32; return true;
	case KSTAT_DATA_INT64: out = static_cast<uint64_t>(kn->value.i64); return true;
	case KSTAT_DATA_UINT64: out = kn->value.ui64; return true;
	default: return false;
	}
}

} // namespace

uint64_t getResidentMemoryBytes() {
	psinfo_t pi;
	if (!readPsinfo(pi)) return 0;
	// pr_rssize is in KB per proc(4).
	return static_cast<uint64_t>(pi.pr_rssize) * 1024ull;
}

uint64_t getVirtualMemoryBytes() {
	psinfo_t pi;
	if (!readPsinfo(pi)) return 0;
	return static_cast<uint64_t>(pi.pr_size) * 1024ull;
}

uint64_t getTotalMemoryBytes() {
	long pages = ::sysconf(_SC_PHYS_PAGES);
	long psize = ::sysconf(_SC_PAGESIZE);
	if (pages <= 0 || psize <= 0) return 0;
	return static_cast<uint64_t>(pages) * static_cast<uint64_t>(psize);
}

uint64_t getAvailableMemoryBytes() {
	KstatCtl k;
	if (!k) return 0;
	uint64_t free_pages = 0;
	if (!readNamedU64(k.ctl, "unix", 0, "system_pages", "freemem", free_pages)) {
		return 0;
	}
	long psize = ::sysconf(_SC_PAGESIZE);
	if (psize <= 0) return 0;
	return free_pages * static_cast<uint64_t>(psize);
}

bool readCpuTicks(CpuTicks& out) {
	KstatCtl k;
	if (!k) return false;

	out = CpuTicks{};
	bool any = false;
	// Walk every cpu:N:sys record.
	for (kstat_t* ks = k.ctl->kc_chain; ks != nullptr; ks = ks->ks_next) {
		if (std::strcmp(ks->ks_module, "cpu") != 0) continue;
		if (std::strcmp(ks->ks_name, "sys") != 0) continue;
		if (ks->ks_type != KSTAT_TYPE_NAMED) continue;
		if (::kstat_read(k.ctl, ks, nullptr) == -1) continue;

		auto fetch = [&](const char* name) -> uint64_t {
			kstat_named_t* kn = static_cast<kstat_named_t*>(::kstat_data_lookup(ks, const_cast<char*>(name)));
			if (!kn) return 0;
			switch (kn->data_type) {
			case KSTAT_DATA_UINT64: return kn->value.ui64;
			case KSTAT_DATA_UINT32: return kn->value.ui32;
			default: return 0;
			}
		};
		// On illumos cpu:N:sys, per-CPU microstate ticks live under cpu_ticks_*.
		// See uts/common/sys/sysinfo.h and cmd/stat/mpstat/mpstat.c in illumos-gate.
		out.idle += fetch("cpu_ticks_idle");
		out.user += fetch("cpu_ticks_user");
		out.system += fetch("cpu_ticks_kernel");
		out.iowait += fetch("cpu_ticks_wait");
		// illumos has no `nice' class; leave out.nice at 0 for schema parity.
		any = true;
	}
	return any;
}

int32_t getCpuCount() {
	long n = ::sysconf(_SC_NPROCESSORS_ONLN);
	if (n <= 0) return 1;
	return static_cast<int32_t>(n);
}

bool getLoadAvg(double* out3) {
	if (!out3) return false;
	double tmp[3] = { 0, 0, 0 };
	if (::getloadavg(tmp, 3) != 3) {
		return false;
	}
	out3[0] = tmp[0];
	out3[1] = tmp[1];
	out3[2] = tmp[2];
	return true;
}

bool readLinkCounters(const char* ifName, LinkCounters& out) {
	KstatCtl k;
	if (!k) return false;

	out = LinkCounters{};
	bool any = false;
	for (kstat_t* ks = k.ctl->kc_chain; ks != nullptr; ks = ks->ks_next) {
		if (std::strcmp(ks->ks_module, "link") != 0) continue;
		if (ks->ks_type != KSTAT_TYPE_NAMED) continue;
		if (ifName && *ifName && std::strcmp(ks->ks_name, ifName) != 0) continue;
		if (::kstat_read(k.ctl, ks, nullptr) == -1) continue;

		auto fetch = [&](const char* name) -> uint64_t {
			kstat_named_t* kn = static_cast<kstat_named_t*>(::kstat_data_lookup(ks, const_cast<char*>(name)));
			if (!kn) return 0;
			if (kn->data_type == KSTAT_DATA_UINT64) return kn->value.ui64;
			if (kn->data_type == KSTAT_DATA_UINT32) return kn->value.ui32;
			return 0;
		};
		out.bytesSent += fetch("obytes64");
		out.bytesReceived += fetch("rbytes64");
		any = true;
	}
	return any;
}

bool readTcpCounters(TcpCounters& out) {
	KstatCtl k;
	if (!k) return false;

	out = TcpCounters{};
	// tcp:0:tcp (numeric names, namespaced under "tcp" module) exposes SNMP
	// style counters on illumos.  See mib2_tcp in <inet/mib2.h>.
	uint64_t outSegs = 0, retrans = 0;
	bool ok1 = readNamedU64(k.ctl, "tcp", 0, "tcp", "outSegs", outSegs);
	bool ok2 = readNamedU64(k.ctl, "tcp", 0, "tcp", "retransSegs", retrans);
	if (!ok1 && !ok2) return false;
	out.outSegs = outSegs;
	out.retransSegs = retrans;
	return true;
}

} // namespace illumos

#endif // __sun && __SVR4
