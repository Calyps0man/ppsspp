#include "ppsspp_config.h"

#include <errno.h>
#include <cmath>
#include <cstdio>
#include <climits>

#include "Common/CommonTypes.h"
#include "Common/Net/SocketCompat.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/File/FileDescriptor.h"
#include "Common/Log.h"

namespace fd_util {

bool WaitUntilReady(int fd, double timeout, bool for_write) {
#if PPSSPP_PLATFORM(PS3)
	// PSL1GHT socket handles include SOCKET_FD_MASK. Its poll wrapper strips
	// that tag; FD_SET would instead index past the end of the SDK's fd_set.
	if (fd < 0 || !std::isfinite(timeout) || timeout < 0.0) {
		return false;
	}
	pollfd descriptor{fd, (short)(for_write ? POLLOUT : POLLIN), 0};
	int milliseconds = timeout >= INT_MAX / 1000.0 ? INT_MAX : (int)ceil(timeout * 1000.0);
	int result = poll(&descriptor, 1, milliseconds);
	return result > 0 && !(descriptor.revents & POLLNVAL) &&
		(descriptor.revents & (descriptor.events | POLLERR | POLLHUP)) != 0;
#else
	struct timeval tv;
	tv.tv_sec = (long)floor(timeout);
	tv.tv_usec = (long)((timeout - floor(timeout)) * 1000000.0);

	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	// First argument to select is the highest socket in the set + 1.
	int rval;
	if (for_write) {
		rval = select(fd + 1, nullptr, &fds, nullptr, &tv);
	} else {
		rval = select(fd + 1, &fds, nullptr, nullptr, &tv);
	}

	if (rval < 0) {
		// Error calling select.
		return false;
	} else if (rval == 0) {
		// Timeout.
		return false;
	} else {
		// Socket is ready.
		return true;
	}
#endif
}

void SetNonBlocking(int sock, bool non_blocking) {
#if PPSSPP_PLATFORM(PS3)
	int value = non_blocking ? 1 : 0;
	if (setsockopt(sock, SOL_SOCKET, SO_NBIO, &value, sizeof(value)) < 0) {
		ERROR_LOG(Log::IO, "Error setting socket nonblocking status: %d", socket_errno);
	}
#elif !defined(_WIN32)
	int opts = fcntl(sock, F_GETFL);
	if (opts < 0) {
		perror("fcntl(F_GETFL)");
		ERROR_LOG(Log::IO, "Error getting socket status while changing nonblocking status");
	}
	if (non_blocking) {
		opts = (opts | O_NONBLOCK);
	} else {
		opts = (opts & ~O_NONBLOCK);
	}

	if (fcntl(sock, F_SETFL, opts) < 0) {
		perror("fcntl(F_SETFL)");
		ERROR_LOG(Log::IO, "Error setting socket nonblocking status");
	}
#else
	u_long val = non_blocking ? 1 : 0;
	if (ioctlsocket(sock, FIONBIO, &val) != 0) {
		ERROR_LOG(Log::IO, "Error setting socket nonblocking status");
	}
#endif
}

std::string GetLocalIP(int sock) {
	union {
		struct sockaddr sa;
		struct sockaddr_in ipv4;
#if !PPSSPP_PLATFORM(SWITCH) && !PPSSPP_PLATFORM(PS3)
		struct sockaddr_in6 ipv6;
#endif
	} server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	socklen_t len = sizeof(server_addr);
	int retval = getsockname(sock, (struct sockaddr *)&server_addr, &len);
	if (retval == 0) {
		char temp[64]{};

		// We clear the port below for WSAAddressToStringA.
		void *addr = nullptr;
#if !PPSSPP_PLATFORM(SWITCH) && !PPSSPP_PLATFORM(PS3)
		if (server_addr.sa.sa_family == AF_INET6) {
			server_addr.ipv6.sin6_port = 0;
			addr = &server_addr.ipv6.sin6_addr;
		}
#endif
		if (addr == nullptr) {
			server_addr.ipv4.sin_port = 0;
			addr = &server_addr.ipv4.sin_addr;
		}
#ifdef _WIN32
		wchar_t wtemp[sizeof(temp)];
		DWORD len = (DWORD)sizeof(temp);
		// Windows XP doesn't support inet_ntop.
		HRESULT result = WSAAddressToStringW((struct sockaddr *)&server_addr, sizeof(server_addr), nullptr, wtemp, &len);
		if (result == 0) {
			return ConvertWStringToUTF8(wtemp);
		} else {
			return "";
		}
#else
		const char *result = inet_ntop(server_addr.sa.sa_family, addr, temp, sizeof(temp));
		if (result) {
			return result;
		} else {
			return "";
		}
#endif
	} else {
		WARN_LOG(Log::IO, "GetLocalIP: getsockname failed with error %d (%s)", retval, strerror(retval));
		return "";
	}
}

}  // fd_util
