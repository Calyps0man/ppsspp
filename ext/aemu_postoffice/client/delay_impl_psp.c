#include <pspthreadman.h>

void delay(int ms){
	sceKernelDelayThread(ms * 1000);
}
