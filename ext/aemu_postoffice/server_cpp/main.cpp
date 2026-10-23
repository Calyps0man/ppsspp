#include <stdint.h>
#include <stdlib.h>

#include <chrono>
#include <thread>

#ifdef __unix__
#include <signal.h>
#include <sys/resource.h>
#include <sys/errno.h>
#endif

#include "server.h"
#include "log.h"

bool should_stop = false;

#ifdef __unix__
void handle_sigterm(int signum){
	should_stop = true;
}
#endif

int main(){
	#ifdef __unix__
	signal(SIGTERM, handle_sigterm);
	signal(SIGINT, handle_sigterm);
	#endif

	{
		aemu_postoffice_server::Config config;
		aemu_postoffice_server::Server server(config);

		#ifdef __unix__
		struct rlimit num_file_limit = {
			(rlim_t)(config.max_num_sessions + 10),
			(rlim_t)(config.max_num_sessions + 10)
		};
		int set_limit_status = setrlimit(RLIMIT_NOFILE, &num_file_limit);
		if (set_limit_status == -1){
			aemu_postoffice_server::LOG("%s: failed changing number of opened files (including sockets) limit, 0x%x\n", __func__, errno);
		}
		#endif

		while(!should_stop){
			auto begin = std::chrono::high_resolution_clock::now();
			aemu_postoffice_server::ServerPumpStatus pump_status = server.pump();
			if (pump_status != aemu_postoffice_server::ServerPumpStatus::SUCCESS){
				exit(1);
			}
			auto timespent = std::chrono::high_resolution_clock::now() - begin;
			int64_t wait_ms = config.target_tick_interval_ms - timespent / std::chrono::milliseconds(1);
			if (wait_ms > 0){
				std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
			}
		}
	}

	aemu_postoffice_server::LOG("%s: server stopped\n", __func__);

	return 0;
}
