#ifndef LOCKER_H
#define LOCKER_H

#include <mutex>

inline std::mutex localtime_mutex;
inline std::recursive_mutex logger_mutex;
inline std::mutex threadpool_mutex;
inline std::mutex mysqlpool_mutex;

// Reactor shared-state locks.
inline std::mutex connlist_mutex;
inline std::mutex loginusers_mutex;
inline std::mutex game_state_mutex;

// Legacy aliases used by existing code paths.
inline std::mutex readyusers_mutex;
inline std::mutex matchedusers_mutex;

using MutexGuard = std::lock_guard<std::mutex>;
using UniqueLock = std::unique_lock<std::mutex>;


#endif