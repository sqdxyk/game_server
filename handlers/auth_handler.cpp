#include "auth_handler.h"
#include "../reactor.h"
#include "../Locker.h"
#include "../c_mysql_pool.h"
#include "../c_thread_pool.h"
#include "../Logger.h"
#include <cstdio>

static void handle_login(const HandlerContext& ctx, const std::string& content) {
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
        ThreadPool::instance().submit([=]() {
            log_warn("login failed for %s: invalid format", temp_username.c_str());
        });
        ctx.send(ctx.fd, "login_fail");
        return;
    }

    bool can_login = true;
    {
        std::scoped_lock lock(connlist_mutex, loginusers_mutex);
        auto conn_it = ctx.state->conn_list.find(ctx.fd);
        if (conn_it == ctx.state->conn_list.end()) return;
        if (!conn_it->second.username.empty() || ctx.state->logined_username.count(temp_username)) {
            can_login = false;
        }
    }

    if (!can_login) {
        ThreadPool::instance().submit([=]() {
            log_warn("login failed for %s: already logged in", temp_username.c_str());
        });
        ctx.send(ctx.fd, "login_fail");
        return;
    }

    MYSQL* mysql_conn = MySQLPool::instance().get();
    bool ok = false;

    if (mysql_conn) {
        char sql1[512];
        snprintf(sql1, sizeof(sql1),
                 "SELECT password FROM t_user WHERE username='%s' LIMIT 1",
                 temp_username.c_str());
        if (mysql_query(mysql_conn, sql1)) {
            ThreadPool::instance().submit([=]() {
                log_error("mysql query failed: %s", mysql_error(mysql_conn));
            });
            MySQLPool::instance().release(mysql_conn);
            ctx.send(ctx.fd, "login_fail");
            return;
        }

        MYSQL_RES* res = mysql_store_result(mysql_conn);
        bool exists = (res && mysql_num_rows(res) > 0);
        if (exists) {
            MYSQL_ROW row = mysql_fetch_row(res);
            std::string exist_pwd = row && row[0] ? row[0] : "";
            ok = (exist_pwd == temp_pwd);
            if (!ok) {
                ThreadPool::instance().submit([=]() {
                    log_warn("password mismatch for user %s", temp_username.c_str());
                });
            }
        } else {
            char sql2[512];
            snprintf(sql2, sizeof(sql2),
                     "INSERT INTO t_user(username, password) VALUES('%s','%s')",
                     temp_username.c_str(), temp_pwd.c_str());
            if (mysql_query(mysql_conn, sql2)) {
                ThreadPool::instance().submit([=]() {
                    log_error("mysql insert failed: %s", mysql_error(mysql_conn));
                });
                MySQLPool::instance().release(mysql_conn);
                ctx.send(ctx.fd, "login_fail");
                return;
            }
            ok = true;
        }
        if (res) mysql_free_result(res);
        MySQLPool::instance().release(mysql_conn);
    }

    if (!ok) {
        ctx.send(ctx.fd, "login_fail");
        return;
    }

    bool login_ok = false;
    {
        std::scoped_lock lock(connlist_mutex, loginusers_mutex);
        auto conn_it = ctx.state->conn_list.find(ctx.fd);
        if (conn_it != ctx.state->conn_list.end() &&
            conn_it->second.username.empty() &&
            !ctx.state->logined_username.count(temp_username)) {
            conn_it->second.username = temp_username;
            conn_it->second.pwd = temp_pwd;
            ctx.state->logined_username.insert(temp_username);
            login_ok = true;
        }
    }

    ctx.send(ctx.fd, login_ok ? "login_ok" : "login_fail");
}

void register_auth_handlers(MessageDispatcher& dispatcher) {
    dispatcher.register_handler("login", handle_login);
}
