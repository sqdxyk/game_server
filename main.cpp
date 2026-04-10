#include "reactor.h"
#include "Logger.h"
#include "c_thread_pool.h"
#include "c_mysql_pool.h"
#include <iostream>
using namespace std;

int main(int argc, char** argvs) {
	Logger::instance().open("../test.log");
	cout<<123<<endl;
	ThreadPool::instance().set_maxthreads(4);
	// ��ʼ�� MySQL ���ӳأ�20������
	MySQLPool::instance().init(20, "localhost", "csc", "csc1472583690", "user_center");
	reactor server(2048);
	
	server.run();
	
	return 0;
}
