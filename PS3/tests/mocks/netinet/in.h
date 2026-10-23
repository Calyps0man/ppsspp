#pragma once
#include <net/socket.h>
using u16 = uint16_t;
struct in_addr { uint32_t s_addr; };
struct sockaddr_in {
	uint8_t sin_len;
	sa_family_t sin_family;
	uint16_t sin_port;
	in_addr sin_addr;
	uint8_t sin_zero[8];
};
#define IPPROTO_TCP 6
#define INADDR_ANY 0
#define INET_ADDRSTRLEN 16
