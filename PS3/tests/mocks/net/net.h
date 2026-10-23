#pragma once
#include "net/socket.h"
#include "netinet/in.h"
struct netSocketInfo { int s, proto, recv_queue_len, send_queue_len; in_addr local_adr; int local_port; in_addr remote_adr; int remote_port, state; };
extern "C" int netGetSockInfo(int, netSocketInfo *, int);
