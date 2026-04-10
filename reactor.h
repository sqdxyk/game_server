#ifndef REACTOR_H
#define REACTOR_H

#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <deque>
#include <cstring>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <mysql/mysql.h>
#include "Locker.h"

const int buffer_len = 2048;

#define HEAD 1
#define BODY 2
#define HIT_BODY 3
#define HIT_HEAD 4
#define HIT_EMPTY 5

//MYSQL* connect_mysql();

class reactor {
public:
	reactor(int port, int worker_count = 0);

	~reactor();

	void run();

private:
	struct connection_t {
		int fd;
		int owner_worker;

		std::vector<char> rbuffer;
		int rlen;
		std::vector<char> wbuffer;
		int wlen;
		std::string in_buffer;
		std::deque<std::string> send_queue;
		size_t send_offset;
		std::mutex send_mutex;

		std::string ip;
		int client_port;
		std::string username;
		std::string pwd;

		bool is_start_game;
		bool isplaying;

		std::vector<std::vector<int>> grids;

		std::function<int(int)> recv_callback;
		std::function<int(int)> send_callback;

		enum State { RECV, SEND } state;
		connection_t() : fd(-1), owner_worker(-1), rlen(0), wlen(0), send_offset(0), client_port(0), is_start_game(false), isplaying(false), state(RECV) {}
	};

	struct shared_state_t {
		std::map<int, connection_t> conn_list;
		std::unordered_set<std::string> logined_username;
		std::unordered_set<int> readyfd_users;
		std::unordered_map<int, int> matched_users;
		std::unordered_map<int, int> turn_owner;
		std::unordered_map<int, time_t> game_turn_start;
	};

	class sub_reactor {
	public:
		sub_reactor(int id, std::shared_ptr<shared_state_t> state);
		~sub_reactor();

		void run();
		void add_client(int fd, const std::string& ip, int client_port);
		void enqueue_send(int fd, const std::string& line);
		void notify_write_interest(int fd);

	private:
		void set_event(int fd, int event, int flag);
		int recv_cb(int fd);
		int send_cb(int fd);

		int request(int fd);
		int response(int fd);

		int http_request(int fd);
		int http_response(int fd);

		void cleanup_connection(int fd);

		int worker_id;
		int epfd;
		std::shared_ptr<shared_state_t> shared_state;
	};

	void set_event(int fd, int event, int flag);
	int accept_cb(int fd);

	void match_players();
	void check_matched_players();

	void check_timeouts();
	void business_loop();
	void enqueue_send_to_fd(int fd, const std::string& line);

	std::shared_ptr<shared_state_t> shared_state;
	std::vector<std::unique_ptr<sub_reactor>> workers;
	std::vector<std::thread> worker_threads;
	std::thread business_thread;
	std::atomic<bool> running;
	int worker_count;
	int next_worker;
	int sockfd;
	int port;
	int epfd;
};



#endif
