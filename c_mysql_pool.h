#ifndef C_MYSQL_POOL_H
#define C_MYSQL_POOL_H

#include <mysql/mysql.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <iostream>
#include "Locker.h"

class MySQLPool {
public:
    static MySQLPool& instance() {
        static MySQLPool pool;
        return pool;
    }

    // 初始化连接池，在程序启动时调用一次
    void init(int pool_size, const std::string& host, const std::string& user,
        const std::string& pwd, const std::string& db, unsigned int port = 0) {
        std::lock_guard<std::mutex> lock(pool_mutex);
        for (int i = 0; i < pool_size; ++i) {
            MYSQL* conn = mysql_init(nullptr);
            if (!conn) {
                std::cerr << "[MySQLPool] mysql_init failed for connection " << i << std::endl;
                continue;
            }
            if (!mysql_real_connect(conn, host.c_str(), user.c_str(),
                pwd.c_str(), db.c_str(), port, nullptr, 0)) {
                std::cerr << "[MySQLPool] connect error: " << mysql_error(conn) << std::endl;
                mysql_close(conn);
                continue;
            }
            conn_pool.push(conn);
        }
        if (conn_pool.empty()) {
            throw std::runtime_error("[MySQLPool] Failed to create any connection");
        }
        //std::cout << "[MySQLPool] Initialized with " << conn_pool.size() << " connections" << std::endl;
    }

    // 从池中获取一个连接（阻塞直到有可用连接）
    MYSQL* get() {
        std::unique_lock<std::mutex> lock(pool_mutex);
        while (conn_pool.empty()) {
            cv.wait(lock);
        }
        MYSQL* conn = conn_pool.front();
        conn_pool.pop();
        return conn;
    }

    // 归还连接
    void release(MYSQL* conn) {
        std::lock_guard<std::mutex> lock(pool_mutex);
        conn_pool.push(conn);
        cv.notify_one();
    }

    // 禁止拷贝
    MySQLPool(const MySQLPool&) = delete;
    MySQLPool& operator=(const MySQLPool&) = delete;

private:
    MySQLPool() = default;
    ~MySQLPool() {
        while (!conn_pool.empty()) {
            MYSQL* conn = conn_pool.front();
            conn_pool.pop();
            mysql_close(conn);
        }
    }

    std::queue<MYSQL*> conn_pool;
    std::mutex pool_mutex;
    std::condition_variable cv;
};

#endif // C_MYSQL_POOL_H