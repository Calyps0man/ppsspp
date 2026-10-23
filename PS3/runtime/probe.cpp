// Copyright (c) 2012- PPSSPP Project.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License 2.0 for more details.
//
// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

constexpr std::string_view literal = "PS3 runtime";
static_assert(literal.size() == 11, "C++17 char_traits/string_view support");

int main() {
	std::mutex mutex;
	std::condition_variable condition;
	std::once_flag once;
	std::atomic<int> initialized{0};
	auto shared = std::make_shared<int>(1234);
	int count = 0;
	int completed = 0;
	std::thread workers[4];
	for (auto &worker : workers) {
		worker = std::thread([&] {
			std::call_once(once, [&] { ++initialized; });
			for (int i = 0; i < 80000; ++i) {
				auto copy = shared;
				std::lock_guard<std::mutex> lock(mutex);
				if (*copy == 1234) {
					++count;
				}
			}
			std::lock_guard<std::mutex> lock(mutex);
			++completed;
			condition.notify_one();
		});
	}
	std::unique_lock<std::mutex> lock(mutex);
	condition.wait(lock, [&] { return completed == 4; });
	lock.unlock();
	for (auto &worker : workers) {
		worker.join();
	}
	lock.lock();
	bool signaled = condition.wait_for(lock, std::chrono::milliseconds(10), [] { return false; });
	lock.unlock();
	bool passed = count == 320000 && initialized == 1 && shared.use_count() == 1
		&& !signaled && std::to_string(count) == "320000" && std::stoi("1234") == 1234;
	std::printf("Runtime contention/shared_ptr/once/CV/C++17: count=%d init=%d %s\n",
		count, (int)initialized, passed ? "PASS" : "FAIL");
	return passed ? 0 : 1;
}
