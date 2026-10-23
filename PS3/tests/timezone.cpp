// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cstdio>
#include <cstdlib>
#include "PS3/TimeZone.h"

int main() {
	struct Test { const char *zone; long seconds; };
	const Test tests[] = {
		{"UTC0", 0}, {"EET-2", 7200}, {"EST5", -18000},
		{"NPT-5:45", 20700}, {"NST3:30", -12600},
		{"EAST-14", 50400}, {"WEST12", -43200},
		{"AEST-10AEDT-11,M10.1.0,M4.1.0", 39600},
	};
	for (const auto &test : tests) {
		if (setenv("TZ", test.zone, 1) != 0) return 1;
		tzset();
		long offset;
		if (!PS3::EpochLocalOffsetSeconds(&offset)) return 1;
		if (offset != test.seconds) {
			std::fprintf(stderr, "%s: got %ld, expected %ld\n", test.zone, offset, test.seconds);
			return 1;
		}
	}
	std::puts("PASS: eight RTC offsets, fractional hours, year boundary and southern DST");
}
