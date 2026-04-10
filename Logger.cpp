#include "Logger.h"
#include "c_time.h"
#include "Locker.h"

#include <cstring>
#include <iostream>
#include <cstdarg>

const char* Logger::c_levels[Logger::LEVEL_COUNT] = {
    "SUCCESS",
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR",
    "FATAL"
};

Logger::Logger() : c_level(ERROR), c_max(8192), c_len(0) {
    
}

Logger::~Logger() {
    close();
}

void Logger::set_record_level(level lev) {
    std::lock_guard<std::recursive_mutex> lock(logger_mutex);
    c_level = lev; 
}

void Logger::open(const std::string& filename) {
    std::lock_guard<std::recursive_mutex> lock(logger_mutex);
    c_filename = filename;
    c_fout.open(filename, std::ios::app);
    if (c_fout.fail()) {
        throw std::logic_error("open file failed : " + filename);
    }
    c_fout.seekp(0, std::ios::end);
    c_len = c_fout.tellp();
}

void Logger::close() {
    std::lock_guard<std::recursive_mutex> lock(logger_mutex);
    c_fout.close();
}

void Logger::change_page() {
    std::lock_guard<std::recursive_mutex> lock(logger_mutex);
    close();
    std::string filename = c_filename + gettime("%Y-%m-%d-%H-%M-%S");
    if (rename(c_filename.c_str(), filename.c_str()) != 0) {
        throw std::logic_error("rename log file failed : " + std::string(strerror(errno)));
    }
    open(c_filename);
}

void Logger::log(level level, const char* file, int line, const char* format, ...) {
    if (c_level > level) {
        return;
    }
    if (c_fout.fail()) {
        throw std::logic_error("open file failed : " + c_filename);
    }

    std::lock_guard<std::recursive_mutex> lock(logger_mutex);
    std::string timenow = gettime("%Y-%m-%d %H:%M:%S, %a");
    //std::cout << timenow << std::endl;
    //// 格式化日志内容
    va_list args;
    va_start(args, format);
    char message[256];
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // 构造完整的日志字符串
    std::string log_entry = "[" + timenow + "] [" + std::string(c_levels[level]) + "] [" + std::string(file) + ":" + std::to_string(line) + "] " + message + "\n";
    c_len += log_entry.length();
    // 输出到控制台
    // std::cout << log_entry;

    // 输出到日志文件
    if (c_fout.is_open()) {
        if (c_len > c_max) {
            change_page();
            c_len = log_entry.length();
        }
        c_fout << log_entry; // 写入日志文件
    }
    c_fout.flush();
}
