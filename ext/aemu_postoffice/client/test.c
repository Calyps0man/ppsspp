#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <pthread.h>
#include <unistd.h>

#include "postoffice_client.h"

#define LOG(...) { \
	fprintf(stderr, __VA_ARGS__); \
}

#define SERVER_PORT 27313
#define TARGET_INADDR (127 << 24 | 0 << 16 | 0 << 8 | 1)

#ifndef htonl
uint32_t htonl(uint32_t host){
	uint32_t ret;
	uint8_t *_ret = (uint8_t *)&ret;
	uint8_t *_host = (uint8_t *)&host;

	_ret[0] = _host[3];
	_ret[1] = _host[2];
	_ret[2] = _host[1];
	_ret[3] = _host[0];

	return ret;
}
#endif

#ifndef htons
uint16_t htons(uint16_t host){
	uint16_t ret;
	uint8_t *_ret = (uint8_t *)&ret;
	uint8_t *_host = (uint8_t *)&host;

	_ret[0] = _host[1];
	_ret[1] = _host[0];

	return ret;
}
#endif

void test_pdp(){
	struct aemu_post_office_sock_addr local_addr = {
		.addr = htonl(TARGET_INADDR),
		.port = htons(SERVER_PORT)
	};
	static const char pdp_mac_a[6] = {0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33};
	static const char pdp_mac_b[6] = {0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33};
	static const char pdp_mac_c[6] = {0xcc, 0xdd, 0xee, 0x11, 0x22, 0x33};
	static const int port_a = 12345;
	static const int port_b = 23456;
	static const int port_c = 34567;

	int state;
	void *pdp_handle_a_replace = pdp_create_v4(&local_addr, pdp_mac_a, port_a, &state);
	if (pdp_handle_a_replace == NULL){
		LOG("%s: failed creating pdp socket\n", __func__);
		exit(1);
	}

	// just so we know the last socket is the one gets replaced
	sleep(1);

	void *pdp_handle_a = pdp_create_v4(&local_addr, pdp_mac_a, port_a, &state);

	if (pdp_handle_a == NULL){
		LOG("%s: failed creating pdp socket\n", __func__);
		exit(1);
	}

	if (!pdp_send_buf_not_full(pdp_handle_a)){
		LOG("%s: pdp send buffer is somehow already full\n", __func__);
		exit(1);
	}

	if (pdp_is_dead(pdp_handle_a)){
		LOG("%s: pdp session is somehow dead already\n", __func__);
		exit(1);
	}

	sleep(1);
	if (!pdp_is_dead(pdp_handle_a_replace)){
		LOG("%s: pdp session is somehow not dead yet\n", __func__);
		exit(1);
	}
	int dead_len = 0;
	int dead_status = pdp_recv(pdp_handle_a_replace, NULL, NULL, NULL, &dead_len, false);
	if (dead_status != AEMU_POSTOFFICE_CLIENT_SESSION_DEAD){
		LOG("%s: expected dead session is not dead!\n", __func__);
		exit(1);
	}

	sleep(1);
	pdp_delete(pdp_handle_a_replace);

	void *pdp_handle_b = pdp_create_v4(&local_addr, pdp_mac_b, port_b, &state);
	if (pdp_handle_b == NULL){
		LOG("%s: failed creating pdp socket\n", __func__);
		exit(1);
	}
	void *pdp_handle_c = pdp_create_v4(&local_addr, pdp_mac_c, port_c, &state);
	if (pdp_handle_c == NULL){
		LOG("%s: failed creating pdp socket\n", __func__);
		exit(1);
	}

	sleep(1);

	for (int i = 0;i < 5;i++){
		// make sure to test the ring buffers
		char test_data[AEMU_POSTOFFICE_PDP_BLOCK_MAX / 3];
		for (int j = 0;j < sizeof(test_data);j++){
			test_data[j] = i + j;
		}
		int send_status = 0;
		for (int j = 0;j < 2;j++){
			int send_status = pdp_send(pdp_handle_a, pdp_mac_b, port_b, test_data, sizeof(test_data), false);
			if (send_status != 0){
				LOG("%s: failed sending pdp packet from a to b, %d\n", __func__, send_status);
				exit(1);
			}
		}

		send_status = pdp_send(pdp_handle_b, pdp_mac_c, port_c, test_data, sizeof(test_data), false);
		if (send_status != 0){
			LOG("%s: failed sending pdp packet from b to c, %d\n", __func__, send_status);
			exit(1);
		}

		sleep(1);

		int buffered_size = pdp_buffered_data_size(pdp_handle_b);
		if (buffered_size != sizeof(test_data) * 2){
			LOG("%s: failed getting buffered data size, expected %lu, got %d\n", __func__, sizeof(test_data) * 2, buffered_size);
			exit(1);
		}

		char recv_buf[sizeof(test_data)];
		char incoming_mac[6];
		int incoming_port;
		int len = 0;
		int recv_status = 0;
		for (int j = 0;j < 2;j++){
			int next_size = pdp_peek_next_size(pdp_handle_b);
			if (next_size != sizeof(test_data)){
				LOG("%s: failed peeking pdp size, expected %lu, got %d\n", __func__, sizeof(test_data), next_size);
				exit(1);
			}
			len = sizeof(recv_buf);
			int recv_status = pdp_recv(pdp_handle_b, incoming_mac, &incoming_port, recv_buf, &len, false);
			if (recv_status != 0){
				LOG("%s: receive failed from a to b, %d\n", __func__, recv_status);
				exit(1);
			}

			if (len != sizeof(recv_buf)){
				LOG("%s: bad length of received data from a to b\n", __func__);
				exit(1);
			}

			if (memcmp(incoming_mac, pdp_mac_a, 6) != 0){
				LOG("%s: bad mac address from a to b\n", __func__);
				exit(1);
			}

			if (incoming_port != port_a){
				LOG("%s: bad incoming port %d from a to b, expected %d\n", __func__, incoming_port, port_a);
				exit(1);
			}

			if (memcmp(test_data, recv_buf, sizeof(test_data)) != 0){
				LOG("%s: bad data received from a to b:\n", __func__);
				for(int i = 0;i < sizeof(test_data);i++){
					LOG("%d ", recv_buf[i]);
				}
				LOG("\n");
				exit(1);
			}
		}

		len = sizeof(recv_buf);
		recv_status = pdp_recv(pdp_handle_c, incoming_mac, &incoming_port, recv_buf, &len, false);
		if (recv_status != 0){
			LOG("%s: receive failed from b to c, %d\n", __func__, recv_status);
			exit(1);
		}

		if (len != sizeof(recv_buf)){
			LOG("%s: bad length of received data from b to c\n", __func__);
			exit(1);
		}

		if (memcmp(incoming_mac, pdp_mac_b, 6) != 0){
			LOG("%s: bad mac address from b to c\n", __func__);
			exit(1);
		}

		if (incoming_port != port_b){
			LOG("%s: bad incoming port %d from b to c, expected %d\n", __func__, incoming_port, port_b);
			exit(1);
		}

		if (memcmp(test_data, recv_buf, sizeof(test_data)) != 0){
			LOG("%s: bad data received from b to c:\n", __func__);
			for(int i = 0;i < sizeof(test_data);i++){
				LOG("%d ", recv_buf[i]);
			}
			LOG("\n");
			exit(1);
		}

		sleep(1);

		send_status = pdp_send(pdp_handle_a, pdp_mac_c, port_c, test_data, sizeof(test_data), false);
		if (send_status != 0){
			LOG("%s: failed sending pdp packet from a to c, %d\n", __func__, send_status);
			exit(1);
		}

		len = sizeof(recv_buf);
		recv_status = pdp_recv(pdp_handle_c, incoming_mac, &incoming_port, recv_buf, &len, false);
		if (recv_status != 0){
			LOG("%s: receive failed from a to c, %d\n", __func__, recv_status);
			exit(1);
		}

		if (len != sizeof(recv_buf)){
			LOG("%s: bad length of received data from a to c\n", __func__);
			exit(1);
		}

		if (memcmp(incoming_mac, pdp_mac_a, 6) != 0){
			LOG("%s: bad mac address from a to c\n", __func__);
			exit(1);
		}

		if (incoming_port != port_a){
			LOG("%s: bad incoming port %d from a to c, expected %d\n", __func__, incoming_port, port_a);
			exit(1);
		}

		if (memcmp(test_data, recv_buf, sizeof(test_data)) != 0){
			LOG("%s: bad data received from a to c:\n", __func__);
			for(int i = 0;i < sizeof(test_data);i++){
				LOG("%d ", recv_buf[i]);
			}
			LOG("\n");
			exit(1);
		}

		len = sizeof(recv_buf);
		recv_status = pdp_recv(pdp_handle_c, incoming_mac, &incoming_port, recv_buf, &len, true);
		if (recv_status != AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
			LOG("%s: expected would block status for recv, got %d instead\n", __func__, recv_status);
			exit(1);
		}

		send_status = pdp_send(pdp_handle_c, pdp_mac_a, port_a, test_data, sizeof(test_data), true);
		if (send_status != 0){
			LOG("%s: failed sending pdp packet from c to a, %d\n", __func__, send_status);
			exit(1);
		}

		sleep(1);

		len = sizeof(recv_buf);
		recv_status = pdp_recv(pdp_handle_a, incoming_mac, &incoming_port, recv_buf, &len, true);
		if (recv_status != 0){
			LOG("%s: receive failed from c to a, %d\n", __func__, recv_status);
			exit(1);
		}

		if (len != sizeof(recv_buf)){
			LOG("%s: bad length of received data from c to a\n", __func__);
			exit(1);
		}

		if (memcmp(incoming_mac, pdp_mac_c, 6) != 0){
			LOG("%s: bad mac address from c to a\n", __func__);
			exit(1);
		}

		if (incoming_port != port_c){
			LOG("%s: bad incoming port %d from c to a, expected %d\n", __func__, incoming_port, port_c);
			exit(1);
		}

		if (memcmp(test_data, recv_buf, sizeof(test_data)) != 0){
			LOG("%s: bad data received from c to a:\n", __func__);
			for(int i = 0;i < sizeof(test_data);i++){
				LOG("%d ", recv_buf[i]);
			}
			LOG("\n");
			exit(1);
		}
	}

	pdp_delete(pdp_handle_a);
	pdp_delete(pdp_handle_b);
	pdp_delete(pdp_handle_c);
}

struct connection_accept_task{
	int expected_port;
	void *listen_handle;
	char expected_mac[6];
};

void *accept_ptp_connection(void *arg){
	struct connection_accept_task *task = (struct connection_accept_task *)arg;
	int state = 0;
	int port = 0;
	char mac[6];

	sleep(1);
	int has_listen_request = ptp_listen_has_request(task->listen_handle);
	if (has_listen_request == 0){
		LOG("%s: listen request expceted but is not ready\n", __func__);
		exit(1);
	}

	void *accept_handle = ptp_accept(task->listen_handle, mac, &port, false, &state);

	if (port != task->expected_port){
		LOG("%s: got port %d, expected %d\n", __func__, port, task->expected_port);
		exit(1);
	}

	if (memcmp(mac, task->expected_mac, 6) != 0){
		LOG("%s: bad mac address while accepting connection\n", __func__);
		exit(1);
	}

	return accept_handle;
}

void test_ptp(){
	struct aemu_post_office_sock_addr local_addr = {
		.addr = htonl(TARGET_INADDR),
		.port = htons(SERVER_PORT)
	};
	static const char ptp_mac_a[6] = {0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33};
	static const char ptp_mac_b[6] = {0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33};
	static const int port_a = 12345;
	static const int port_b = 23456;

	int state;
	void *listen_handle_a = ptp_listen_v4(&local_addr, ptp_mac_a, port_a, &state);
	if (listen_handle_a == NULL){
		LOG("%s: failed opening listen handle for a\n", __func__);
		exit(1);
	}

	if (ptp_listen_is_dead(listen_handle_a)){
		LOG("%s: listen session for a is somehow already dead\n", __func__);
		exit(1);
	}

	if (ptp_listen_is_dead(listen_handle_a)){
		LOG("%s: a's listen handle is somehow already dead\n", __func__);
		exit(1);
	}

	void *listen_handle_b = ptp_listen_v4(&local_addr, ptp_mac_b, port_b, &state);
	if (listen_handle_b == NULL){
		LOG("%s: failed opening listen handle for b\n", __func__);
		exit(1);
	}

	int port;
	char mac[6];
	ptp_accept(listen_handle_a, mac, &port, true, &state);
	if (state != AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
		LOG("%s: unexpected state %d on non block accept\n", __func__, state);
		exit(1);
	}

	int has_listen_request = ptp_listen_has_request(listen_handle_a);
	if (has_listen_request != 0){
		LOG("%s: listen handle a unexpectly already has a listen request, %d\n", __func__, has_listen_request);
		exit(1);
	}

	ptp_accept(listen_handle_b, mac, &port, true, &state);
	if (state != AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
		LOG("%s: unexpected state %d on non block accept\n", __func__, state);
		exit(1);
	}

	has_listen_request = ptp_listen_has_request(listen_handle_b);
	if (has_listen_request != 0){
		LOG("%s: listen handle b unexpectly already has a listen request, %d\n", __func__, has_listen_request);
		exit(1);
	}

	pthread_t accept_thread_a;
	pthread_t accept_thread_b;
	struct connection_accept_task task_a = {
		.listen_handle = listen_handle_a,
		.expected_port = port_b
	};
	memcpy(task_a.expected_mac, ptp_mac_b, 6);
	struct connection_accept_task task_b = {
		.listen_handle = listen_handle_b,
		.expected_port = port_a
	};
	memcpy(task_b.expected_mac, ptp_mac_a, 6);


	pthread_create(&accept_thread_b, NULL, accept_ptp_connection, &task_b);
	void *a_to_b = ptp_connect_v4(&local_addr, ptp_mac_a, port_a, ptp_mac_b, port_b, &state);
	if (state != AEMU_POSTOFFICE_CLIENT_OK){
		LOG("%s: failed connecting from a to b\n", __func__);
		exit(1);
	}

	if (ptp_is_dead(a_to_b)){
		LOG("%s: a connect to be handle is somehow already dead\n", __func__);
		exit(1);
	}

	pthread_create(&accept_thread_a, NULL, accept_ptp_connection, &task_a);
	void *b_to_a = ptp_connect_v4(&local_addr, ptp_mac_b, port_b, ptp_mac_a, port_a, &state);
	if (state != AEMU_POSTOFFICE_CLIENT_OK){
		LOG("%s: failed connecting from b to a\n", __func__);
		exit(1);
	}

	void *accept_handle_a;
	void *accept_handle_b;
	pthread_join(accept_thread_a, &accept_handle_a);
	pthread_join(accept_thread_b, &accept_handle_b);

	if (!ptp_send_buf_not_full(a_to_b)){
		LOG("%s: ptp send buffer is somehow already full\n", __func__);
		exit(1);
	}

	if (accept_handle_a == NULL){
		LOG("%s: failed accepting connection from b to a\n", __func__);
		exit(1);
	}

	if (accept_handle_b == NULL){
		LOG("%s: failed accepting connection from a to b\n", __func__);
		exit(1);
	}

	char test_data[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	char recv_buf[sizeof(test_data)];
	int recv_size = sizeof(recv_buf);
	int recv_status = ptp_recv(accept_handle_a, recv_buf, &recv_size, true);

	if (recv_status != AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
		LOG("%s: unexpected state on non block receive\n", __func__);
		exit(1);
	}

	recv_size = sizeof(recv_buf);
	recv_status = ptp_recv(accept_handle_b, recv_buf, &recv_size, true);

	if (recv_status != AEMU_POSTOFFICE_CLIENT_SESSION_WOULD_BLOCK){
		LOG("%s: unexpected state on non block receive\n", __func__);
		exit(1);
	}

	for (int i = 0; i < 5;i++){
		// make sure to test the ring buffer
		char test_data[AEMU_POSTOFFICE_PTP_BLOCK_MAX / 3];
		for (int j = 0;j < sizeof(test_data);j++){
			test_data[j] = j + i;
		}
		char recv_buf[sizeof(test_data)];

		int send_status = ptp_send(accept_handle_a, test_data, sizeof(test_data), false);
		if (send_status != 0){
			LOG("%s: failed sending from a accept to b\n", __func__);
			exit(1);
		}

		recv_size = sizeof(recv_buf);
		recv_status = ptp_recv(b_to_a, recv_buf, &recv_size, false);
		if (recv_status != 0){
			LOG("%s: failed receiving from a accept to b\n", __func__);
			exit(1);
		}
		if (memcmp(recv_buf, test_data, sizeof(test_data)) != 0){
			LOG("%s: receiving from a accept to b has bad data\n", __func__);
			exit(1);
		}
		if (recv_size != sizeof(recv_buf)){
			LOG("%s: receiving from a accept to b has bad data size\n", __func__);
			exit(1);
		}

		send_status = ptp_send(b_to_a, test_data, sizeof(test_data), false);
		if (send_status != 0){
			LOG("%s: failed sending from b connect to a\n", __func__);
			exit(1);
		}


		sleep(1);
		int next_size = ptp_peek_next_size(accept_handle_a);
		if (next_size != sizeof(recv_buf)){
			LOG("%s: failed peeking ptp size, expected %lu, got %d\n", __func__, sizeof(recv_buf), next_size);
			exit(1);
		}

		recv_size = sizeof(recv_buf);
		recv_status = ptp_recv(accept_handle_a, recv_buf, &recv_size, false);
		if (recv_status != 0){
			LOG("%s: failed receiving from b connect to a\n", __func__);
			exit(1);
		}
		if (memcmp(recv_buf, test_data, sizeof(test_data)) != 0){
			LOG("%s: receiving from b connect to b has bad data\n", __func__);
			exit(1);
		}
		if (recv_size != sizeof(recv_buf)){
			LOG("%s: receiving from b connect to b has bad data size\n", __func__);
			exit(1);
		}


		send_status = ptp_send(accept_handle_b, test_data, sizeof(test_data), false);
		if (send_status != 0){
			LOG("%s: failed sending from b accept to a\n", __func__);
			exit(1);
		}

		recv_size = sizeof(recv_buf);
		recv_status = ptp_recv(a_to_b, recv_buf, &recv_size, false);
		if (recv_status != 0){
			LOG("%s: failed receiving from a connect to b\n", __func__);
			exit(1);
		}
		if (memcmp(recv_buf, test_data, sizeof(test_data)) != 0){
			LOG("%s: receiving from a connect to b has bad data\n", __func__);
			exit(1);
		}
		if (recv_size != sizeof(recv_buf)){
			LOG("%s: receiving from a connect to b has bad data size\n", __func__);
			exit(1);
		}

		send_status = ptp_send(a_to_b, test_data, sizeof(test_data), false);
		if (send_status != 0){
			LOG("%s: failed sending from a connect to b\n", __func__);
			exit(1);
		}

		for (int j = 0;j < sizeof(recv_buf);j++){
			recv_size = 1;
			recv_status = ptp_recv(accept_handle_b, &recv_buf[j], &recv_size, false);
			if (recv_status != 0 && recv_status != AEMU_POSTOFFICE_CLIENT_SESSION_DATA_TRUNC){
				LOG("%s: failed receiving from a connect to b, byte %d\n", __func__, j);
				exit(1);
			}
			if (recv_size != 1){
				LOG("%s: receiving from a connect to b has bad data size, byte %d\n", __func__, j);
				exit(1);
			}
		}

		if (memcmp(recv_buf, test_data, sizeof(test_data)) != 0){
			LOG("%s: receiving from a connect to b has bad data\n", __func__);
			exit(1);
		}

		send_status = ptp_send(a_to_b, test_data, sizeof(test_data), false);
		if (send_status != 0){
			LOG("%s: failed sending from a connect to b\n", __func__);
			exit(1);
		}
		send_status = ptp_send(a_to_b, test_data, sizeof(test_data), false);
		if (send_status != 0){
			LOG("%s: failed sending from a connect to b\n", __func__);
			exit(1);
		}

		sleep(1);
		next_size = ptp_peek_next_size(accept_handle_b);
		if (next_size != sizeof(test_data) * 2){
			LOG("%s: failed peeking ptp size during stream test, expected %lu, got %d\n", __func__, sizeof(test_data) * 2, next_size);
			exit(1);
		}
		char recv_buf_2[sizeof(recv_buf) * 2];
		recv_size = sizeof(recv_buf_2);
		recv_status = ptp_recv(accept_handle_b, recv_buf_2, &recv_size, false);
		if (recv_status != 0){
			LOG("%s: failed receiving from b accept to a\n", __func__);
			exit(1);
		}
		if (recv_size != sizeof(recv_buf_2)){
			LOG("%s: receiving from b accept to a has bad data size\n", __func__);
			exit(1);
		}
		if (memcmp(recv_buf_2, test_data, sizeof(test_data)) != 0){
			LOG("%s: receiving from b accept to a has bad data\n", __func__);
			exit(1);
		}
		if (memcmp(&recv_buf_2[sizeof(test_data)], test_data, sizeof(test_data)) != 0){
			LOG("%s: receiving from b accept to a has bad data\n", __func__);
			exit(1);
		}
	}

	ptp_close(a_to_b);
	ptp_close(b_to_a);

	sleep(2);

	ptp_close(accept_handle_a);
	ptp_close(accept_handle_b);
	
	ptp_listen_close(listen_handle_a);
	ptp_listen_close(listen_handle_b);
};

int main(){
	aemu_post_office_init();

	test_pdp();
	test_ptp();
	LOG("%s: test ok\n", __func__);
	return 0;
}
