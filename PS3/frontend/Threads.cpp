// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#include <atomic>
#include <mutex>
#include <vector>
#include <pthread.h>
#include <sys/thread.h>
#include <lv2/thread.h>
#include "PS3/frontend/Frontend.h"

// Every PPU thread the core creates (and the main thread), so the frontend can push them all to
// the lowest priority while the XMB is open: at a few FPS the emulator keeps both PPU hardware
// threads busy, and the XMB's own loader threads never got to run (endless spinner).
namespace {
struct ThreadEntry {
	sys_ppu_thread_t id;
	s32 savedPriority;
};
std::mutex g_threadsLock;
std::vector<ThreadEntry> g_threads;
bool g_background = false;

void RegisterCurrentThread() {
	sys_ppu_thread_t id;
	if (sysThreadGetId(&id) != 0) return;
	std::lock_guard<std::mutex> lock(g_threadsLock);
	for (auto &e : g_threads)
		if (e.id == id) return;
	ThreadEntry e{id, 0};
	if (g_background) {
		sysThreadGetPriority(id, &e.savedPriority);
		sysThreadSetPriority(id, 3071);
	}
	g_threads.push_back(e);
}

struct Trampoline {
	void *(*entry)(void *);
	void *argument;
};

void *ThreadTrampoline(void *raw) {
	Trampoline t = *(Trampoline *)raw;
	delete (Trampoline *)raw;
	RegisterCurrentThread();
	return t.entry(t.argument);
}
}  // namespace

void PS3Frontend::RegisterMainThread() {
	RegisterCurrentThread();
}

void PS3Frontend::SetEmulatorThreadsBackground(bool background) {
	std::lock_guard<std::mutex> lock(g_threadsLock);
	if (background == g_background) return;
	g_background = background;
	int changed = 0;
	for (auto &e : g_threads) {
		if (background) {
			if (sysThreadGetPriority(e.id, &e.savedPriority) != 0) continue;
			if (sysThreadSetPriority(e.id, 3071) == 0) changed++;
		} else if (e.savedPriority > 0) {
			if (sysThreadSetPriority(e.id, e.savedPriority) == 0) changed++;
		}
	}
	PS3Frontend::Trace("Emulator threads %s: %d of %u changed", background ? "set to lowest priority" : "restored", changed, (unsigned)g_threads.size());
}

// Startup tracing writes and fsyncs to several log files per call, which is fine while the core
// boots but ruinous once a game runs: PPSSPP creates condition variables at runtime (parallel
// loops, memory copies, waitables), and every one of them went through this trace. Frontend 04
// left it on for the whole session. Tracing now stops when the emulation loop starts.
static std::atomic<bool> diagnostics{false};
// Core-created threads with default attributes always get this stack (Frontend 03 fix), whether
// or not tracing is on, so threads started later in a session keep the same headroom.
static constexpr size_t coreThreadStack = 1024 * 1024;
static std::atomic<bool> largeStacks{false};

void PS3Frontend::EnableThreadDiagnostics() {
	largeStacks.store(true, std::memory_order_relaxed);
	diagnostics.store(true, std::memory_order_relaxed);
}

void PS3Frontend::DisableThreadDiagnostics() {
	diagnostics.store(false, std::memory_order_relaxed);
}

extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern "C" int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);

extern "C" int __wrap_pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes) {
	if (!diagnostics.load(std::memory_order_relaxed))
		return __real_pthread_cond_init(condition, attributes);
	PS3Frontend::Trace("pthread_cond_init entered");
	int result = __real_pthread_cond_init(condition, attributes);
	PS3Frontend::Trace("pthread_cond_init returned: %d", result);
	return result;
}

extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
	void *(*entry)(void *), void *argument) {
	const bool trace = diagnostics.load(std::memory_order_relaxed);
	Trampoline *trampoline = new Trampoline{entry, argument};
	entry = ThreadTrampoline;
	argument = trampoline;
	if (!largeStacks.load(std::memory_order_relaxed) || attributes != nullptr) {
		if (!trace)
			return __real_pthread_create(thread, attributes, entry, argument);
		PS3Frontend::Trace("pthread_create entered (explicit attributes)");
		int result = __real_pthread_create(thread, attributes, entry, argument);
		PS3Frontend::Trace("pthread_create returned: %d", result);
		return result;
	}

	if (trace) PS3Frontend::Trace("pthread_create entered");
	pthread_attr_t owned{};
	int result = pthread_attr_init(&owned);
	if (result != 0) {
		PS3Frontend::Trace("pthread_attr_init failed: %d", result);
		return result;
	}
	size_t previousSize = 0;
	pthread_attr_getstacksize(&owned, &previousSize);
	result = pthread_attr_setstacksize(&owned, coreThreadStack);
	if (result != 0) {
		PS3Frontend::Trace("pthread_attr_setstacksize failed: %d", result);
		pthread_attr_destroy(&owned);
		return result;
	}
	if (trace) PS3Frontend::Trace("Core thread stack: SDK request=%lu, using=%lu", (unsigned long)previousSize, (unsigned long)coreThreadStack);
	result = __real_pthread_create(thread, &owned, entry, argument);
	// Out of memory for a 1 MB stack (Silent Hill Origins leaves no free piece that big): try a
	// smaller one rather than failing - a failed std::thread closes the emulator.
	for (size_t smaller = 256 * 1024; result != 0 && smaller >= 64 * 1024 && smaller < coreThreadStack; smaller /= 2) {
		if (pthread_attr_setstacksize(&owned, smaller) != 0)
			break;
		const int retry = __real_pthread_create(thread, &owned, entry, argument);
		PS3Frontend::Trace("pthread_create returned %d with a 1 MB stack; with %lu KB: %d", result, (unsigned long)(smaller >> 10), retry);
		result = retry;
	}
	if (trace || result != 0) PS3Frontend::Trace("pthread_create returned: %d", result);
	pthread_attr_destroy(&owned);
	return result;
}
