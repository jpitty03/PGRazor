#pragma once
#include <chrono>
#include <thread>
#include <functional>
#define CLOCK_BOOTTIME 7
inline uint64_t gettid(){return std::hash<std::thread::id>{}(std::this_thread::get_id());}
inline int clock_gettime(int,timespec*t){auto n=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();t->tv_sec=n/1000000000;t->tv_nsec=n%1000000000;return 0;}
