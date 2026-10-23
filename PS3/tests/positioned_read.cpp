#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <unistd.h>
static off_t position;
static int seeks, reads, interrupted;
static bool failSeek, failRead;
static off_t mockSeek(int fd, off_t offset, int origin) {
	assert(fd==37 && origin==SEEK_SET); ++seeks;
	if(failSeek) return -1;
	position=offset; std::this_thread::yield(); return position;
}
static ssize_t mockRead(int fd, void *data, size_t count) {
	assert(fd==37); ++reads;
	if(interrupted) { --interrupted; errno=EINTR; return -1; }
	if(failRead) { errno=EIO; return -1; }
	if(count) { assert(count==sizeof(position)); memcpy(data,&position,count); }
	return count;
}
#define lseek mockSeek
#define read mockRead
#include "PS3/PositionedRead.h"
#undef lseek
#undef read
int main() {
	std::mutex mutex; off_t value=0;
	assert(PS3PositionedRead(37,0x100000001LL,1,sizeof(value),&value,mutex)==sizeof(value) && value==0x100000001LL);
	failSeek=true; int before=reads; assert(PS3PositionedRead(37,1,1,sizeof(value),&value,mutex)==0 && reads==before); failSeek=false;
	failRead=true; assert(PS3PositionedRead(37,1,1,sizeof(value),&value,mutex)==0); failRead=false;
	interrupted=1; assert(PS3PositionedRead(37,2,sizeof(value),1,&value,mutex)==1 && value==2);
	before=seeks; assert(PS3PositionedRead(37,-1,1,1,&value,mutex)==0);
	assert(PS3PositionedRead(37,0,0,1,&value,mutex)==0);
	assert(PS3PositionedRead(37,0,2,std::numeric_limits<size_t>::max(),&value,mutex)==0 && seeks==before);
	auto worker=[&](off_t offset) { for(int i=0;i<1000;++i) { off_t actual; assert(PS3PositionedRead(37,offset,1,sizeof(actual),&actual,mutex)==sizeof(actual) && actual==offset); } };
	std::thread a(worker,111),b(worker,222); a.join(); b.join();
	puts("PS3 positioned read: 64-bit offsets, seek/read errors, EINTR, overflow and concurrent reads PASS (host API model)");
}
