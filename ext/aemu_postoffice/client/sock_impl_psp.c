#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pspnet_inet.h>
#include <sys/errno.h>
#include <pspthreadman.h>

#include <string.h>

#include "postoffice_client.h"
#include "sock_impl.h"
#include "log_impl.h"

void to_native_sock_addr(native_sock_addr *dst, const struct aemu_post_office_sock_addr *src){
	dst->sin_family = AF_INET;
	dst->sin_addr.s_addr = src->addr;
	dst->sin_port = src->port;
}

void to_native_sock6_addr(native_sock6_addr *dst, const struct aemu_post_office_sock6_addr *src){
	// cannot do this on psp
}

int has_high_mem();

int native_connect_tcp_sock(void *addr, int addrlen){
	native_sock_addr *native_addr = addr;
	int sock = sceNetInetSocket(native_addr->sin_family, SOCK_STREAM, 0);
	if (sock == -1){
		int err = sceNetInetGetErrno();
		LOG("%s: failed creating socket, 0x%x\n", __func__, err);
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	// Set socket options
	int sockopt = 1;
	int set_status = sceNetInetSetsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt));
	//LOG("%s: IPPROTO_TCP TCP_NODELAY %d, 0x%x\n", __func__, sockopt, set_status);
	if (has_high_mem()){
		// we have enlarged network heap!
		sockopt = 1024 * 64;
		set_status = sceNetInetSetsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sockopt, sizeof(sockopt));
		//LOG("%s: SOL_SOCKET SO_SNDBUF %d, 0x%x\n", __func__, sockopt, set_status);
		sockopt = 1024 * 64;
		set_status = sceNetInetSetsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockopt, sizeof(sockopt));
		//LOG("%s: SOL_SOCKET SO_RCVBUF %d, 0x%x\n", __func__, sockopt, set_status);
	}

	// Connect
	int connect_status = sceNetInetConnect(sock, addr, addrlen);
	if (connect_status == -1){
		int err = sceNetInetGetErrno();
		LOG("%s: failed connecting, 0x%x\n", __func__, err);
		sceNetInetClose(sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	return sock;
}

int native_send_till_done(int fd, const char *buf, int len, bool non_block, bool *abort){
	int write_offset = 0;
	while(write_offset != len){
		if (*abort){
			return NATIVE_SOCK_ABORTED;
		}
		int write_status = sceNetInetSend(fd, &buf[write_offset], len - write_offset, MSG_DONTWAIT);
		if (write_status == -1){
			int err = sceNetInetGetErrno();
			if (err == EAGAIN || err == EWOULDBLOCK){
				if (non_block && write_offset == 0){
					sceKernelDelayThread(0);
					return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
				}
				// Continue block sending, either in block mode or we already received part of the message
				sceKernelDelayThread(0);
				continue;
			}
			// Other errors
			LOG("%s: failed sending, 0x%x\n", __func__, err);
			return write_status;
		}
		write_offset += write_status;
	}
	return write_offset;
}

int native_recv(int fd, char *buf, int len){
	int recv_status = sceNetInetRecv(fd, buf, len, MSG_DONTWAIT);
	if (recv_status == 0){
		return recv_status;
	}
	if (recv_status < 0){
		int err = sceNetInetGetErrno();
		if (err == EAGAIN || err == EWOULDBLOCK){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		// Other errors
		LOG("%s: failed receving, 0x%x\n", __func__, err);
		return recv_status;
	}
	return recv_status;
}

int native_recv_till_done(int fd, char *buf, int len, bool non_block, bool *abort){
	int read_offset = 0;
	while(read_offset != len){
		if (*abort){
			return NATIVE_SOCK_ABORTED;
		}
		int recv_status = native_recv(fd, &buf[read_offset], len - read_offset);
		if (recv_status == 0){
			return recv_status;
		}
		if (recv_status < 0){
			if (recv_status == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
				if (non_block && read_offset == 0){
					sceKernelDelayThread(0);
					return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
				}
				// Continue block receving, either in block mode or we already received part of the message
				sceKernelDelayThread(0);
				continue;
			}
			return recv_status;
		}
		read_offset += recv_status;
	}
	return read_offset;
}

int native_close_tcp_sock(int sock){
	return sceNetInetClose(sock);
}

int native_peek(int fd, char *buf, int len){
	int read_result = sceNetInetRecv(fd, buf, len, MSG_DONTWAIT | MSG_PEEK);
	if (read_result == 0){
		return 0;
	}
	if (read_result == -1){
		int err = sceNetInetGetErrno();
		if (err == EAGAIN || err == EWOULDBLOCK){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		LOG("%s: failed peeking, 0x%x\n", __func__, err);
		return -1;
	}
	return read_result;
}

// bits from aemu pspnet_inet.h
typedef struct SceNetInetPollfd {
	int fd;
	short events;
	short revents;
} SceNetInetPollfd;

#define INET_POLLWRNORM 0x0004
#define INET_POLLRDNORM 0x0040
#define INET_POLLHUP 0x0010

int sceNetInetPoll(struct SceNetInetPollfd *fds, size_t nfds, int timeout);

bool native_send_buf_not_full(int fd){
	struct SceNetInetPollfd pfd;
	pfd.fd = fd;
	pfd.events = INET_POLLWRNORM;
	pfd.revents = 0;
	sceNetInetPoll(&pfd, 1, 0);
	if (pfd.revents & INET_POLLWRNORM){
		return true;
	}
	return false;
}

bool native_hung_up(int fd){
	uint8_t buf;
	int peek_result = native_peek(fd, (char *)&buf, sizeof(buf));
	if (peek_result == AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK ||
		peek_result == sizeof(buf)
	){
		return false;
	}
	return true;
}
