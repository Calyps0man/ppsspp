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

// Pure conversion checks compiled with the native SDK, executed on Linux PPC64.
// Only logging and unused diagnostic formatting are isolated. This does not execute or model PS3 sockets.
#include <cstdio>
#include <cstdlib>
#include "Common/Net/SocketCompat.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/NetInetConstants.h"

LogChannel g_log[(size_t)Log::NUMBER_OF_LOGS];
static bool loggingEnabled = false;
bool *g_bLogEnabledSetting = &loggingEnabled;
void hleLeave() {}
void hleDoLogInternal(Log, LogLevel, u64, const char *, int, const char *, const char *, const char *) {
	std::abort();
}

std::string StringFromFormat(const char *, ...) {
	std::abort();
}

static int checks;
static void Check(bool result) {
	++checks;
	if (!result) {
		std::printf("Native constants check %d failed\n", checks);
		std::exit(1);
	}
}

int main() {
	Check(convertSocketDomainPSP2Host(PSP_NET_INET_AF_INET) == AF_INET);
	Check(convertSocketDomainPSP2Host(0x1234) == -1);
	Check(convertSocketTypePSP2Host(PSP_NET_INET_SOCK_STREAM) == SOCK_STREAM);
	Check(convertSocketTypePSP2Host(PSP_NET_INET_SOCK_DGRAM) == SOCK_DGRAM);
	Check(convertSocketTypePSP2Host(PSP_NET_INET_SOCK_RDM) == -1);
	Check(convertSocketTypePSP2Host(PSP_NET_INET_SOCK_SEQPACKET) == -1);
	Check(convertSocketTypePSP2Host(PSP_NET_INET_SOCK_CONN_DGRAM) == -1);
	Check(convertSocketTypePSP2Host(PSP_NET_INET_SOCK_PACKET) == -1);
	Check(convertSocketProtoPSP2Host(PSP_NET_INET_IPPROTO_TCP) == IPPROTO_TCP);
	Check(convertSocketProtoPSP2Host(PSP_NET_INET_IPPROTO_UDP) == IPPROTO_UDP);
	Check(convertSocketProtoPSP2Host(PSP_NET_INET_IPPROTO_IGMP) == -1);
	Check(convertSocketProtoPSP2Host(PSP_NET_INET_IPPROTO_RAW) == -1);
	Check(convertSockoptLevelPSP2Host(PSP_NET_INET_SOL_SOCKET) == SOL_SOCKET);
	Check(convertSockoptLevelPSP2Host(0x1234) == -1);
	Check(convertSockoptNamePSP2Host(PSP_NET_INET_SO_REUSEADDR, PSP_NET_INET_SOL_SOCKET) == SO_REUSEADDR);
	Check(convertSockoptNamePSP2Host(PSP_NET_INET_SO_NBIO, PSP_NET_INET_SOL_SOCKET) == SO_NBIO);
	Check(convertSockoptNamePSP2Host(PSP_NET_INET_SO_DEBUG, PSP_NET_INET_SOL_SOCKET) == -1);
	Check(convertSockoptNamePSP2Host(PSP_NET_INET_TCP_NODELAY, PSP_NET_INET_IPPROTO_TCP) == -1);
	Check(convertSockoptNamePSP2Host(PSP_NET_INET_TCP_MAXSEG, PSP_NET_INET_IPPROTO_TCP) == -1);
	Check(convertInetErrnoHost2PSP(ENETDOWN) == ERROR_INET_ENETDOWN);
	Check(convertInetErrnoHost2PSP(EOPNOTSUPP) == ERROR_INET_EOPNOTSUPP);
	std::printf("PASS: %d production native SDK constant conversions; no PS3 syscalls executed\n", checks);
}
