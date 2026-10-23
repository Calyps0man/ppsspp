// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#pragma once
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unistd.h>

inline size_t PS3PositionedRead(int fd, int64_t position, size_t bytes, size_t count,
	void *data, std::mutex &readLock) {
	static_assert(sizeof(off_t) >= 8, "PS3 ISO reads require 64-bit offsets");
	if (!bytes || position < 0 || count > std::numeric_limits<size_t>::max() / bytes)
		return 0;
	std::lock_guard<std::mutex> guard(readLock);
	if (lseek(fd, position, SEEK_SET) < 0) return 0;
	ssize_t result;
	do { result = read(fd, data, bytes * count); } while (result < 0 && errno == EINTR);
	return result < 0 ? 0 : (size_t)result / bytes;
}
