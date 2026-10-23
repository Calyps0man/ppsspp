#ifndef __AEMU_POSTOFFICE_PACKETS_H
#define __AEMU_POSTOFFICE_PACKETS_H

#include <stdint.h>

// current limits on send/recv calls
#define AEMU_POSTOFFICE_PDP_BLOCK_MAX (10 * 1024)
#define AEMU_POSTOFFICE_PTP_BLOCK_MAX (50 * 1024)

// structs here are gcc little endian

typedef enum{
	AEMU_POSTOFFICE_INIT_PDP = 0,
	AEMU_POSTOFFICE_INIT_PTP_LISTEN = 1,
	AEMU_POSTOFFICE_INIT_PTP_CONNECT = 2,
	AEMU_POSTOFFICE_INIT_PTP_ACCEPT = 3
} aemu_postoffice_init_type;

#pragma pack(push, 1)

// client -> server
typedef struct aemu_postoffice_init{
	int32_t init_type;
	// 2 bytes on padding on the end, only 6 bytes are used
	char src_addr[8];
	uint16_t sport;
	// 2 bytes on padding on the end
	char dst_addr[8];
	uint16_t dport;
} aemu_postoffice_init;

// client <-> server
typedef struct aemu_postoffice_pdp{
	// 2 bytes on padding on the end
	char addr[8];
	uint16_t port;
	uint32_t size;
} aemu_postoffice_pdp;

// server -> client
typedef struct aemu_postoffice_ptp_connect{
	// 2 bytes on padding on the end
	char addr[8];
	uint16_t port;
} aemu_postoffice_ptp_connect;

typedef struct aemu_postoffice_ptp_data{
	uint32_t size;
} aemu_postoffice_ptp_data;

#pragma pack(pop)

#endif
