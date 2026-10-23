#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#include <string.h>
#include <stdio.h>

#include "postoffice_client.h"
#include "sock_impl.h"
#include "log_impl.h"

void to_native_sock_addr(native_sock_addr *dst, const struct aemu_post_office_sock_addr *src){
	dst->sin_family = AF_INET;
	dst->sin_addr.s_addr = src->addr;
	dst->sin_port = src->port;
}

void to_native_sock6_addr(native_sock6_addr *dst, const struct aemu_post_office_sock6_addr *src){
	dst->sin6_family = AF_INET6;
	dst->sin6_port = src->port;
	dst->sin6_flowinfo = 0;
	memcpy(dst->sin6_addr.s6_addr, src->addr, 16);
	dst->sin6_scope_id = 0;
}

// SO_SNDTIMEO can be used on linux, but no idea if it can be used on other unix likes
static int connect_with_timeout(int sock, void *addr, int addrlen, int timeout_ms, int *error){
	int flags = fcntl(sock, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(sock, F_SETFL, flags);

	int ret = 0;

	struct timespec begin = {0};
	clock_gettime(CLOCK_MONOTONIC, &begin);

	while(1){
		int result = connect(sock, (struct sockaddr *)addr, addrlen);
		if (result == 0){
			ret = 0;
			break;
		}
		if (result == -1){
			*error = errno;

			struct timespec now = {0};
			clock_gettime(CLOCK_MONOTONIC, &now);

			int ms_since_begin = (now.tv_sec - begin.tv_sec) * 1000 + (now.tv_nsec - begin.tv_nsec) / 1000000;
			if (ms_since_begin > timeout_ms){
				*error = ETIMEDOUT;
				ret = -1;
				break;
			}

			if (*error == EAGAIN || *error == EALREADY || *error == EINPROGRESS){
				// in progress
				struct timespec sleep_time = {
					.tv_sec = 0,
					.tv_nsec = 1000000,
				};
				nanosleep(&sleep_time, NULL);
				continue;
			}
			if (*error == EISCONN){
				// connected
				*error = 0;
				ret = 0;
				break;
			}
			ret = -1;
			break;
		}
	}

	// just for completness, NBIO is used on the socket after connection anyway
	flags = fcntl(sock, F_GETFL, 0);
	flags &= ~O_NONBLOCK;
	fcntl(sock, F_SETFL, flags);

	return ret;
}


int native_connect_tcp_sock(void *addr, int addrlen){
	native_sock_addr *native_addr = addr;
	int sock = socket(native_addr->sin_family, SOCK_STREAM, 0);
	if (sock == -1){
		LOG("%s: failed creating socket, %s\n", __func__, strerror(errno));
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	// XXX this restricts latency to 500ms, if a server is even further away, connection won't be possible

	// Connect
	int error = 0;
	int connect_status = connect_with_timeout(sock, addr, addrlen, 5000, &error);
	if (connect_status == -1){
		LOG("%s: failed connecting, %s\n", __func__, strerror(error));
		close(sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	// Set socket options
	socklen_t sockopt = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt));
	int flags = fcntl(sock, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(sock, F_SETFL, flags);

	sockopt = 2626560;
	setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sockopt, sizeof(sockopt));
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockopt, sizeof(sockopt));

	#ifndef __linux__
	sockopt = 1;
	setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &sockopt, sizeof(sockopt));
	#endif

	// Show some socket options
	unsigned int opt_len = sizeof(sockopt);
	sockopt = 0;
	int get_ret = getsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sockopt, &opt_len);
	LOG("%s: TCP_NODELAY is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? errno : 0);

	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sockopt, &opt_len);
	LOG("%s: SO_SNDBUF is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? errno : 0);

	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockopt, &opt_len);
	LOG("%s: SO_RCVBUF is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? errno : 0);

	#ifndef __linux__
	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &sockopt, &opt_len);
	LOG("%s: SO_NOSIGPIPE is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? errno : 0);
	#endif

	return sock;
}

int native_send_till_done(int fd, const char *buf, int len, bool non_block, bool *abort){
	int write_offset = 0;
	while(write_offset != len){
		if (*abort){
			return NATIVE_SOCK_ABORTED;
		}
		#ifdef __linux__
		int write_status = send(fd, &buf[write_offset], len - write_offset, MSG_NOSIGNAL);
		#else
		int write_status = send(fd, &buf[write_offset], len - write_offset, 0);
		#endif
		if (write_status == -1){
			int err = errno;
			if (err == EAGAIN || err == EWOULDBLOCK){
				if (non_block && write_offset == 0){
					return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
				}
				// Continue block sending, either in block mode or we already received part of the message
				sleep(0);
				continue;
			}
			// Other errors
			LOG("%s: failed sending, %s\n", __func__, strerror(errno));
			return write_status;
		}
		write_offset += write_status;
	}
	return write_offset;
}

int native_recv(int fd, char *buf, int len){
	int recv_status = recv(fd, buf, len, 0);
	if (recv_status == 0){
		return recv_status;
	}
	if (recv_status < 0){
		int err = errno;
		if (err == EAGAIN || err == EWOULDBLOCK){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		// Other errors
		LOG("%s: failed receving, %s\n", __func__, strerror(errno));
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
					return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
				}
				// Continue block receving, either in block mode or we already sent part of the message
				sleep(0);
				continue;
			}
			return recv_status;
		}
		read_offset += recv_status;
	}
	return read_offset;
}

int native_close_tcp_sock(int sock){
	return close(sock);
}

int native_peek(int fd, char *buf, int len){
	int read_result = recv(fd, buf, len, MSG_PEEK);
	if (read_result == 0){
		return 0;
	}
	if (read_result == -1){
		int err = errno;
		if (err == EAGAIN || err == EWOULDBLOCK){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		LOG("%s: failed peeking, %s\n", __func__, strerror(errno));
		return -1;
	}
	return read_result;
}

bool native_send_buf_not_full(int fd){
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLWRNORM;
	pfd.revents = 0;
	poll(&pfd, 1, 0);
	if (pfd.revents & POLLHUP){
		return false;
	}
	if (pfd.revents & POLLWRNORM){
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
