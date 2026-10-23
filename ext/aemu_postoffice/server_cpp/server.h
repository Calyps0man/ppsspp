#ifndef __SERVER_H
#define __SERVER_H

#include <map>
#include <list>
#include <thread>
#include <semaphore>
#include <set>

#include "config.h"
#include "session.h"
#include "semaphore.h"

namespace aemu_postoffice_server {

enum class ServerPumpStatus{
	SUCCESS,
	LISTEN_SOCK_DEAD,
};

class Server{
	public:
		Server(Config config);
		~Server();
		// pump server processing
		ServerPumpStatus pump();

	private:
		std::list<PendingSession> pending_sessions;
		std::map<std::string, Session> sessions;

		bool stopping;

		std::vector<Semaphore> pending_sessions_pump_worker_semas;
		std::vector<Semaphore> pending_sessions_pump_worker_done_semas;
		std::vector<std::thread> pending_sessions_pump_workers;
		std::vector<std::list<PendingSession*>> to_pump_pending_sessions;
		std::vector<std::list<PendingSession*>> pending_sessions_to_remove;
		std::vector<std::list<PendingSession*>> ready_pending_sessions;

		std::vector<Semaphore> session_from_client_pump_worker_semas;
		std::vector<Semaphore> session_from_client_pump_worker_done_semas;
		std::vector<std::thread> session_from_client_pump_workers;
		std::vector<std::list<Session *>> sessions_to_pump;
		std::vector<std::set<std::string>> sessions_to_remove;
		std::vector<std::map<std::string, std::list<SendListItem>>> send_list;

		std::vector<Semaphore> session_to_client_pump_worker_semas;
		std::vector<Semaphore> session_to_client_pump_worker_done_semas;
		std::vector<std::thread> session_to_client_pump_workers;

		int sock_fd;

		Config config;

		void pump_pending_sessions(int set);
		void pump_connect_and_from_clients(int set);
		void pump_to_clients(int set);
};

}
#endif
