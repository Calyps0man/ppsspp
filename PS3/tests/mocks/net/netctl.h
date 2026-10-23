#pragma once
#define NET_CTL_INFO_IP_ADDRESS 16
union net_ctl_info { char ip_address[16]; };
extern "C" int netCtlGetInfo(int, net_ctl_info *);
