#ifndef C_TIME_H
#define C_TIME_H

#include <ctime>
#include <cstring>
#include <mutex>
#include "Locker.h"

inline std::string gettime(const char* format) {
	std::time_t now = std::time(nullptr);
	std::tm ltm;
	{
		MutexGuard lock(localtime_mutex);
#ifdef _MSC_VER
	// 使用 MSVC 的 localtime_s
		if (localtime_s(&ltm, &now) != 0) {
			throw std::runtime_error("localtime_s failed");
		}
#else
	// 使用 POSIX 的 localtime_r
		if (localtime_r(&now, &ltm) == nullptr) {
			throw std::runtime_error("localtime_r failed");
		}
#endif
	}
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), format, &ltm);
	return std::string(buffer);
}

#endif
