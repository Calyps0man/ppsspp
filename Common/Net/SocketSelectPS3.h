// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <vector>

// PSL1GHT's FD_SET neither removes SOCKET_FD_MASK nor uses 32-bit mask widths
// consistently on the 64-bit PPU. Translate sets through the real poll wrapper.
struct PS3SocketFdSet {
	std::array<uint32_t, 32> words{};
	bool invalid = false;
};

inline int PS3SocketIndex(int fd) {
	return fd < 0 ? -1 : fd & ~SOCKET_FD_MASK;
}

inline void PS3SocketFdZero(PS3SocketFdSet *set) { *set = {}; }
inline void PS3SocketFdSetBit(int fd, PS3SocketFdSet *set) {
	int index = PS3SocketIndex(fd);
	if (index < 0 || index >= 1024) { set->invalid = true; return; }
	set->words[index / 32] |= uint32_t{1} << (index % 32);
}
inline void PS3SocketFdClear(int fd, PS3SocketFdSet *set) {
	int index = PS3SocketIndex(fd);
	if (index >= 0 && index < 1024) set->words[index / 32] &= ~(uint32_t{1} << (index % 32));
}
inline bool PS3SocketFdIsSet(int fd, const PS3SocketFdSet *set) {
	int index = PS3SocketIndex(fd);
	return set && index >= 0 && index < 1024 && (set->words[index / 32] & (uint32_t{1} << (index % 32)));
}

inline int PS3SocketSelect(int nfds, PS3SocketFdSet *readSet, PS3SocketFdSet *writeSet,
	PS3SocketFdSet *exceptSet, timeval *timeout) {
	int limit = PS3SocketIndex(nfds);
	if (limit < 0 || limit > 1024) { errno = EINVAL; return -1; }
	for (const auto *set : {readSet, writeSet, exceptSet}) {
		if (set && set->invalid) { errno = EBADF; return -1; }
	}
	int milliseconds = -1;
	if (timeout) {
		if (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000
			|| timeout->tv_sec > INT_MAX / 1000) { errno = EINVAL; return -1; }
		int64_t value = int64_t(timeout->tv_sec) * 1000 + (timeout->tv_usec + 999) / 1000;
		if (value > INT_MAX) { errno = EINVAL; return -1; }
		milliseconds = static_cast<int>(value);
	}
	std::vector<pollfd> descriptors;
	for (int index = 0; index < limit; ++index) {
		short events = 0;
		if (PS3SocketFdIsSet(index, readSet)) events |= POLLIN;
		if (PS3SocketFdIsSet(index, writeSet)) events |= POLLOUT;
		if (PS3SocketFdIsSet(index, exceptSet)) events |= POLLPRI;
		if (events) descriptors.push_back({index | SOCKET_FD_MASK, events, 0});
	}
	int result = poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), milliseconds);
	if (result < 0) return -1;
	for (const auto &descriptor : descriptors) {
		if (descriptor.revents & POLLNVAL) { errno = EBADF; return -1; }
	}
	for (auto *set : {readSet, writeSet, exceptSet}) if (set) PS3SocketFdZero(set);
	int ready = 0;
	for (const auto &descriptor : descriptors) {
		if (readSet && (descriptor.events & POLLIN) && (descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
			PS3SocketFdSetBit(descriptor.fd, readSet); ++ready;
		}
		if (writeSet && (descriptor.events & POLLOUT) && (descriptor.revents & (POLLOUT | POLLHUP | POLLERR))) {
			PS3SocketFdSetBit(descriptor.fd, writeSet); ++ready;
		}
		if (exceptSet && (descriptor.events & POLLPRI) && (descriptor.revents & POLLPRI)) {
			PS3SocketFdSetBit(descriptor.fd, exceptSet); ++ready;
		}
	}
	return ready;
}

inline unsigned long PS3SocketReceiveQueue(int socket) {
	netSocketInfo info{};
	int index = PS3SocketIndex(socket);
	if (index < 0 || netGetSockInfo(index, &info, 1) < 0 || info.recv_queue_len < 0) return 0;
	return static_cast<unsigned long>(info.recv_queue_len);
}
