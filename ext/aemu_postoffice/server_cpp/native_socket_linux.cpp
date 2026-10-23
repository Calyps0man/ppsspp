#include "native_socket.h"
#include "log.h"

#include <string>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include <stdint.h>

namespace aemu_postoffice_server {

int native_recv(int fd, void *buf, int buflen){
	return recv(fd, buf, buflen, 0);
}

int native_send(int fd, void *buf, int buflen){
	#ifdef __linux__
	return send(fd, buf, buflen, MSG_NOSIGNAL);
	#else
	return send(fd, buf, buflen, 0);
	#endif
}

int native_get_last_socket_error(){
	return errno;
}

bool native_error_is_would_block(int error){
	return error == EAGAIN || error == EWOULDBLOCK;
}

bool native_error_is_no_mem(int error){
	return error == ENOBUFS || error == ENOMEM;
}

bool native_error_is_emfile(int error){
	return error == EMFILE;
}

int native_tcp_listen(std::string ip, uint16_t port){
	struct sockaddr_in6 addr6 = {0};
	struct sockaddr_in addr4 = {0};

	addr6.sin6_family = AF_INET6;
	addr4.sin_family = AF_INET;

	addr6.sin6_port = htons(port);
	addr4.sin_port = htons(port);

	int family = AF_INET6;

	if (inet_pton(AF_INET6, ip.c_str(), &addr6.sin6_addr) == -1){
		family = AF_INET;
		if (inet_pton(AF_INET, ip.c_str(), &addr4.sin_addr) == -1){
			return -2;
		}
	}

	int sock_fd = socket(family, SOCK_STREAM, 0);
	if (sock_fd == - 1){
		return -1;
	}

	if (family == AF_INET6){
		int opt = 0;
		setsockopt(sock_fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
	}

	int opt = 1;
	setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	void *addr = family == AF_INET6 ? (void *)&addr6 : (void *)&addr4;
	int addr_len = family == AF_INET6 ? sizeof(addr6) : sizeof(addr4);
	int bind_result = bind(sock_fd, (const sockaddr *)addr, addr_len);
	if (bind_result == -1){
		LOG("%s: bind failed, 0x%x\n", __func__, errno);
		return -1;
	}

	int listen_result = listen(sock_fd, 1000);
	if (listen_result == -1){
		LOG("%s: listen failed, 0x%x\n", __func__, errno);
		return -1;
	}

	int flags = fcntl(sock_fd, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(sock_fd, F_SETFL, flags);

	return sock_fd;
}

int native_accept(int sock_fd, std::string *peer_addr, uint16_t *peer_port){
	struct sockaddr_in6 addr = {0};

	socklen_t addr_len = sizeof(addr);
	int accept_result = accept(sock_fd, (sockaddr *)&addr, &addr_len);
	if (accept_result == -1){
		return -1;
	}

	int flags = fcntl(accept_result, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(accept_result, F_SETFL, flags);

	int sockopt = 1;
	setsockopt(accept_result, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt));

	sockopt = 64 * 1024;
	setsockopt(accept_result, SOL_SOCKET, SO_SNDBUF, &sockopt, sizeof(sockopt));
	setsockopt(accept_result, SOL_SOCKET, SO_RCVBUF, &sockopt, sizeof(sockopt));

	#ifndef __linux__
	sockopt = 1;
	setsockopt(accept_result, SOL_SOCKET, SO_NOSIGPIPE, &sockopt, sizeof(sockopt));
	#endif

	char addr_buf[128] = {0};
	uint16_t port = 0;
	if (addr.sin6_family == AF_INET6){
		inet_ntop(AF_INET6, &addr.sin6_addr, addr_buf, sizeof(addr_buf));
		port = addr.sin6_port;
	}else{
		struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
		inet_ntop(AF_INET, &addr4->sin_addr, addr_buf, sizeof(addr_buf));
		port = addr4->sin_port;
	}

	*peer_addr = std::string(addr_buf);
	*peer_port = port;

	return accept_result;
}

void native_close(int sock_fd){
	close(sock_fd);
}

}
