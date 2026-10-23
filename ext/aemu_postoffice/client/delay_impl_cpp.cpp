#include <thread>
#include <chrono>

extern "C" {

void delay(int ms){
	auto duration = std::chrono::milliseconds(ms);
	std::this_thread::sleep_for(duration);
}

}
