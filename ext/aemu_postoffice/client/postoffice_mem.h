#ifndef __POSTOFFICE_MEM_H
#define __POSTOFFICE_MEM_H

#include <stdint.h>
#include "sock_impl.h"

#include "postoffice_client.h"
#include "../aemu_postoffice_packets.h"

struct pdp_session{
	char *pdp_mac[6];
	int16_t pdp_port;
	int sock;
	bool dead;
	bool abort;
	char recv_ring_buf[AEMU_POSTOFFICE_PDP_BLOCK_MAX + sizeof(aemu_postoffice_pdp)];
	int recv_ring_buf_start;
	int recv_ring_buf_used;
	int buffered_data;
	int bytes_till_next_header;
	int last_block_size;
	bool recving;
	bool sending;
};

struct ptp_listen_session{
	char *ptp_mac[6];
	int16_t ptp_port;
	int sock;
	bool dead;
	bool abort;
	char addr[sizeof(native_sock6_addr) > sizeof(native_sock_addr) ? sizeof(native_sock6_addr) : sizeof(native_sock_addr)];
	int addrlen;
	bool accepting;
};

struct ptp_session{
	int sock;
	bool dead;
	bool abort;
	char recv_ring_buf[AEMU_POSTOFFICE_PTP_BLOCK_MAX];
	int recv_ring_buf_start;
	int recv_ring_buf_used;
	int bytes_till_next_header;
	bool recving;
	bool sending;
};

#ifdef __cplusplus
extern "C" {
#endif

extern int NUM_PDP_SESSIONS;
extern int NUM_PTP_LISTEN_SESSIONS;
extern int NUM_PTP_SESSIONS;

extern struct pdp_session *pdp_sessions;
extern struct ptp_listen_session *ptp_listen_sessions;
extern struct ptp_session *ptp_sessions;

void init_postoffice_mem();

#ifdef __cplusplus
}
#endif

#endif
