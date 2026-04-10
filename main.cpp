#include "reactor.h"
#include "Logger.h"
#include "c_thread_pool.h"
#include "c_mysql_pool.h"
#include <iostream>
using namespace std;

int main(int argc, char** argvs) {
	Logger::instance().open("../test.log");

	ThreadPool::instance().set_maxthreads(4);
	// 初始化 MySQL 连接池，20个连接
	MySQLPool::instance().init(20, "localhost", "csc", "csc1472583690", "user_center");
	reactor server(2048);
	
	server.run();
	
	return 0;
}
