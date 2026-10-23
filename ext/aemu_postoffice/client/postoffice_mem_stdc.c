#include "postoffice_mem.h"

#define SESSIONS_PER_TYPE 32

int NUM_PDP_SESSIONS = SESSIONS_PER_TYPE;
int NUM_PTP_LISTEN_SESSIONS = SESSIONS_PER_TYPE;
int NUM_PTP_SESSIONS = SESSIONS_PER_TYPE;

struct pdp_session _pdp_sessions[SESSIONS_PER_TYPE];
struct ptp_listen_session _ptp_listen_sessions[SESSIONS_PER_TYPE];
struct ptp_session _ptp_sessions[SESSIONS_PER_TYPE];

struct pdp_session *pdp_sessions = _pdp_sessions;
struct ptp_listen_session *ptp_listen_sessions = _ptp_listen_sessions;
struct ptp_session *ptp_sessions = _ptp_sessions;

void init_postoffice_mem(){
}
