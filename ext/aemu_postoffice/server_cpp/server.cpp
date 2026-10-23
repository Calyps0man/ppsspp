#ifdef __unix__
// for naming threads
#include <pthread.h>
#endif

#include <stdio.h>

#include "server.h"
#include "log.h"

#include "native_socket.h"

namespace aemu_postoffice_server {

static void set_thread_name(std::string name){
	#if __unix__
	pthread_t tid = pthread_self();
	pthread_setname_np(tid, name.c_str());
	#else
	// hm, what do
	#endif
}

Server::Server(Config config){
	this->config = config;
	this->sock_fd = native_tcp_listen(config.ip_addr, config.port);

	if (this->sock_fd == -1){
		LOG("%s: failed creating listen socket, 0x%x\n", __func__, native_get_last_socket_error());
		return;
	}

	if (this->sock_fd == -2){
		this->sock_fd = -1;
		LOG("%s: failed parsing %s as ipv6 nor ipv4\n", __func__, config.ip_addr.c_str());
		return;
	}

	this->stopping = false;

	for(int i = 0;i < config.num_threads;i++){
		this->pending_sessions_pump_worker_semas.push_back(Semaphore());
		this->pending_sessions_pump_worker_done_semas.push_back(Semaphore());
		this->session_from_client_pump_worker_semas.push_back(Semaphore());
		this->session_from_client_pump_worker_done_semas.push_back(Semaphore());
		this->session_to_client_pump_worker_semas.push_back(Semaphore());
		this->session_to_client_pump_worker_done_semas.push_back(Semaphore());
	}

	for(int i = 0;i < config.num_threads;i++){
		this->pending_sessions_pump_workers.push_back(std::thread([this, i]() {
			char buf[16] = {0};
			snprintf(buf, sizeof(buf), "pending_%d", i);
			set_thread_name(std::string(buf));
			while(true){
				this->pending_sessions_pump_worker_semas[i].acquire();
				if (this->stopping){
					this->pending_sessions_pump_worker_done_semas[i].release();
					break;
				}
				pump_pending_sessions(i);
				this->pending_sessions_pump_worker_done_semas[i].release();
			}
		}));
		this->to_pump_pending_sessions.push_back(std::list<PendingSession*>());
		this->pending_sessions_to_remove.push_back(std::list<PendingSession*>());
		this->ready_pending_sessions.push_back(std::list<PendingSession*>());
	}

	for(int i = 0;i < config.num_threads;i++){
		this->session_from_client_pump_workers.push_back(std::thread([this, i]() {
			char buf[16] = {0};
			snprintf(buf, sizeof(buf), "from_client_%d", i);
			set_thread_name(std::string(buf));
			while(true){
				this->session_from_client_pump_worker_semas[i].acquire();
				if (this->stopping){
					this->session_from_client_pump_worker_done_semas[i].release();
					break;
				}
				this->pump_connect_and_from_clients(i);
				this->session_from_client_pump_worker_done_semas[i].release();
			}
		}));
		this->sessions_to_pump.push_back(std::list<Session *>());
		this->sessions_to_remove.push_back(std::set<std::string>());
		this->send_list.push_back(std::map<std::string, std::list<SendListItem>>());
	}

	for(int i = 0;i < config.num_threads;i++){
		this->session_to_client_pump_workers.push_back(std::thread([this, i]() {
			char buf[16] = {0};
			snprintf(buf, sizeof(buf), "to_client_%d", i);
			set_thread_name(std::string(buf));
			while(true){
				this->session_to_client_pump_worker_semas[i].acquire();
				if (this->stopping){
					this->session_to_client_pump_worker_done_semas[i].release();
					break;
				}
				this->pump_to_clients(i);
				this->session_to_client_pump_worker_done_semas[i].release();
			}
		}));
	}

	LOG("%s: created server listening on %s %u\n", __func__, config.ip_addr.c_str(), config.port);
}

Server::~Server(){
	if (this->sock_fd >= 0){
		native_close(this->sock_fd);
	}else{
		return;
	}

	this->stopping = true;
	for(auto &sema : this->pending_sessions_pump_worker_semas){
		sema.release();
	}
	for(auto &sema : this->session_from_client_pump_worker_semas){
		sema.release();
	}	
	for(auto &sema : this->session_to_client_pump_worker_semas){
		sema.release();
	}
	for(auto &thread : this->pending_sessions_pump_workers){
		thread.join();
	}
	for(auto &thread : this->session_from_client_pump_workers){
		thread.join();
	}
	for(auto &thread : this->session_to_client_pump_workers){
		thread.join();
	}

	for(auto &session : this->sessions){
		session.second.close_socket();
	}

	for(auto &pending_session : this->pending_sessions){
		pending_session.close_socket();
	}
}

void Server::pump_pending_sessions(int set){
	for(auto &pending_session : this->to_pump_pending_sessions[set]){
		PendingSessionPumpStatus pump_status = pending_session->pump(this->sessions);
		switch(pump_status){
			case PendingSessionPumpStatus::SUCCESS:
				continue;
			case PendingSessionPumpStatus::SOCKET_CLOSED:
			case PendingSessionPumpStatus::TIMEOUT:
			case PendingSessionPumpStatus::BAD_INIT:
				this->pending_sessions_to_remove[set].push_back(pending_session);
				continue;
			case PendingSessionPumpStatus::SESSION_READY:
				this->ready_pending_sessions[set].push_back(pending_session);
				continue;
		}
	}
}

void Server::pump_connect_and_from_clients(int set){
	for(auto &session : this->sessions_to_pump[set]){
		SessionPumpStatus pump_status = session->pump_connect();
		if (pump_status == SessionPumpStatus::TIMEOUT){
			sessions_to_remove[set].insert(session->get_identifier());
			continue;
		}

		pump_status = session->pump_from_client();
		switch(pump_status){
			case SessionPumpStatus::TIMEOUT:
			case SessionPumpStatus::CONNECTED:
			case SessionPumpStatus::SUCCESS:
				break;
			case SessionPumpStatus::SOCKET_CLOSED:
			case SessionPumpStatus::BAD_DATA_SIZE:
				this->sessions_to_remove[set].insert(session->get_identifier());
				break;
		}

		for(auto &send : session->get_send_list()){
			auto send_list_by_session_name = this->send_list[set].find(send.session_name);
			if (send_list_by_session_name != this->send_list[set].end()){
				send_list_by_session_name->second.push_back(send);
			}else{
				std::list<SendListItem> new_list;
				new_list.push_back(send);
				this->send_list[set].insert_or_assign(send.session_name, new_list);
			}
		}
	}
}

void Server::pump_to_clients(int set){
	for(auto &session : this->sessions_to_pump[set]){
		std::string identifier = session->get_identifier();

		DataQueueStatus queue_status = DataQueueStatus::SUCCESS;
		for(auto &send_list_set : this->send_list){
			auto my_send_list = send_list_set.find(identifier);
			if (my_send_list == send_list_set.end()){
				continue;
			}
			for(auto &send : my_send_list->second){
				if (send.session_name == identifier){
					DataQueueStatus queue_status = session->queue_send(send.data);
					if (queue_status == DataQueueStatus::MAX_DATA_REACHED){
						sessions_to_remove[set].insert(identifier);
						break;
					}
				}
			}
			if (queue_status == DataQueueStatus::MAX_DATA_REACHED){
				break;
			}
		}
		if (queue_status == DataQueueStatus::MAX_DATA_REACHED){
			continue;
		}

		SessionPumpStatus pump_status = session->pump_to_client();
		if (pump_status == SessionPumpStatus::SOCKET_CLOSED){
			sessions_to_remove[set].insert(identifier);
			break;
		}
	}
}

ServerPumpStatus Server::pump(){
	if (this->sock_fd == -1){
		return ServerPumpStatus::LISTEN_SOCK_DEAD;
	}

	// create pending sessions from accept
	while(true){
		std::string peer_addr;
		uint16_t peer_port;
		int accept_status = native_accept(this->sock_fd, &peer_addr, &peer_port);

		if (accept_status == -1){
			int error = native_get_last_socket_error();
			if (native_error_is_would_block(error)){
				break;
			}
			if (native_error_is_emfile(error)){
				LOG("%s: warning, new connection dropped as system limit has reached\n", __func__);
				break;
			}
			LOG("%s: failed accepting connection, 0x%x\n", __func__, error);
			native_close(this->sock_fd);
			this->sock_fd = -1;
			return ServerPumpStatus::LISTEN_SOCK_DEAD;
		}

		if (this->pending_sessions.size() + this->sessions.size() >= this->config.max_num_sessions){
			LOG("%s: session limit %d reached, rejecting connection from %s\n", __func__, this->config.max_num_sessions, peer_addr.c_str());
			native_close(accept_status);
			continue;
		}

		pending_sessions.push_back(PendingSession(accept_status, peer_addr, &this->config));
	}

	// pump pending sessions in workers
	for(auto &to_pump_pending_session_set : this->to_pump_pending_sessions){
		to_pump_pending_session_set.clear();
	}
	for(auto &pending_sessions_to_remove_set : this->pending_sessions_to_remove){
		pending_sessions_to_remove_set.clear();
	}
	for(auto &ready_pending_sessions_set : this->ready_pending_sessions){
		ready_pending_sessions_set.clear();
	}

	int i = 0;
	for(auto &pending_session : this->pending_sessions){
		int worker = i % this->pending_sessions_pump_workers.size();
		this->to_pump_pending_sessions[worker].push_back(&pending_session);
		i++;
	}

	for(auto &sema : this->pending_sessions_pump_worker_semas){
		sema.release();
	}
	for(auto &sema : this->pending_sessions_pump_worker_done_semas){
		sema.acquire();
	}

	// process pumping result
	for(auto &ready_pending_sessions_set : this->ready_pending_sessions){
		for(auto &ready_session : ready_pending_sessions_set){
			Session new_session = ready_session->create_session(this->sessions);
			std::string identifier = new_session.get_identifier();
			auto old_session = this->sessions.find(identifier);
			if (old_session != this->sessions.end()){
				LOG("%s: replacing session %s from %s by session from %s\n", __func__, identifier.c_str(), old_session->second.get_client_addr().c_str(), new_session.get_client_addr().c_str());
				old_session->second.close_socket();
			}
			this->sessions.insert_or_assign(identifier, new_session);
			this->pending_sessions_to_remove[0].push_back(ready_session);
		}
	}

	for(auto &pending_sessions_to_remove_set : this->pending_sessions_to_remove){
		for(auto &to_remove_pending_session : pending_sessions_to_remove_set){
			for(auto itr = this->pending_sessions.begin();itr != this->pending_sessions.end();itr++){
				if (&(*itr) == to_remove_pending_session){
					this->pending_sessions.erase(itr);
					break;
				}
			}
		}
	}

	// pump data from clients
	for(auto &sessions_to_pump_set : this->sessions_to_pump){
		sessions_to_pump_set.clear();
	}
	for(auto &sessions_to_remove_set : this->sessions_to_remove){
		sessions_to_remove_set.clear();
	}

	i = 0;
	for(auto &session : this->sessions){
		int worker = i % this->session_from_client_pump_workers.size();
		this->sessions_to_pump[worker].push_back(&(session.second));
		i++;
	}

	for(auto &sema : this->session_from_client_pump_worker_semas){
		sema.release();
	}

	for(auto &sema : this->session_from_client_pump_worker_done_semas){
		sema.acquire();
	}

	// pump data top clients
	for(auto &sema : this->session_to_client_pump_worker_semas){
		sema.release();
	}

	for(auto &sema : this->session_to_client_pump_worker_done_semas){
		sema.acquire();
	}

	// clear send list
	for(auto &send_list_set : this->send_list){
		send_list_set.clear();
	}	

	// remove sessions
	for(auto &sessions_to_remove_set : this->sessions_to_remove){
		for(auto &session_name : sessions_to_remove_set){
			auto session = this->sessions.find(session_name);
			if (session == this->sessions.end()){
				continue;
			}
			session->second.close_socket();
			std::string peer_identifier = session->second.get_peer_identifier();
			LOG("%s: removing %s of %s\n", __func__, session_name.c_str(), session->second.get_client_addr().c_str());
			this->sessions.erase(session);
			if (peer_identifier != std::string("")){
				auto peer_session = this->sessions.find(peer_identifier);
				if (peer_session != this->sessions.end()){
					peer_session->second.close_socket();
					LOG("%s: removing %s of %s by peer relation\n", __func__, peer_identifier.c_str(), peer_session->second.get_client_addr().c_str());
					this->sessions.erase(peer_session);
				}
			}
		}
	}

	return ServerPumpStatus::SUCCESS;
}

}
