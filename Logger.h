#ifndef LOGGER_H
#define LOGGER_H

#include <cstring>
#include <fstream>

#define log_success(format, ...) Logger::instance().log(Logger::SUCCESS,__FILE__,__LINE__,format, ##__VA_ARGS__)
#define log_debug(format, ...) Logger::instance().log(Logger::DEBUG,__FILE__,__LINE__,format, ##__VA_ARGS__)
#define log_info(format, ...) Logger::instance().log(Logger::INFO,__FILE__,__LINE__,format, ##__VA_ARGS__)
#define log_warn(format, ...) Logger::instance().log(Logger::WARN,__FILE__,__LINE__,format, ##__VA_ARGS__)
#define log_error(format, ...) Logger::instance().log(Logger::ERROR,__FILE__,__LINE__,format, ##__VA_ARGS__)
#define log_fatal(format, ...) Logger::instance().log(Logger::FATAL,__FILE__,__LINE__,format, ##__VA_ARGS__)

class Logger {
public:
	enum level {
		SUCCESS = 0,
		DEBUG,
		INFO,
		WARN,
		ERROR,
		FATAL,
		LEVEL_COUNT
	};
	static Logger& instance() {
		static Logger single;
		return single;
	}

	void set_record_level(level lev);
	void open(const std::string& filename);
	void close();
	void log(level level, const char* file, int line, const char* format, ...);

private:
	Logger();
	~Logger();
	void change_page();
private:
	std::string c_filename;
	std::ofstream c_fout;
	level c_level;
	int c_max;
	int c_len;
	static const char* c_levels[LEVEL_COUNT];
};

#endif // !LOGGER_H
