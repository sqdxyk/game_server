#include "chat_handler.h"
#include "../reactor.h"
#include "../Locker.h"
#include <iostream>
#include <vector>

static void handle_chat(const HandlerContext& ctx, const std::string& content) {
    std::string sender = "guest";
    std::vector<int> targets;
    {
        MutexGuard lock(connlist_mutex);
        auto it = ctx.state->conn_list.find(ctx.fd);
        if (it != ctx.state->conn_list.end() && !it->second.username.empty()) {
            sender = it->second.username;
        }

        for (auto& kv : ctx.state->conn_list) {
            int to_fd = kv.first;
            if (to_fd <= 0 || to_fd == ctx.fd) continue;
            if (kv.second.username.empty()) continue;
            targets.push_back(to_fd);
        }
    }

    std::cout << sender << ": " << content << std::endl;
    std::string msg = "chat," + sender + ": " + content;
    for (int to_fd : targets) {
        ctx.send(to_fd, msg);
    }
}

void register_chat_handlers(MessageDispatcher& dispatcher) {
    dispatcher.register_handler("chat", handle_chat);
}
