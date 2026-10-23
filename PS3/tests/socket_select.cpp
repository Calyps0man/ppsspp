// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
#include <cassert>
#include <cstdio>
#include "Common/Net/SocketCompat.h"

static int calls, timeoutSeen, pollResult = 1;
static short eventsToReturn = POLLIN | POLLOUT;
static int fdSeen;
extern "C" int poll(pollfd *fds, nfds_t count, int timeout) {
	++calls; timeoutSeen = timeout;
	for (nfds_t i = 0; i < count; ++i) {
		fdSeen = fds[i].fd;
		fds[i].revents = pollResult > 0 ? eventsToReturn : 0;
	}
	return pollResult;
}

static int queueResult, queueLength = 1234;
extern "C" int netGetSockInfo(int socket, netSocketInfo *info, int count) {
	assert(socket == 37 && count == 1);
	info->recv_queue_len = queueLength;
	return queueResult;
}

int main() {
	constexpr int socket = SOCKET_FD_MASK | 37;
	fd_set reads, writes, exceptions;
	FD_ZERO(&reads); FD_ZERO(&writes); FD_ZERO(&exceptions);
	FD_SET(socket, &reads); FD_SET(socket, &writes);
	timeval timeout{0, 1};
	assert(select(socket + 1, &reads, &writes, nullptr, &timeout) == 2);
	assert(fdSeen == socket && timeoutSeen == 1);
	assert(FD_ISSET(socket, &reads) && FD_ISSET(socket, &writes));
	FD_CLR(socket, &writes); assert(!FD_ISSET(socket, &writes));
	FD_SET(SOCKET_FD_MASK | 1023, &reads);
	assert(FD_ISSET(1023, &reads));
	FD_CLR(1023, &reads);
	int before = calls;
	FD_SET(-1, &reads);
	assert(select(38, &reads, nullptr, nullptr, &timeout) == -1 && errno == EBADF && calls == before);
	FD_ZERO(&reads); FD_SET(socket, &reads);
	timeout.tv_usec = -1;
	assert(select(38, &reads, nullptr, nullptr, &timeout) == -1 && errno == EINVAL);
	timeout.tv_usec = 1000000;
	assert(select(38, &reads, nullptr, nullptr, &timeout) == -1 && errno == EINVAL);
	timeout = {0, 0};
	eventsToReturn = POLLNVAL;
	assert(select(38, &reads, nullptr, nullptr, &timeout) == -1 && errno == EBADF);
	assert(FD_ISSET(socket, &reads));
	eventsToReturn = POLLHUP;
	FD_SET(socket, &writes);
	assert(select(38, &reads, &writes, nullptr, &timeout) == 2);
	eventsToReturn = POLLPRI;
	FD_SET(socket, &exceptions);
	assert(select(38, &reads, &writes, &exceptions, nullptr) == 1 && timeoutSeen == -1);
	assert(!FD_ISSET(socket, &reads) && !FD_ISSET(socket, &writes) && FD_ISSET(socket, &exceptions));
	pollResult = 0;
	FD_SET(socket, &reads);
	assert(select(38, &reads, nullptr, nullptr, &timeout) == 0 && !FD_ISSET(socket, &reads));
	pollResult = -1; errno = EINTR;
	FD_SET(socket, &reads);
	assert(select(38, &reads, nullptr, nullptr, &timeout) == -1 && errno == EINTR && FD_ISSET(socket, &reads));
	assert(PS3SocketReceiveQueue(socket) == 1234);
	queueLength = -1; assert(PS3SocketReceiveQueue(socket) == 0);
	queueResult = -1; assert(PS3SocketReceiveQueue(socket) == 0);
	assert(PS3SocketReceiveQueue(-1) == 0);
	puts("PS3 select adapter: encoded handles, 32-bit masks, read/write counts, priority, hangup, invalid handles, timeouts and errors PASS (host API model)");
}
