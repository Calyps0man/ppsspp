// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ppsspp_config.h"
#if PPSSPP_PLATFORM(PS3)

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <net/netctl.h>
#include "Common/Net/SocketCompat.h"
#include "Common/Net/Resolve.h"

namespace net {
namespace {
// PSL1GHT returns a static hostent. Hold the lock until its addresses have
// been copied into caller-owned storage, including on allocation failure.
std::mutex dnsMutex;
struct ResolvedAddress {
	addrinfo info;
	sockaddr_in address;
};
}

void DNSResolveFree(addrinfo *res) {
	while (res) {
		addrinfo *next = res->ai_next;
		free(res);
		res = next;
	}
}

bool DNSResolve(const std::string &host, const std::string &service, addrinfo **res, std::string &error, DNSType type) {
	*res = nullptr;
	error.clear();
	if (type == DNSType::IPV6) {
		error = "IPv6 DNS is unavailable on PS3";
		return false;
	}
	if (host.empty()) {
		error = "Empty hostname";
		return false;
	}
	unsigned port = 0;
	for (char ch : service) {
		if (ch < '0' || ch > '9' || (port = port * 10 + ch - '0') > 65535) {
			error = "PS3 DNS requires a numeric port from 0 to 65535";
			return false;
		}
	}
	addrinfo **tail = res;
	auto append = [&](const void *address) {
		auto *entry = static_cast<ResolvedAddress *>(calloc(1, sizeof(ResolvedAddress)));
		if (!entry) return false;
		entry->address.sin_len = sizeof(sockaddr_in);
		entry->address.sin_family = AF_INET;
		entry->address.sin_port = htons((u16)port);
		memcpy(&entry->address.sin_addr, address, sizeof(in_addr));
		entry->info.ai_family = AF_INET;
		entry->info.ai_socktype = SOCK_STREAM;
		entry->info.ai_protocol = IPPROTO_TCP;
		entry->info.ai_addrlen = sizeof(sockaddr_in);
		entry->info.ai_addr = reinterpret_cast<sockaddr *>(&entry->address);
		*tail = &entry->info;
		tail = &entry->info.ai_next;
		return true;
	};
	in_addr numeric{};
	if (::inet_pton(AF_INET, host.c_str(), &numeric) == 1) {
		if (append(&numeric)) return true;
		error = "Out of memory resolving IPv4 address";
		return false;
	}
	std::lock_guard<std::mutex> guard(dnsMutex);
	hostent *resolved = gethostbyname(host.c_str());
	if (!resolved || resolved->h_addrtype != AF_INET || resolved->h_length != sizeof(in_addr) || !resolved->h_addr_list) {
		error = "PS3 IPv4 hostname lookup failed";
		return false;
	}
	for (char **address = resolved->h_addr_list; *address; ++address) {
		if (!append(*address)) {
			DNSResolveFree(*res);
			*res = nullptr;
			error = "Out of memory resolving hostname";
			return false;
		}
	}
	if (!*res) error = "Hostname has no IPv4 addresses";
	return *res != nullptr;
}

bool HostPortExists(const std::string &host, int port, int timeout_ms) {
	if (port <= 0 || port > 65535 || timeout_ms < 0) return false;
	addrinfo *resolved = nullptr;
	std::string error;
	if (!DNSResolve(host, std::to_string(port), &resolved, error, DNSType::IPV4)) return false;
	bool connected = false;
	for (addrinfo *address = resolved; address && !connected; address = address->ai_next) {
		int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (fd < 0) continue;
		int nonblocking = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_NBIO, &nonblocking, sizeof(nonblocking)) == 0) {
			int result = connect(fd, address->ai_addr, address->ai_addrlen);
			connected = result == 0;
			if (!connected && connectInProgress(socket_errno)) {
				pollfd descriptor{fd, POLLOUT, 0};
				if (poll(&descriptor, 1, timeout_ms) > 0 && (descriptor.revents & POLLOUT) && !(descriptor.revents & POLLNVAL)) {
					int socketError = 0;
					socklen_t length = sizeof(socketError);
					connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) == 0 && socketError == 0;
				}
			}
		}
		closesocket(fd);
	}
	DNSResolveFree(resolved);
	return connected;
}

bool GetLocalIP4List(std::vector<std::string> &addresses) {
	net_ctl_info info{};
	if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) != 0) return false;
	info.ip_address[sizeof(info.ip_address) - 1] = 0;
	in_addr address{};
	if (::inet_pton(AF_INET, info.ip_address, &address) != 1) return false;
	if (address.s_addr != INADDR_ANY) addresses.emplace_back(info.ip_address);
	return true;
}

}  // namespace net
#endif
