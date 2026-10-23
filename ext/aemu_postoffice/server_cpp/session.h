	#ifndef __SESSION_H
#define __SESSION_H

#include <stdint.h>

#include <string>
#include <vector>
#include <chrono>
#include <map>

#include "config.h"

namespace aemu_postoffice_server {

class Session;

enum class PendingSessionPumpStatus{
	SUCCESS,
	SOCKET_CLOSED, // socket is closed on this
	SESSION_READY, // create_session needs to be called right after, before changes happen to the global session list
	TIMEOUT, // socket is also closed on this
	BAD_INIT, // socket is also closed on this
};

class PendingSession{
	public:
		PendingSession(int sock_fd, std::string client_addr, Config *config);
		PendingSessionPumpStatus pump(std::map<std::string, Session> &global_sessions);
		// The pending session should be discarded after creating a session
		Session create_session(std::map<std::string, Session> &global_sessions);
		void close_socket();

	private:
		std::string init_data_buffer;
		int sock_fd;
		std::chrono::high_resolution_clock::time_point create_time;
		std::string client_addr;
		Config *config;
};

enum class SessionMode{
	PDP,
	PTP_LISTEN,
	PTP_CONNECT,
	PTP_ACCEPT,
};

enum class DataQueueStatus{
	SUCCESS,
	MAX_DATA_REACHED,
};

struct SendListItem{
	std::string session_name;
	std::string data;
};

enum class SessionPumpStatus{
	SUCCESS,	
	SOCKET_CLOSED,
	BAD_DATA_SIZE,
	TIMEOUT, // only happens when pumping a connect session
	CONNECTED, // only happens when pumping a connect session
};

enum class SessionPhase{
	PTP_CONNECTING,
	HEADER,
	DATA,
};

class Session{
	public:
		Session(SessionMode mode, char *from_mac, uint16_t from_port, char *to_mac, uint16_t to_port, std::string initial_data_buffer, int sock_fd, Session *peer_session, std::string client_addr, Config *config);
		~Session();
		SessionPumpStatus pump_connect(); // 0. connect session has to be pumped until a connect accept pair is formed
		SessionPumpStatus pump_from_client(); // 1. fetch data from client socket into buffer and put read data into send list
		std::vector<SendListItem> get_send_list(); // 2. fetch the send list
		DataQueueStatus queue_send(const std::string &data); // 3. queue data to be sent to client
		SessionPumpStatus pump_to_client(); // 4. pump data into client socket
		std::string get_identifier(); // session name for logging and hashing
		std::string get_peer_identifier(); // for ptp only
		SessionMode get_session_mode();
		SessionPhase get_session_phase();
		std::string get_client_addr();
		void close_socket();

	protected:
		// processed data from client to be sent to other sessions
		std::vector<SendListItem> send_list;

		// session identifier
		char from_mac[6];
		uint16_t from_port;
		char to_mac[6];
		uint16_t to_port;

		SessionPhase phase;
	private:
		SessionMode mode;

		Config *config;

		// data buffers
		std::string from_client_data_buffer;
		std::string to_client_data_buffer;
		int sock_fd;

		// mostly for detecting connection timeout
		std::chrono::high_resolution_clock::time_point create_time;

		// data processing
		std::string pdp_data_target;
		uint64_t data_size;

		std::string client_addr;
};

}
// Session loop phases
// 1. accept connections to spawn new pending sessions
// 2. Process pending sessions to spawn new sessions/remove timed out pending sessions
// 3. pump ptp connect sessions to check for timeout/accept, tag removal if timeout
// 4. pump data from client, tag session for removal if socket close/error
// 5. collect send list from clients
// 6. queue send to clients, tag session for removal if socket close/error
// 7. pump send to clients, tag for removal if socket close/error
// 8. remove sessions that are tagged for removal
// 9. rest based on target tick rate and time spent

#endif
