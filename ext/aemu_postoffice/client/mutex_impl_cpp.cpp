#include <mutex>

extern "C" {

static std::mutex sock_alloc_mutex;
void init_sock_alloc_mutex(){
}
void lock_sock_alloc_mutex(){
	sock_alloc_mutex.lock();
}
void unlock_sock_alloc_mutex(){
	sock_alloc_mutex.unlock();
}

static std::mutex drain_mutex;
void init_drain_mutex(){
}
void lock_drain_mutex(){
	drain_mutex.lock();
}
void unlock_drain_mutex(){
	drain_mutex.unlock();
}

}
