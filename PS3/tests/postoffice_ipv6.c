#define CHECK(condition) do { if (!(condition)) return 1; } while (0)
#include <stdio.h>
#include "postoffice_client.h"
int main(void) {
	struct aemu_post_office_sock6_addr address = {{0}, 0};
	int state = 123;
	CHECK(pdp_create_v6(&address, "123456", 1, &state) == NULL);
	CHECK(state == AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK);
	state = 123;
	CHECK(ptp_listen_v6(&address, "123456", 1, &state) == NULL);
	CHECK(state == AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK);
	state = 123;
	CHECK(ptp_connect_v6(&address, "123456", 1, "abcdef", 2, &state) == NULL);
	CHECK(state == AEMU_POSTOFFICE_CLIENT_SESSION_NETWORK);
	CHECK(pdp_create_v6(&address, "123456", 1, NULL) == NULL);
	puts("PASS: three native postoffice IPv6 entry points explicitly fail, including null state (PPU objects, Linux PowerPC QEMU)");
	return 0;
}
