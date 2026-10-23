#include <pspsdk.h>

#include <stdint.h>

#include "postoffice_client.h"

#define MODULE_NAME "aemu_postoffice_client"

PSP_MODULE_INFO(MODULE_NAME, PSP_MODULE_USER + 6, 1, 4);

int module_start(SceSize args, void *argp){
	aemu_post_office_init();
	return 0;
}

int module_stop(SceSize args, void *argp){
	return 0;
}

uint8_t *memcpy(uint8_t *dst, const uint8_t *src, int size){
	for(int i = 0;i < size;i++){
		dst[i] = src[i];
	}
	return dst;
}
