#pragma once
#include <cstdint>
#include <cstddef>
// Model the SDK's BSD address layout, not the host's Linux sockaddr.
using socklen_t = uint32_t;
using sa_family_t = uint8_t;
struct sockaddr { uint8_t sa_len; sa_family_t sa_family; char sa_data[]; };
#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 0x18
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 0xFFFF
#define SO_ERROR 0x1007
#define SO_NBIO 0x1100
#define SOCKET_FD_MASK 0x40000000
#define SHUT_RD 0
#define SHUT_RDWR 2
extern "C" {
int socket(int, int, int);
int connect(int, const sockaddr *, socklen_t);
int getsockname(int, sockaddr *, socklen_t *);
int getsockopt(int, int, int, void *, socklen_t *);
int setsockopt(int, int, int, const void *, socklen_t);
}

#define MSG_PEEK 0x2
extern "C" {
int send(int, const void *, int, int);
int recv(int, void *, int, int);
}

#include <sys/uio.h>
struct msghdr {
	void *msg_name; socklen_t msg_namelen; iovec *msg_iov; int msg_iovlen;
	void *msg_control; socklen_t msg_controllen; int msg_flags;
};
extern "C" int sendto(int, const void *, size_t, int, const sockaddr *, socklen_t);
