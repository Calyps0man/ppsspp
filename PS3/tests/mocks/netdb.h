#pragma once
#include <net/socket.h>
struct hostent {
	char *h_name;
	char **h_aliases;
	int h_addrtype;
	int h_length;
	char **h_addr_list;
};
struct addrinfo {
	int ai_flags, ai_family, ai_socktype, ai_protocol;
	socklen_t ai_addrlen;
	sockaddr *ai_addr;
	char *ai_canonname;
	addrinfo *ai_next;
};
extern "C" hostent *gethostbyname(const char *);
