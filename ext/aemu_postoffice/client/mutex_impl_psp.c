#include <pspthreadman.h>

static SceLwMutexWorkarea sock_alloc_mutex;
void init_sock_alloc_mutex(){
	sceKernelCreateLwMutex(&sock_alloc_mutex, "aemu_postoffice sock_alloc_mutex", 0, 0, NULL);
}
void lock_sock_alloc_mutex(){
	sceKernelLockLwMutex(&sock_alloc_mutex, 1, 0);
}
void unlock_sock_alloc_mutex(){
	sceKernelUnlockLwMutex(&sock_alloc_mutex, 1);
}

// generally we do not expect applications to trigger buffer -> ring buffer on the same socket on different threads
// however pspemu_inet_multithread used along with a few games will trigger that by yielding on nonblock inet functions, so guard it
static SceLwMutexWorkarea drain_mutex;
void init_drain_mutex(){
	sceKernelCreateLwMutex(&drain_mutex, "aemu_postoffice drain_mutex", 0, 0, NULL);
}
void lock_drain_mutex(){
	sceKernelLockLwMutex(&drain_mutex, 1, 0);
}
void unlock_drain_mutex(){
	sceKernelUnlockLwMutex(&drain_mutex, 1);
}
