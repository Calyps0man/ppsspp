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

#pragma once

#include <time.h>

namespace PS3 {

// Match the RTC's existing localtime(0) offset without the nonstandard tm_gmtoff.
// localtime_r includes the libc timezone rules, including daylight saving.
inline bool EpochLocalOffsetSeconds(long *offset) {
	const time_t epoch = 0;
	tm local{};
	if (!localtime_r(&epoch, &local)) {
		return false;
	}
	const long year = local.tm_year + 1900;
	const long previousYear = year - 1;
	const long days = 365 * (year - 1970) + previousYear / 4 - 1969 / 4
		- previousYear / 100 + 1969 / 100 + previousYear / 400 - 1969 / 400 + local.tm_yday;
	*offset = days * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
	return true;
}

}  // namespace PS3
