// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#include "ext/libzip/zipint.h"
#include <lv2/system.h>
#include <stdlib.h>

bool zip_secure_random(zip_uint8_t *buffer, zip_uint16_t length) {
	if (!buffer && length) return false;
	while (length) {
		u64 chunk = length > RANDOM_NUMBER_MAX_SIZE ? RANDOM_NUMBER_MAX_SIZE : length;
		if (sysGetRandomNumber(buffer, chunk) != 0) return false;
		buffer += chunk;
		length -= chunk;
	}
	return true;
}

zip_uint32_t zip_random_uint32(void) {
	zip_uint32_t value;
	if (zip_secure_random((zip_uint8_t *)&value, sizeof(value))) return value;
	// Only the non-cryptographic API falls back. Do not claim secure bytes on
	// syscall failure. rand_r avoids mutating the application's rand() state.
	static unsigned sequence;
	unsigned seed = (unsigned)time(NULL) ^ __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED);
	return (zip_uint32_t)rand_r(&seed);
}
