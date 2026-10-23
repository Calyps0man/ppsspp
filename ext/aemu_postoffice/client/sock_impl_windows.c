#include <winsock2.h>
#include <windows.h>
#include <profileapi.h>

#include <string.h>
#include <stdio.h>

#include "postoffice_client.h"
#include "sock_impl.h"
#include "log_impl.h"

void to_native_sock_addr(native_sock_addr *dst, const struct aemu_post_office_sock_addr *src){
	dst->sin_family = AF_INET;
	dst->sin_addr.s_addr = src->addr;
	dst->sin_port = src->port;
	memset(dst->sin_zero, 0, sizeof(dst->sin_zero));
}

void to_native_sock6_addr(native_sock6_addr *dst, const struct aemu_post_office_sock6_addr *src){
	dst->sin6_family = AF_INET6;
	dst->sin6_port = src->port;
	dst->sin6_flowinfo = 0;
	memcpy(dst->sin6_addr.s6_addr, src->addr, 16);
	dst->sin6_scope_id = 0;
}

static void init_winsock2(){
	static bool initialized = false;
	if (!initialized){
		initialized = true;
		WSADATA data;
		int init_result = WSAStartup(MAKEWORD(2,2), &data);
		if (init_result != 0){
			printf("%s: warning: WSAStartup seems to have failed, %d\n", __func__, init_result);
		}
	}
}

static int connect_with_timeout(int sock, native_sock_addr *addr, int addrlen, int timeout_ms, int *error){
	u_long ioctlopt = 1;
	ioctlsocket(sock, FIONBIO, &ioctlopt);

	int ret = 0;

	LARGE_INTEGER begin = {0};
	QueryPerformanceCounter(&begin);
	LARGE_INTEGER ticks_per_seconds = {0};
	QueryPerformanceFrequency(&ticks_per_seconds);

	while(1){
		int result = connect(sock, (struct sockaddr *)addr, addrlen);
		if (result == 0){
			ret = 0;
			break;
		}
		if (result == -1){
			LARGE_INTEGER now = {0};
			QueryPerformanceCounter(&now);
			int ms_since_begin = (now.QuadPart - begin.QuadPart) * 1000 / ticks_per_seconds.QuadPart;
			if (ms_since_begin > timeout_ms){
				*error = WSAETIMEDOUT;
				ret = -1;
				break;
			}

			*error = WSAGetLastError();
			// it was found out together with @hrydgard that after using WinHttp, it could also throw WSAEINVAL for WSAEALREADY
			if (*error == WSAEWOULDBLOCK || *error == WSAEALREADY || *error == WSAEINVAL){
				// in progress
				Sleep(1);
				continue;
			}
			if (*error == WSAEISCONN){
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
	ioctlopt = 0;
	ioctlsocket(sock, FIONBIO, &ioctlopt);
	return ret;
}

int native_connect_tcp_sock(void *addr, int addrlen){
	init_winsock2();

	native_sock_addr *native_addr = addr;
	SOCKET win_sock = socket(native_addr->sin_family, SOCK_STREAM, 0);
	if (win_sock == INVALID_SOCKET){
		LOG("%s: failed creating socket, %d\n", __func__, WSAGetLastError());
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}
	int sock = win_sock;

	// XXX need to simulate timeout on windows, there's no sockopt for that
	// this also restricts latency to 500ms, if a server is even further away, connection won't be possible

	// Connect
	int error = 0;
	int connect_status = connect_with_timeout(sock, addr, addrlen, 5000, &error);
	if (connect_status == -1){
		LOG("%s: failed connecting, %d\n", __func__, error);
		closesocket(sock);
		return AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK;
	}

	// Set socket options
	int sockopt = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&sockopt, sizeof(sockopt));
	u_long ioctlopt = 1;
	ioctlsocket(sock, FIONBIO, &ioctlopt);

	sockopt = 2626560;
	setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&sockopt, sizeof(sockopt));
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&sockopt, sizeof(sockopt));

	// Show some socket options
	int opt_len = sizeof(sockopt);
	sockopt = 0;
	int get_ret = getsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&sockopt, &opt_len);
	LOG("%s: TCP_NODELAY is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? WSAGetLastError() : 0);

	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&sockopt, &opt_len);
	LOG("%s: SO_SNDBUF is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? WSAGetLastError() : 0);

	opt_len = sizeof(sockopt);
	sockopt = 0;
	get_ret = getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&sockopt, &opt_len);
	LOG("%s: SO_RCVBUF is %d (0x%x)\n", __func__, sockopt, get_ret == -1 ? WSAGetLastError() : 0);

	return sock;
}

int native_send_till_done(int fd, const char *buf, int len, bool non_block, bool *abort){
	int write_offset = 0;
	while(write_offset != len){
		if (*abort){
			return NATIVE_SOCK_ABORTED;
		}
		int write_status = send(fd, &buf[write_offset], len - write_offset, 0);
		if (write_status == -1){
			int err = WSAGetLastError();
			if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS){
				if (non_block && write_offset == 0){
					return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
				}
				// Continue block sending, either in block mode or we already received part of the message
				Sleep(0);
				continue;
			}
			// Other errors
			LOG("%s: failed sending, %d\n", __func__, err);
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
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		// Other errors
		LOG("%s: failed receving, %d\n", __func__, err);
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
				Sleep(0);
				continue;
			}
			return recv_status;
		}
		read_offset += recv_status;
	}
	return read_offset;
}

int native_close_tcp_sock(int sock){
	return closesocket(sock);
}

int native_peek(int fd, char *buf, int len){
	int read_result = recv(fd, buf, len, MSG_PEEK);
	if (read_result == 0){
		return 0;
	}
	if (read_result == -1){
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS){
			return AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK;
		}
		LOG("%s: failed peeking, %d\n", __func__, WSAGetLastError());
		return -1;
	}
	return read_result;
}

bool native_send_buf_not_full(int fd){
	WSAPOLLFD pfd;
	pfd.fd = fd;
	pfd.events = POLLWRNORM;
	pfd.revents = 0;
	WSAPoll(&pfd, 1, 0);
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
