// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <net/netctl.h>
#include "Common/Net/SocketCompat.h"
#include "Common/Net/Resolve.h"

namespace {
constexpr int handle = SOCKET_FD_MASK | 37;
in_addr addresses[2];
char *addressList[] = {reinterpret_cast<char *>(&addresses[0]), reinterpret_cast<char *>(&addresses[1]), nullptr};
hostent host{nullptr, nullptr, AF_INET, sizeof(in_addr), addressList};
bool failLookup = false, failLocal = false, failOption = false;
int connectResult = -1, connectError = EINPROGRESS, socketError = 0, pollResult = 1;
short returnedEvents = POLLOUT;
int lookupCalls = 0, closed = 0;
}

extern "C" hostent *gethostbyname(const char *name) {
	assert(strcmp(name, "example.test") == 0);
	++lookupCalls;
	return failLookup ? nullptr : &host;
}
extern "C" int netCtlGetInfo(int code, net_ctl_info *info) {
	assert(code == NET_CTL_INFO_IP_ADDRESS);
	if (failLocal) return -1;
	strcpy(info->ip_address, "192.0.2.7");
	return 0;
}
extern "C" int socket(int family, int type, int protocol) {
	assert(family == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
	return handle;
}
extern "C" int setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
	assert(fd == handle && level == SOL_SOCKET && option == SO_NBIO);
	assert(length == sizeof(int) && *static_cast<const int *>(value) == 1);
	return failOption ? -1 : 0;
}
extern "C" int connect(int fd, const sockaddr *address, socklen_t length) {
	assert(fd == handle && length == sizeof(sockaddr_in));
	assert(address->sa_len == length && address->sa_family == AF_INET);
	errno = connectError;
	return connectResult;
}
extern "C" int poll(pollfd *fds, nfds_t count, int timeout) {
	assert(count == 1 && fds[0].fd == handle && fds[0].events == POLLOUT && timeout == 250);
	fds[0].revents = returnedEvents;
	return pollResult;
}
extern "C" int getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
	assert(fd == handle && level == SOL_SOCKET && option == SO_ERROR && *length == sizeof(int));
	*static_cast<int *>(value) = socketError;
	return 0;
}
extern "C" int close(int fd) {
	assert(fd == handle);
	++closed;
	return 0;
}

int main() {
	assert(::inet_pton(AF_INET, "192.0.2.1", &addresses[0]) == 1);
	assert(::inet_pton(AF_INET, "192.0.2.2", &addresses[1]) == 1);
	addrinfo *result = nullptr;
	std::string error;
	assert(net::DNSResolve("192.0.2.9", "65535", &result, error));
	assert(lookupCalls == 0 && !result->ai_next && result->ai_family == AF_INET);
	auto *address = reinterpret_cast<sockaddr_in *>(result->ai_addr);
	assert(address->sin_len == sizeof(sockaddr_in) && address->sin_port == htons(65535));
	net::DNSResolveFree(result);
	assert(net::DNSResolve("example.test", "80", &result, error));
	assert(result->ai_next && !result->ai_next->ai_next);
	in_addr saved = reinterpret_cast<sockaddr_in *>(result->ai_addr)->sin_addr;
	memset(addresses, 0, sizeof(addresses));
	assert(reinterpret_cast<sockaddr_in *>(result->ai_addr)->sin_addr.s_addr == saved.s_addr);
	net::DNSResolveFree(result);
	for (const char *service : {"65536", "999999999999999999999", "http", "-1"}) {
		assert(!net::DNSResolve("example.test", service, &result, error));
		assert(!result && !error.empty());
	}
	assert(!net::DNSResolve("", "80", &result, error));
	assert(!net::DNSResolve("example.test", "80", &result, error, net::DNSType::IPV6));
	failLookup = true;
	assert(!net::DNSResolve("example.test", "80", &result, error));
	failLookup = false;
	host.h_addrtype = AF_INET6;
	assert(!net::DNSResolve("example.test", "80", &result, error));
	host.h_addrtype = AF_INET;
	std::vector<std::string> local;
	assert(net::GetLocalIP4List(local) && local.size() == 1 && local[0] == "192.0.2.7");
	failLocal = true;
	assert(!net::GetLocalIP4List(local) && local.size() == 1);
	assert(net::HostPortExists("192.0.2.9", 80, 250));
	assert(closed == 1);
	socketError = ECONNREFUSED;
	assert(!net::HostPortExists("192.0.2.9", 80, 250));
	socketError = 0;
	returnedEvents = POLLERR;
	assert(!net::HostPortExists("192.0.2.9", 80, 250));
	returnedEvents = POLLOUT;
	pollResult = 0;
	assert(!net::HostPortExists("192.0.2.9", 80, 250));
	pollResult = 1;
	connectError = ECONNREFUSED;
	assert(!net::HostPortExists("192.0.2.9", 80, 250));
	connectResult = 0;
	assert(net::HostPortExists("192.0.2.9", 80, 250));
	failOption = true;
	assert(!net::HostPortExists("192.0.2.9", 80, 250));
	assert(closed == 7);
	assert(!net::HostPortExists("192.0.2.9", 0, 250));
	assert(!net::HostPortExists("192.0.2.9", 80, -1));
	puts("PS3 DNS adapter: owned IPv4 results, BSD layout, port validation, failures and connection checks PASS (host API model)");
}
