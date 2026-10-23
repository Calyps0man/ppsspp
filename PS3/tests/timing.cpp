// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include <cstdio>
#include "Common/TimeUtil.h"
static int64_t systemMicros = 123456789;
static int yieldCalls;
int64_t sysGetSystemTime() { return systemMicros; }
int sysThreadYield() { ++yieldCalls; return 0; }
int main() {
	TimeInit();
	assert(time_now_raw() == 123456789000ULL);
	assert(time_now_d() == 0.0);
	double unixStart = time_now_unix_utc();
	assert(std::abs(time_to_unix_utc(0) - unixStart) < 0.1);
	Instant instant;
	systemMicros += 1500123;
	assert(instant.ElapsedNanos() == 1500123000);
	assert(std::abs(instant.ElapsedSeconds() - 1.500123) < 1e-10);
	assert(std::abs(time_now_d() - 1.500123) < 1e-10);
	assert(std::abs(from_time_raw_relative(2000000000) - 2.0) < 1e-10);
	assert(std::abs(time_to_unix_utc(2) - unixStart - 2.0) < 0.1);
	yield();
	assert(yieldCalls == 1);
	char text[13];
	GetCurrentTimeFormatted(text);
	assert(text[2] == ':' && text[5] == ':');
	puts("PS3 timing adapter: monotonic units, elapsed time, UTC conversion and yield PASS (host syscall model)");
}
