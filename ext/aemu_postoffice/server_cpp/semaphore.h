#ifndef __SEMAPHORE_H
#define __SEMAPHORE_H

#include <semaphore>

class Semaphore{
	public:
		Semaphore() : sema(0){}
		// caution: this is not a real move constructor, this is just to work around std::counting_semaphore not being movable
		Semaphore(Semaphore &&to_move) : sema(0){}
		void acquire();
		void release();
	private:
		std::counting_semaphore<65535> sema;
};

#endif
