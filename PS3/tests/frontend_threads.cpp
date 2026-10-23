// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
// Native PPU object / SDK pthread ABI with mocked calls; not PS3 execution.
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include "PS3/frontend/Frontend.h"

struct pthread_attr_t_ { size_t size; };
static pthread_attr_t_ storage;
static int initError, setError, createError, condError;
static int initCalls, destroyCalls, createCalls, condCalls, traceCalls;
static const pthread_attr_t *explicitAttributes;
static void *expectedArgument;
static void *(*expectedEntry)(void *);
static int checks;
static void Check(bool valid) { ++checks; if (!valid) std::abort(); }
void PS3Frontend::Trace(const char *, ...) { ++traceCalls; }
extern "C" int __wrap_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern "C" int __wrap_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
extern "C" int pthread_attr_init(pthread_attr_t *a) {
	++initCalls;
	if (initError) return initError;
	storage.size = 0; *a = &storage; return 0;
}
extern "C" int pthread_attr_destroy(pthread_attr_t *a) {
	Check(*a == &storage); ++destroyCalls; *a = nullptr; return 0;
}
extern "C" int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size) {
	*size = (*a)->size; return 0;
}
extern "C" int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) {
	if (setError) return setError;
	(*a)->size = size; return 0;
}
extern "C" int __real_pthread_create(pthread_t *t, const pthread_attr_t *a,
	void *(*entry)(void *), void *argument) {
	++createCalls;
	Check(entry == expectedEntry && argument == expectedArgument);
	if (explicitAttributes) Check(a == explicitAttributes && (*a)->size == 512 * 1024);
	else if (initCalls) Check(a && *a == &storage && (*a)->size == 1024 * 1024 && !destroyCalls);
	else Check(a == nullptr);
	t->p = argument; t->x = 123; return createError;
}
extern "C" int __real_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
	++condCalls; Check(c && !a); return condError;
}
static void Reset() {
	initError = setError = createError = condError = 0;
	initCalls = destroyCalls = createCalls = condCalls = traceCalls = 0;
	explicitAttributes = nullptr;
}
static void *Entry(void *argument) { return argument; }
int main() {
	int argument = 17; expectedArgument = &argument; expectedEntry = Entry;
	pthread_t thread{}; pthread_cond_t condition{};
	Reset(); createError = EAGAIN;
	Check(__wrap_pthread_create(&thread, nullptr, Entry, &argument) == EAGAIN);
	Check(createCalls == 1 && !initCalls && !destroyCalls && !traceCalls);
	PS3Frontend::EnableThreadDiagnostics();
	Reset(); initError = ENOMEM;
	Check(__wrap_pthread_create(&thread, nullptr, Entry, &argument) == ENOMEM);
	Check(initCalls == 1 && !createCalls && !destroyCalls);
	Reset(); setError = EINVAL;
	Check(__wrap_pthread_create(&thread, nullptr, Entry, &argument) == EINVAL);
	Check(initCalls == 1 && !createCalls && destroyCalls == 1);
	Reset();
	Check(__wrap_pthread_create(&thread, nullptr, Entry, &argument) == 0);
	Check(initCalls == 1 && createCalls == 1 && destroyCalls == 1);
	Check(thread.p == &argument && thread.x == 123);
	Reset(); createError = EAGAIN;
	Check(__wrap_pthread_create(&thread, nullptr, Entry, &argument) == EAGAIN);
	Check(createCalls == 1 && destroyCalls == 1);
	Reset(); pthread_attr_t_ external{512 * 1024}; pthread_attr_t externalHandle = &external;
	explicitAttributes = &externalHandle;
	Check(__wrap_pthread_create(&thread, &externalHandle, Entry, &argument) == 0);
	Check(!initCalls && !destroyCalls && external.size == 512 * 1024);
	Reset(); condError = EINVAL;
	Check(__wrap_pthread_cond_init(&condition, nullptr) == EINVAL);
	Check(condCalls == 1 && traceCalls == 2);
	// Frontend 05: after boot, tracing stops but Core threads keep the 1 MiB stack.
	PS3Frontend::DisableThreadDiagnostics();
	Reset();
	Check(__wrap_pthread_create(&thread, nullptr, Entry, &argument) == 0);
	Check(initCalls == 1 && createCalls == 1 && destroyCalls == 1 && !traceCalls);
	Reset();
	Check(__wrap_pthread_cond_init(&condition, nullptr) == 0);
	Check(condCalls == 1 && !traceCalls);
	Reset(); createError = EAGAIN;
	Check(__wrap_pthread_create(&thread, nullptr, Entry, &argument) == EAGAIN);
	Check(createCalls == 1 && destroyCalls == 1 && traceCalls == 1);  // Failures are still logged.
	std::printf("Native PPU thread wrapper: %d checks PASS under Linux PowerPC QEMU; mocked SDK calls, not hardware execution\n", checks);
	return 0;
}
