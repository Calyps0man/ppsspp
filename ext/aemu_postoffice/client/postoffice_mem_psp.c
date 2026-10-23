#include "postoffice_mem.h"
#include "log_impl.h"

#include <pspsysmem.h>

#define SESSIONS_PER_TYPE 4
#define SESSIONS_PER_TYPE_HIGHMEM 16

int NUM_PDP_SESSIONS = 0;
int NUM_PTP_LISTEN_SESSIONS = 0;
int NUM_PTP_SESSIONS = 0;

struct pdp_session *pdp_sessions = NULL;
struct ptp_listen_session *ptp_listen_sessions = NULL;
struct ptp_session *ptp_sessions = NULL;

int has_high_mem();
int partition_to_use();

void init_postoffice_mem(){
	int sessions_per_type = SESSIONS_PER_TYPE_HIGHMEM;
	if (!has_high_mem()){
		sessions_per_type = SESSIONS_PER_TYPE;
	}
	LOG("%s: allocating for %d sessions per type\n", __func__, sessions_per_type);

	SceUID uid_pdp = sceKernelAllocPartitionMemory(partition_to_use(), "postoffice_pdp_sessions", 4 /* high aligned */, sessions_per_type * sizeof(struct pdp_session), (void *)4 /* alignment */);
	SceUID uid_ptp_listen = sceKernelAllocPartitionMemory(partition_to_use(), "postoffice_ptp_listen_sessions", 4 /* high aligned */, sessions_per_type * sizeof(struct ptp_listen_session), (void *)4 /* alignment */);
	SceUID uid_ptp = sceKernelAllocPartitionMemory(partition_to_use(), "postoffice_ptp_sessions", 4 /* high aligned */, sessions_per_type * sizeof(struct ptp_session), (void *)4 /* alignment */);

	if (uid_pdp >= 0){
		pdp_sessions = sceKernelGetBlockHeadAddr(uid_pdp);
		NUM_PDP_SESSIONS = sessions_per_type;
	}else{
		LOG("%s: failed allocating memory for pdp sessions, 0x%x\n", __func__, uid_pdp);
	}

	if (uid_ptp_listen >= 0){
		ptp_listen_sessions = sceKernelGetBlockHeadAddr(uid_ptp_listen);
		NUM_PTP_LISTEN_SESSIONS = sessions_per_type;
	}else{
		LOG("%s: failed allocating memory for ptp listen sessions, 0x%x\n", __func__, uid_ptp_listen);
	}

	if (uid_ptp >= 0){
		ptp_sessions = sceKernelGetBlockHeadAddr(uid_ptp);
		NUM_PTP_SESSIONS = sessions_per_type;
	}else{
		LOG("%s: failed allocating memory for ptp sessions, 0x%x\n", __func__, uid_ptp);
	}
}
