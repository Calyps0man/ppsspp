// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#pragma once
#include <climits>
#include <cstdlib>
#include <cstring>

// PSL1GHT declares sendmsg but does not provide a POSIX wrapper. Gather the
// payload for one real send/sendto, preserving UDP datagram boundaries and
// TCP partial-send counts. Their wrappers translate SDK errno correctly.
inline ssize_t PS3SocketSendMsg(int socket, const msghdr *message, int flags) {
	if (!message || message->msg_iovlen < 0 || message->msg_iovlen > 1024
		|| (message->msg_iovlen && !message->msg_iov)) { errno = EINVAL; return -1; }
	if (message->msg_controllen) { errno = EOPNOTSUPP; return -1; }
	size_t total = 0;
	for (int i = 0; i < message->msg_iovlen; ++i) {
		const auto &part = message->msg_iov[i];
		if (!part.iov_base && part.iov_len) { errno = EFAULT; return -1; }
		if (part.iov_len > INT_MAX - total) { errno = EMSGSIZE; return -1; }
		total += part.iov_len;
	}
	char empty = 0;
	void *allocation = nullptr;
	const void *payload = &empty;
	if (message->msg_iovlen == 1) {
		if (total) payload = message->msg_iov[0].iov_base;
	} else if (total) {
		allocation = std::malloc(total);
		if (!allocation) { errno = ENOMEM; return -1; }
		size_t offset = 0;
		for (int i = 0; i < message->msg_iovlen; ++i) {
			const auto &part = message->msg_iov[i];
			if (part.iov_len) std::memcpy((char *)allocation + offset, part.iov_base, part.iov_len);
			offset += part.iov_len;
		}
		payload = allocation;
	}
	ssize_t result = message->msg_name
		? sendto(socket, payload, total, flags, (const sockaddr *)message->msg_name, message->msg_namelen)
		: send(socket, payload, total, flags);
	int error = errno;
	std::free(allocation);
	errno = error;
	return result;
}
