#include <iostream>
#include <fstream>
#include <cstring>
#include <sstream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <vector>
#include <utility>
#include "reactor.h"
#include "Logger.h"
#include "Locker.h"
#include "c_time.h"
#include "c_thread_pool.h"
#include "c_mysql_pool.h"

using namespace std;

namespace {

bool all_heads_destroyed(const std::vector<std::vector<int>>& grids) {
    for (int i = 0; i < 15; ++i) {
        for (int j = 0; j < 15; ++j) {
            if (grids[i][j] == HEAD) {
                return false;
            }
        }
    }
    return true;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace

reactor::sub_reactor::sub_reactor(int id, std::shared_ptr<shared_state_t> state)
    : worker_id(id), epfd(epoll_create(1)), shared_state(std::move(state)) {}

reactor::sub_reactor::~sub_reactor() {
    if (epfd >= 0) {
        close(epfd);
    }
}

void reactor::sub_reactor::set_event(int fd, int event, int flag) {
    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    if (flag) {
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    } else {
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }
}

void reactor::sub_reactor::notify_write_interest(int fd) {
    set_event(fd, EPOLLIN | EPOLLOUT, 0);
}

void reactor::sub_reactor::enqueue_send(int fd, const std::string& line) {
    connection_t* conn = nullptr;
    int owner_worker = -1;
    {
        MutexGuard lock(connlist_mutex);
        auto it = shared_state->conn_list.find(fd);
        if (it == shared_state->conn_list.end() || it->second.fd == -1) {
            return;
        }
        conn = &it->second;
        owner_worker = it->second.owner_worker;
    }

    if (owner_worker != worker_id) {
        auto dispatcher = shared_state->dispatch_send;
        if (dispatcher) {
            dispatcher(fd, line);
        }
        return;
    }

    {
        MutexGuard qlock(conn->send_mutex);
        conn->send_queue.emplace_back(line + "\n");
    }
    notify_write_interest(fd);
}

void reactor::sub_reactor::add_client(int fd, const std::string& ip, int client_port) {
    if (set_nonblocking(fd) < 0) {
        perror("set_nonblocking");
    }

    {
        MutexGuard lock(connlist_mutex);
        auto [it, inserted] = shared_state->conn_list.try_emplace(fd);
        connection_t& conn = it->second;
        conn.fd = fd;
        conn.owner_worker = worker_id;
        conn.ip = ip;
        conn.client_port = client_port;
        conn.rbuffer.resize(buffer_len);
        conn.recv_callback = [this](int cfd) { return this->recv_cb(cfd); };
        conn.send_callback = [this](int cfd) { return this->send_cb(cfd); };
        conn.state = connection_t::RECV;
        (void)inserted;
    }

    set_event(fd, EPOLLIN, 1);
}

int reactor::sub_reactor::recv_cb(int fd) {
    return request(fd);
}

int reactor::sub_reactor::send_cb(int fd) {
    return response(fd);
}

void reactor::sub_reactor::cleanup_connection(int fd) {
    std::string username;
    int rival_fd = -1;

    {
        std::scoped_lock lock(connlist_mutex, loginusers_mutex, game_state_mutex);
        auto conn_it = shared_state->conn_list.find(fd);
        if (conn_it == shared_state->conn_list.end()) {
            return;
        }

        username = conn_it->second.username;
        if (!username.empty()) {
            shared_state->logined_username.erase(username);
        }

        shared_state->readyfd_users.erase(fd);

        auto mit = shared_state->matched_users.find(fd);
        if (mit != shared_state->matched_users.end()) {
            rival_fd = mit->second;
            auto rival_it = shared_state->conn_list.find(rival_fd);
            if (rival_it != shared_state->conn_list.end()) {
                rival_it->second.is_start_game = false;
                rival_it->second.isplaying = false;
                rival_it->second.grids.clear();
            }

            shared_state->matched_users.erase(fd);
            shared_state->matched_users.erase(rival_fd);
            shared_state->turn_owner.erase(fd);
            shared_state->turn_owner.erase(rival_fd);
            int min_fd = std::min(fd, rival_fd);
            shared_state->game_turn_start.erase(min_fd);
        }

        shared_state->conn_list.erase(conn_it);
    }
}

int reactor::sub_reactor::request(int fd) {
    connection_t* conn = nullptr;
    {
        MutexGuard lock(connlist_mutex);
        auto it = shared_state->conn_list.find(fd);
        if (it == shared_state->conn_list.end() || it->second.fd == -1) {
            return -1;
        }
        conn = &it->second;
    }

    char read_buf[buffer_len];
    while (true) {
        int count = recv(fd, read_buf, sizeof(read_buf), 0);
        if (count > 0) {
            conn->in_buffer.append(read_buf, count);
            continue;
        }
        if (count == 0) {
            //if (fd < 1000) {
            //    cout << "client " << fd << " disconnected" << endl;
            //}
            //else if (fd % 1000 == 0) {
            //    cout << "client " << fd << " disconnected" << endl;
            //}
            epoll_ctl(this->epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
            cleanup_connection(fd);
            return -1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

		//perror("recv error");
        //ThreadPool::instance().submit([=]() { log_error("recv error on fd %d, errno: %d", fd, errno); });
        epoll_ctl(this->epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        cleanup_connection(fd);
        return -1;
    }

    std::vector<std::string> messages;
    {
        size_t pos = 0;
        while (true) {
            size_t newline = conn->in_buffer.find('\n', pos);
            if (newline == std::string::npos) {
                conn->in_buffer.erase(0, pos);
                break;
            }
            messages.emplace_back(conn->in_buffer.substr(pos, newline - pos));
            pos = newline + 1;
        }
    }

    for (const std::string& message : messages) {
        size_t comma_pos = message.find(',');
        if (comma_pos == std::string::npos) {
			std::cerr << "Invalid message format: " << message << std::endl;
            continue;
        }

        std::string type = message.substr(0, comma_pos);
        std::string content = message.substr(comma_pos + 1);

        if (type == "chat") {
            std::string sender = "guest";
            std::vector<int> targets;
            {
                MutexGuard lock(connlist_mutex);
                auto it = shared_state->conn_list.find(fd);
                if (it != shared_state->conn_list.end() && !it->second.username.empty()) {
                    sender = it->second.username;
                }

                for (auto& kv : shared_state->conn_list) {
                    int to_fd = kv.first;
                    connection_t& client = kv.second;
                    if (to_fd <= 0 || to_fd == fd) continue;
                    if (client.fd != to_fd) continue;
                    if (client.username.empty()) continue;
                    targets.push_back(to_fd);
                }
            }
			//std::cout << sender << ": " << content << std::endl;
            std::cout<<"enter1"<<std::endl;
            std::cout << "chat route from fd=" << fd << " user=" << sender
                      << " targets=" << targets.size() << std::endl;
            for (int to_fd : targets) {
                int owner = -1;
                {
                    MutexGuard lock(connlist_mutex);
                    auto it = shared_state->conn_list.find(to_fd);
                    if (it != shared_state->conn_list.end()) {
                        owner = it->second.owner_worker;
                    }
                }
                std::cout << "  -> to fd=" << to_fd << " owner_worker=" << owner << std::endl;
            }

            std::string msg = "chat," + sender + ": " + content;
            for (int to_fd : targets) {
                enqueue_send(to_fd, msg);
            }
        } else if (type == "login") {
            std::string temp_username;
            std::string temp_pwd;      

            if (content.rfind("id:", 0) == 0) {
                size_t cpos = content.find(',', 3);
                if (cpos != std::string::npos && content.rfind("pwd:", cpos + 1) == cpos + 1) {
                    temp_username = content.substr(3, cpos - 3);
                    temp_pwd = content.substr(cpos + 5);
                }
            }

            if (temp_username.empty() || temp_pwd.empty()) {
				ThreadPool::instance().submit([=]() { log_warn("login failed for %s: invalid format or already logged in", temp_username.c_str()); });
                enqueue_send(fd, "login_fail");
                continue;
            }

            bool can_login = true;
            {
                std::scoped_lock lock(connlist_mutex, loginusers_mutex);
                auto conn_it = shared_state->conn_list.find(fd);
                if (conn_it == shared_state->conn_list.end()) {
                    continue;
                }
                if (!conn_it->second.username.empty() || shared_state->logined_username.count(temp_username)) {
                    can_login = false;
                }
            }

            if (!can_login) {
				ThreadPool::instance().submit([=]() { log_warn("login failed for %s: invalid format or already logged in", temp_username.c_str()); });
                enqueue_send(fd, "login_fail");
                continue;
            }

            MYSQL* mysql_conn = MySQLPool::instance().get();
            bool ok = false;

            if (mysql_conn) {
                char sql1[512];
                snprintf(sql1, sizeof(sql1), "SELECT password FROM t_user WHERE username='%s' LIMIT 1", temp_username.c_str());
                if (mysql_query(mysql_conn, sql1)) {
					ThreadPool::instance().submit([=]() { log_error("mysql query failed: %s", mysql_error(mysql_conn)); });
					std::cerr << "query failed: " << mysql_error(mysql_conn) << '\n';
					MySQLPool::instance().release(mysql_conn);
					enqueue_send(fd, "login_fail");
					continue;
				}

				{
                    MYSQL_RES* res = mysql_store_result(mysql_conn);
                    bool exists = (res && mysql_num_rows(res) > 0);
                    if (exists) {
                        MYSQL_ROW row = mysql_fetch_row(res);
                        std::string exist_pwd = row && row[0] ? row[0] : "";
                        ok = (exist_pwd == temp_pwd);
						if (!ok) {
							ThreadPool::instance().submit([=]() { log_warn("password mismatch for user %s", temp_username.c_str()); });
						}
                    } else {
                        char sql2[512];
                        snprintf(sql2, sizeof(sql2), "INSERT INTO t_user(username, password) VALUES('%s','%s')", temp_username.c_str(), temp_pwd.c_str());
						if (mysql_query(mysql_conn, sql2)) {
							ThreadPool::instance().submit([=]() { log_error("mysql insert failed: %s", mysql_error(mysql_conn)); });
							std::cerr << "insert failed: " << mysql_error(mysql_conn) << '\n';
							MySQLPool::instance().release(mysql_conn);
							enqueue_send(fd, "login_fail");
							continue;
						}
						ok = true;
                    }
                    if (res) {
                        mysql_free_result(res);
                    }
                }
                MySQLPool::instance().release(mysql_conn);
            }

            if (!ok) {
                enqueue_send(fd, "login_fail");
                continue;
            }

            bool login_ok = false;
            {
                std::scoped_lock lock(connlist_mutex, loginusers_mutex);
                auto conn_it = shared_state->conn_list.find(fd);
                if (conn_it != shared_state->conn_list.end() &&
                    conn_it->second.username.empty() &&
                    !shared_state->logined_username.count(temp_username)) {
                    conn_it->second.username = temp_username;
                    conn_it->second.pwd = temp_pwd;
                    shared_state->logined_username.insert(temp_username);
                    login_ok = true;
                }
            }

            enqueue_send(fd, login_ok ? "login_ok" : "login_fail");
        } else if (type == "ready") {
            std::scoped_lock lock(connlist_mutex, game_state_mutex);
            auto conn_it = shared_state->conn_list.find(fd);
            if (conn_it == shared_state->conn_list.end()) {
                continue;
            }

            if (content == "ok") {
				cout << fd << "match begin" << endl;
                conn_it->second.grids.assign(15, std::vector<int>(15, 0));
                shared_state->readyfd_users.insert(fd);
            } else {
				cout << fd << "match cancel" << endl;
                shared_state->readyfd_users.erase(fd);
            }
        } else if (type == "init") {
            MutexGuard lock(connlist_mutex);
            auto conn_it = shared_state->conn_list.find(fd);
            if (conn_it == shared_state->conn_list.end()) {
                continue;
            }

            if (!conn_it->second.is_start_game) {
                std::istringstream stream(content);
                std::string token;
                int first = 0;
                bool isY = false;
                int flag = 0;

                while (std::getline(stream, token, ',')) {
                    if (token.empty()) {
                        continue;
                    }
                    if (isY) {
                        int y = std::stoi(token);
                        conn_it->second.grids[first][y] = (flag % 10 == 0) ? HEAD : BODY;
                        ++flag;
                        isY = false;
                    } else {
                        first = std::stoi(token);
                        isY = true;
                    }
                }
                conn_it->second.is_start_game = true;
            }
        } else if (type == "attack") {
            std::vector<std::pair<int, std::string>> out_msgs;
            {
                std::scoped_lock lock(connlist_mutex, game_state_mutex);

                auto mit = shared_state->matched_users.find(fd);
                if (mit == shared_state->matched_users.end()) {
                    continue;
                }
                int rival_fd = mit->second;
                if (shared_state->turn_owner[fd] != fd) {
                    continue;
                }

                auto rival_it = shared_state->conn_list.find(rival_fd);
                auto self_it = shared_state->conn_list.find(fd);
                if (rival_it == shared_state->conn_list.end() || self_it == shared_state->conn_list.end()) {
                    continue;
                }

                auto& rival_grids = rival_it->second.grids;
                std::istringstream stream(content);
                std::string token;
                int first = 0;
                bool isY = false;

                while (std::getline(stream, token, ',')) {
                    if (token.empty()) {
                        continue;
                    }
                    if (isY) {
                        int y = std::stoi(token);
                        std::string rs;
                        if (rival_grids[first][y] == BODY) {
                            rival_grids[first][y] = HIT_BODY;
                            rs = "body";
                        } else if (rival_grids[first][y] == HEAD) {
                            rival_grids[first][y] = HIT_HEAD;
                            rs = "head";
                        } else {
                            rival_grids[first][y] = HIT_EMPTY;
                            rs = "empty";
                        }
                        out_msgs.emplace_back(fd, "hit," + rs + "," + std::to_string(first) + "," + std::to_string(y));
                        out_msgs.emplace_back(rival_fd, "behited," + rs + "," + std::to_string(first) + "," + std::to_string(y));
                        isY = false;
                    } else {
                        first = std::stoi(token);
                        isY = true;
                    }
                }

                if (all_heads_destroyed(rival_grids)) {
                    out_msgs.emplace_back(fd, "gameover,win,headshot");
                    out_msgs.emplace_back(rival_fd, "gameover,lose,headshot");

                    self_it->second.grids.clear();
                    rival_it->second.grids.clear();
                    self_it->second.is_start_game = false;
                    self_it->second.isplaying = false;
                    rival_it->second.is_start_game = false;
                    rival_it->second.isplaying = false;

                    shared_state->matched_users.erase(fd);
                    shared_state->matched_users.erase(rival_fd);
                    shared_state->turn_owner.erase(fd);
                    shared_state->turn_owner.erase(rival_fd);
                    int min_fd = std::min(fd, rival_fd);
                    shared_state->game_turn_start.erase(min_fd);
                } else {
                    shared_state->turn_owner[fd] = rival_fd;
                    shared_state->turn_owner[rival_fd] = rival_fd;

                    int min_fd = std::min(fd, rival_fd);
                    shared_state->game_turn_start[min_fd] = time(nullptr);

                    std::string turn_msg = "turn," + rival_it->second.username;
                    out_msgs.emplace_back(fd, turn_msg);
                    out_msgs.emplace_back(rival_fd, turn_msg);
                }
            }

            for (const auto& item : out_msgs) {
                enqueue_send(item.first, item.second);
            }
        } else if (type == "timeout") {
            std::vector<std::pair<int, std::string>> out_msgs;
            {
                std::scoped_lock lock(connlist_mutex, game_state_mutex);
                auto mit = shared_state->matched_users.find(fd);
                if (mit == shared_state->matched_users.end()) {
                    continue;
                }
                int rival_fd = mit->second;

                auto rival_it = shared_state->conn_list.find(rival_fd);
                if (rival_it == shared_state->conn_list.end()) {
                    continue;
                }

                shared_state->turn_owner[fd] = rival_fd;
                shared_state->turn_owner[rival_fd] = rival_fd;
                int min_fd = std::min(fd, rival_fd);
                shared_state->game_turn_start[min_fd] = time(nullptr);

                std::string turn_msg = "turn," + rival_it->second.username;
                out_msgs.emplace_back(fd, turn_msg);
                out_msgs.emplace_back(rival_fd, turn_msg);
                out_msgs.emplace_back(fd, "timeout");
                out_msgs.emplace_back(rival_fd, "timeout");
            }

            for (const auto& item : out_msgs) {
                enqueue_send(item.first, item.second);
            }
		} else {
			std::cout << "Unknown message type: " << type << std::endl;
        }
    }

    return 0;
}

int reactor::sub_reactor::response(int fd) {
    while (true) {
        connection_t* conn = nullptr;
        {
            MutexGuard lock(connlist_mutex);
            auto it = shared_state->conn_list.find(fd);
            if (it == shared_state->conn_list.end() || it->second.fd == -1) {
                return -1;
            }
            conn = &it->second;
        }

        MutexGuard qlock(conn->send_mutex);
        if (conn->send_queue.empty()) {
            set_event(fd, EPOLLIN, 0);
            return 0;
        }

        std::string& front = conn->send_queue.front();
        const char* p = front.data() + conn->send_offset;
        size_t left = front.size() - conn->send_offset;
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);

        if (n > 0) {
            conn->send_offset += static_cast<size_t>(n);
            if (conn->send_offset >= front.size()) {
                conn->send_queue.pop_front();
                conn->send_offset = 0;
            }
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            set_event(fd, EPOLLIN | EPOLLOUT, 0);
            return 0;
        }

        epoll_ctl(this->epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        cleanup_connection(fd);
        return -1;
    }
}

int reactor::sub_reactor::http_request(int fd) {
	(void)fd;
    return 0;
}

int reactor::sub_reactor::http_response(int fd) {
	(void)fd;
    return 0;
}

void reactor::sub_reactor::run() {
    struct epoll_event events[1024];
    while (1) {
        int nready = epoll_wait(epfd, events, 1024, 1000);
        for (int i = 0; i < nready; ++i) {
            int confd = events[i].data.fd;

            if (events[i].events & EPOLLIN) {
                recv_cb(confd);
            }
            if (events[i].events & EPOLLOUT) {
                send_cb(confd);
            }
        }
    }
}

reactor::reactor(int port, int worker_count)
    : running(true), worker_count(worker_count), next_worker(0), sockfd(-1), port(port), epfd(-1) {
    if (this->worker_count <= 0) {
        unsigned int hc = std::thread::hardware_concurrency();
        this->worker_count = (hc > 1 ? static_cast<int>(hc - 1) : 1);
    }
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(struct sockaddr_in));

    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons(port);

    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (-1 == bind(lfd, (struct sockaddr*)&serveraddr, sizeof(struct sockaddr))) {
        perror("bind");
    }
    listen(lfd, 65535);
    set_nonblocking(lfd);

    this->epfd = epoll_create(1);
    this->set_event(lfd, EPOLLIN, 1);
    this->sockfd = lfd;

    shared_state = std::make_shared<shared_state_t>();
    workers.reserve(this->worker_count);
    worker_threads.reserve(this->worker_count);
    for (int i = 0; i < this->worker_count; ++i) {
        workers.emplace_back(std::make_unique<sub_reactor>(i, shared_state));
    }
    shared_state->dispatch_send = [this](int fd, const std::string& line) {
        this->enqueue_send_to_fd(fd, line);
    };
}

reactor::~reactor() {
    running = false;

    if (business_thread.joinable()) {
        business_thread.detach();
    }
    for (auto& t : worker_threads) {
        if (t.joinable()) {
            t.detach();
        }
    }
    if (epfd >= 0) {
        close(epfd);
    }
    if (this->sockfd >= 0) {
        close(this->sockfd);
    }
}

void reactor::set_event(int fd, int event, int flag) {
    struct epoll_event ev;
    ev.events = event;
    ev.data.fd = fd;
    if (flag) {
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    } else {
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }
}

int reactor::accept_cb(int fd) {
    while (true) {
        struct sockaddr_in clientaddr;
        socklen_t len = sizeof(clientaddr);
        int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
        if (clientfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            ThreadPool::instance().submit([=]() { log_error("accept error: errno:%d", errno); });
            return -1;
        }

        char bufff[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientaddr.sin_addr, bufff, sizeof(bufff));
        int client_port = ntohs(clientaddr.sin_port);

		//if (clientfd < 1000) {
		//	cout << "client" << clientfd << " has connected" << endl;
		//}
		//else if (clientfd % 1000 == 0) {
		//	cout << "client" << clientfd << " has connected" << endl;
		//}

        int idx = next_worker++ % worker_count;
        workers[idx]->add_client(clientfd, bufff, client_port);
    }
}

void reactor::enqueue_send_to_fd(int fd, const std::string& line) {
    int worker_id = -1;
    {
        MutexGuard lock(connlist_mutex);
        auto it = shared_state->conn_list.find(fd);
        if (it == shared_state->conn_list.end() || it->second.fd == -1) {
            return;
        }
        worker_id = it->second.owner_worker;
    }

    if (worker_id >= 0 && worker_id < static_cast<int>(workers.size())) {
        workers[worker_id]->enqueue_send(fd, line);
    }
}

void reactor::match_players() {
    int fd1 = -1;
    int fd2 = -1;
    int first_player = -1;
    std::string name1;
    std::string name2;

    {
        std::scoped_lock lock(connlist_mutex, game_state_mutex);
        if (shared_state->readyfd_users.size() < 2) {
            return;
        }

        auto it = shared_state->readyfd_users.begin();
        fd1 = *it++;
        fd2 = *it;

        bool first_is_fd1 = (rand() % 2 == 0);
        first_player = first_is_fd1 ? fd1 : fd2;

        shared_state->matched_users[fd1] = fd2;
        shared_state->matched_users[fd2] = fd1;
        shared_state->turn_owner[fd1] = first_player;
        shared_state->turn_owner[fd2] = first_player;

        auto c1 = shared_state->conn_list.find(fd1);
        auto c2 = shared_state->conn_list.find(fd2);
        name1 = (c1 == shared_state->conn_list.end()) ? "" : c1->second.username;
        name2 = (c2 == shared_state->conn_list.end()) ? "" : c2->second.username;

        shared_state->readyfd_users.erase(fd1);
        shared_state->readyfd_users.erase(fd2);
    }

    enqueue_send_to_fd(fd1, "game_start," + name2 + "," + (first_player == fd1 ? "1" : "0"));
    enqueue_send_to_fd(fd2, "game_start," + name1 + "," + (first_player == fd2 ? "1" : "0"));
	ThreadPool::instance().submit([=]() {log_info("match success: %d (%s) vs %d (%s), first player %d",
		fd1, name1.c_str(), fd2, name2.c_str(), first_player); });
}

void reactor::check_matched_players() {
    std::vector<std::pair<int, int>> battles;

    {
        std::scoped_lock lock(connlist_mutex, game_state_mutex);
        for (const auto& pair : shared_state->matched_users) {
            int a = pair.first;
            int b = pair.second;
            if (a > b) {
                continue;
            }

            auto ia = shared_state->conn_list.find(a);
            auto ib = shared_state->conn_list.find(b);
            if (ia == shared_state->conn_list.end() || ib == shared_state->conn_list.end()) {
                continue;
            }

            if (ia->second.is_start_game && ib->second.is_start_game) {
                if (!ia->second.isplaying || !ib->second.isplaying) {
                    ia->second.isplaying = true;
                    ib->second.isplaying = true;
                    int min_fd = std::min(a, b);
                    shared_state->game_turn_start[min_fd] = time(nullptr);
                    battles.emplace_back(a, b);
                }
            }
        }
    }

    for (const auto& battle : battles) {
		ThreadPool::instance().submit([=]() {log_info("battle start: %d vs %d", battle.first, battle.second); });
        enqueue_send_to_fd(battle.first, "battle_start");
        enqueue_send_to_fd(battle.second, "battle_start");
    }
}

void reactor::check_timeouts() {
    time_t now = time(nullptr);
    std::vector<std::pair<int, std::string>> outs;

    {
        std::scoped_lock lock(connlist_mutex, game_state_mutex);
        for (auto it = shared_state->game_turn_start.begin(); it != shared_state->game_turn_start.end();) {
            int min_fd = it->first;
            time_t start = it->second;
            if (now - start <= 20) {
                ++it;
                continue;
            }

            auto mit = shared_state->matched_users.find(min_fd);
            if (mit == shared_state->matched_users.end()) {
                it = shared_state->game_turn_start.erase(it);
                continue;
            }

            int fd1 = min_fd;
            int fd2 = mit->second;
            int current = shared_state->turn_owner[fd1];

            if (current == fd1) {
                outs.emplace_back(fd1, "gameover,lose,timeout");
                outs.emplace_back(fd2, "gameover,win,timeout");
            } else {
                outs.emplace_back(fd1, "gameover,win,timeout");
                outs.emplace_back(fd2, "gameover,lose,timeout");
            }
			ThreadPool::instance().submit([=]() {log_info("timeout: player %d timed out, winner %d", current, (current == fd1 ? fd2 : fd1)); });

            auto c1 = shared_state->conn_list.find(fd1);
            auto c2 = shared_state->conn_list.find(fd2);
            if (c1 != shared_state->conn_list.end()) {
                c1->second.grids.clear();
                c1->second.is_start_game = false;
                c1->second.isplaying = false;
            }
            if (c2 != shared_state->conn_list.end()) {
                c2->second.grids.clear();
                c2->second.is_start_game = false;
                c2->second.isplaying = false;
            }

            shared_state->matched_users.erase(fd1);
            shared_state->matched_users.erase(fd2);
            shared_state->turn_owner.erase(fd1);
            shared_state->turn_owner.erase(fd2);
            it = shared_state->game_turn_start.erase(it);
        }
    }

    for (const auto& item : outs) {
        enqueue_send_to_fd(item.first, item.second);
    }
}

void reactor::business_loop() {
    while (running) {
        match_players();
        check_matched_players();
        check_timeouts();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void reactor::run() {
    srand(static_cast<unsigned int>(time(nullptr)));
    for (int i = 0; i < worker_count; ++i) {
        worker_threads.emplace_back([this, i]() {
            workers[i]->run();
        });
    }
    business_thread = std::thread([this]() {
        business_loop();
    });

    // Main reactor only accepts connections and dispatches them to sub reactors.
    struct epoll_event events[1024];
    while (1) {
        int nready = epoll_wait(epfd, events, 1024, 1000);
        for (int i = 0; i < nready; ++i) {
            if (events[i].events & EPOLLIN) {
                accept_cb(events[i].data.fd);
            }
        }
    }
}
