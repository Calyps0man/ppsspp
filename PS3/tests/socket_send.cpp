#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <net/socket.h>
#include "Common/Net/SocketSendPS3.h"
static int calls, result=5;
static bool addressed;
static const void *expectedPointer;
extern "C" int send(int fd, const void *buf, int len, int flags) {
	++calls; assert(fd==37 && flags==9);
	if(len) { assert(len==5 && !memcmp(buf,"abcde",5)); if(expectedPointer) assert(buf==expectedPointer); }
	if(result<0) errno=EAGAIN;
	return result;
}
extern "C" int sendto(int fd, const void *buf, size_t len, int flags, const sockaddr *addr, socklen_t size) {
	assert(addr && size==16); addressed=true; return send(fd,buf,len,flags);
}
int main() {
	char first[]="ab",second[]="cde",whole[]="abcde";
	iovec parts[]={{first,2},{nullptr,0},{second,3}}; msghdr message{};
	message.msg_iov=parts; message.msg_iovlen=3;
	assert(PS3SocketSendMsg(37,&message,9)==5 && calls==1);
	char address[16]{}; message.msg_name=address; message.msg_namelen=16; result=2;
	assert(PS3SocketSendMsg(37,&message,9)==2 && calls==2 && addressed);
	message.msg_name=nullptr; result=-1;
	assert(PS3SocketSendMsg(37,&message,9)==-1 && errno==EAGAIN && calls==3);
	parts[0]={whole,5}; message.msg_iovlen=1; expectedPointer=whole; result=5;
	assert(PS3SocketSendMsg(37,&message,9)==5 && calls==4);
	message.msg_iovlen=0; result=0; assert(PS3SocketSendMsg(37,&message,9)==0 && calls==5);
	message.msg_controllen=4; assert(PS3SocketSendMsg(37,&message,9)==-1 && errno==EOPNOTSUPP);
	message.msg_controllen=0; message.msg_iovlen=-1; assert(PS3SocketSendMsg(37,&message,9)==-1 && errno==EINVAL);
	message.msg_iovlen=1025; assert(PS3SocketSendMsg(37,&message,9)==-1 && errno==EINVAL);
	message.msg_iovlen=1; parts[0]={nullptr,1}; assert(PS3SocketSendMsg(37,&message,9)==-1 && errno==EFAULT);
	parts[0]={whole,(size_t)INT_MAX+1}; assert(PS3SocketSendMsg(37,&message,9)==-1 && errno==EMSGSIZE);
	message.msg_iov=nullptr; assert(PS3SocketSendMsg(37,&message,9)==-1 && errno==EINVAL);
	assert(calls==5);
	puts("PS3 sendmsg: gathered payload, one datagram, partial TCP counts, errors, empty payload, bounds and single-buffer path PASS (host API model)");
}
