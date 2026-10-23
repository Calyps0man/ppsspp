#include <cassert>
#include <cstdio>
#include <cstring>
#include "ext/libzip/zip.h"
extern "C" zip_uint32_t zip_random_uint32(void);
extern "C" bool zip_secure_random(zip_uint8_t *, zip_uint16_t);
static int calls,failCall;
static unsigned sizes[20];
extern "C" uint32_t sysGetRandomNumber(void *data,uint64_t size) {
	assert(size<=4096); sizes[calls++]=size;
	if(calls==failCall) return 1;
	memset(data,0xAB,size); return 0;
}
int main() {
	zip_uint8_t data[9000]{};
	assert(zip_secure_random(data,sizeof(data)) && calls==3);
	assert(sizes[0]==4096 && sizes[1]==4096 && sizes[2]==808);
	for(auto byte:data) assert(byte==0xAB);
	calls=0; failCall=2; assert(!zip_secure_random(data,sizeof(data)) && calls==2);
	assert(!zip_secure_random(nullptr,1)); assert(zip_secure_random(nullptr,0));
	calls=0; failCall=0; assert(zip_random_uint32()==0xABABABABu && calls==1);
	puts("PS3 ZIP randomness: real syscall adapter chunks, secure failure propagation and 32-bit result PASS (host API model)");
}
