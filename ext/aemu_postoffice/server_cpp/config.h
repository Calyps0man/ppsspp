#ifndef __CONFIG_H
#define __CONFIG_H

#include <string>

#include <stdint.h>

namespace aemu_postoffice_server {

struct Config {
	// ip address to bind to, will be decoded using OS provided inet_pton
	// if a v4 address is provided, the socket will only listen to v4
	// if a v6 address is provided, the socket will listen in v4 v6 mixed mode
	std::string ip_addr = std::string("::FFFF:0.0.0.0");
	// TCP port to listen to
	uint16_t port = 27313;

	// number of workers for parallelized workloads
	int num_threads = 4;

	// time from client creating a tcp socket to sending init data
	// in the case of ptp connect, it also includes the window of waiting for ptp listen to show up
	uint64_t session_init_time_limit_ms = 5000;
	// how big queued send can be in userspace buffer until the session is considered as dead
	uint64_t data_queue_size_limit_byte = 512000;
	// how log ptp_connect waits for ptp_listen to respond until ptp_connect times out
	uint64_t connect_time_limit_ms = 5000;

	// only used in stand-alone mode, target packet processing tick rate
	uint64_t target_tick_interval_ms = 8;

	// maximum number of pending and active sessions before new connections are rejceted
	int max_num_sessions = 5000;
};

}
#endif
