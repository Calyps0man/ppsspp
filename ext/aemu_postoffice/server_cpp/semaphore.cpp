#include "semaphore.h"

void Semaphore::acquire(){
	this->sema.acquire();
}

void Semaphore::release(){
	this->sema.release();
}
