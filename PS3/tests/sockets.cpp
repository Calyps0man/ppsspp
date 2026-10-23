// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
// Tests the production PS3 socket branch with controlled API results.
// BSD address layouts model PSL1GHT; native compilation checks its headers.
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <limits>
#include "Common/Net/SocketCompat.h"
#include "Common/File/FileDescriptor.h"

namespace {
constexpr int socketHandle = SOCKET_FD_MASK | 37;
int pollResult = 1;
short returnedEvents = POLLIN;
int observedTimeout = -1;
short observedEvents = 0;
int pollCalls = 0;
int nonblockingValue = -1;
bool failOption = false;
bool failAddress = false;
}

extern "C" int poll(pollfd *fds, nfds_t count, int timeout) {
	assert(count == 1 && fds[0].fd == socketHandle);
	++pollCalls;
	observedTimeout = timeout;
	observedEvents = fds[0].events;
	fds[0].revents = returnedEvents;
	return pollResult;
}

extern "C" int setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
	assert(fd == socketHandle && level == SOL_SOCKET && option == SO_NBIO);
	assert(length == sizeof(int));
	nonblockingValue = *static_cast<const int *>(value);
	errno = failOption ? EINVAL : 0;
	return failOption ? -1 : 0;
}

extern "C" int getsockname(int fd, sockaddr *address, socklen_t *length) {
	assert(fd == socketHandle && *length >= sizeof(sockaddr_in));
	if (failAddress) return -1;
	sockaddr_in result{};
	result.sin_family = AF_INET;
	result.sin_port = htons(1234);
	assert(inet_pton(AF_INET, "192.0.2.5", &result.sin_addr) == 1);
	memcpy(address, &result, sizeof(result));
	*length = sizeof(result);
	return 0;
}

int main() {
	assert(fd_util::WaitUntilReady(socketHandle, 0.0001));
	assert(observedTimeout == 1 && observedEvents == POLLIN);
	returnedEvents = POLLOUT;
	assert(fd_util::WaitUntilReady(socketHandle, 0.5, true));
	assert(observedTimeout == 500 && observedEvents == POLLOUT);
	assert(!fd_util::WaitUntilReady(socketHandle, 0.5, false));
	returnedEvents = POLLHUP;
	assert(fd_util::WaitUntilReady(socketHandle, 0));
	returnedEvents = POLLERR;
	assert(fd_util::WaitUntilReady(socketHandle, 0));
	returnedEvents = POLLNVAL | POLLIN;
	assert(!fd_util::WaitUntilReady(socketHandle, 0));
	returnedEvents = POLLIN;
	pollResult = 0;
	assert(!fd_util::WaitUntilReady(socketHandle, 1));
	pollResult = -1;
	assert(!fd_util::WaitUntilReady(socketHandle, 1));
	pollResult = 1;
	assert(fd_util::WaitUntilReady(socketHandle, 1.0e30));
	assert(observedTimeout == INT_MAX);
	int calls = pollCalls;
	assert(!fd_util::WaitUntilReady(-1, 1));
	assert(!fd_util::WaitUntilReady(socketHandle, -1));
	assert(!fd_util::WaitUntilReady(socketHandle, std::numeric_limits<double>::quiet_NaN()));
	assert(!fd_util::WaitUntilReady(socketHandle, std::numeric_limits<double>::infinity()));
	assert(pollCalls == calls);
	fd_util::SetNonBlocking(socketHandle, true);
	assert(nonblockingValue == 1);
	fd_util::SetNonBlocking(socketHandle, false);
	assert(nonblockingValue == 0);
	failOption = true;
	fd_util::SetNonBlocking(socketHandle, true);
	assert(errno == EINVAL);
	assert(fd_util::GetLocalIP(socketHandle) == "192.0.2.5");
	failAddress = true;
	assert(fd_util::GetLocalIP(socketHandle).empty());
	puts("PS3 socket adapter: encoded handles, readiness, timeouts, errors, nonblocking and local IPv4 PASS (host API model)");
}
