#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

struct shared_state_t;

struct HandlerContext {
    int fd;
    std::shared_ptr<shared_state_t> state;
    std::function<void(int, const std::string&)> send;
};

using MessageHandler = std::function<void(const HandlerContext& ctx, const std::string& content)>;

class MessageDispatcher {
public:
    void register_handler(const std::string& type, MessageHandler handler) {
        handlers_[type] = std::move(handler);
    }

    bool dispatch(const HandlerContext& ctx, const std::string& type, const std::string& content) {
        auto it = handlers_.find(type);
        if (it == handlers_.end()) return false;
        it->second(ctx, content);
        return true;
    }

private:
    std::unordered_map<std::string, MessageHandler> handlers_;
};
