#include "game_handler.h"
#include "../reactor.h"
#include "../Locker.h"
#include "../Logger.h"
#include "../c_thread_pool.h"
#include <sstream>
#include <iostream>
#include <vector>
#include <utility>
#include <ctime>

namespace {

bool all_heads_destroyed(const std::vector<std::vector<int>>& grids) {
    for (int i = 0; i < 15; ++i)
        for (int j = 0; j < 15; ++j)
            if (grids[i][j] == HEAD) return false;
    return true;
}

void cleanup_bomb_match(const std::shared_ptr<shared_state_t>& state, int fd1, int fd2) {
    state->matched_users.erase(fd1);
    state->matched_users.erase(fd2);
    state->turn_owner.erase(fd1);
    state->turn_owner.erase(fd2);
    state->match_game_type.erase(fd1);
    state->match_game_type.erase(fd2);
    int min_fd = std::min(fd1, fd2);
    state->game_turn_start.erase(min_fd);
}

void cleanup_splitbrain_match(const std::shared_ptr<shared_state_t>& state, int fd1, int fd2) {
    state->matched_users.erase(fd1);
    state->matched_users.erase(fd2);
    state->turn_owner.erase(fd1);
    state->turn_owner.erase(fd2);
    state->match_game_type.erase(fd1);
    state->match_game_type.erase(fd2);
    state->gomoku_stone_color.erase(fd1);
    state->gomoku_stone_color.erase(fd2);
    state->splitbrain_bomb_done.erase(fd1);
    state->splitbrain_gomoku_done.erase(fd1);
    state->splitbrain_bomb_done.erase(fd2);
    state->splitbrain_gomoku_done.erase(fd2);
    int min_fd = std::min(fd1, fd2);
    state->gomoku_boards.erase(min_fd);
    state->game_turn_start.erase(min_fd);
}

} // namespace

static void handle_ready(const HandlerContext& ctx, const std::string& content) {
    // content format: "bombing" / "gomoku" / "splitbrain" / "notok"
    std::scoped_lock lock(connlist_mutex, game_state_mutex);
    auto conn_it = ctx.state->conn_list.find(ctx.fd);
    if (conn_it == ctx.state->conn_list.end()) return;

    if (content == "notok") {
        std::cout << ctx.fd << " match cancel" << std::endl;
        ctx.state->readyfd_users.erase(ctx.fd);
        ctx.state->ready_game_types.erase(ctx.fd);
    } else if (content == "bombing" || content == "gomoku" || content == "splitbrain") {
        std::cout << ctx.fd << " match begin (" << content << ")" << std::endl;
        conn_it->second.grids.assign(15, std::vector<int>(15, 0));
        ctx.state->readyfd_users.insert(ctx.fd);
        ctx.state->ready_game_types[ctx.fd] = content;
    } else {
        ctx.state->readyfd_users.erase(ctx.fd);
        ctx.state->ready_game_types.erase(ctx.fd);
    }
}

static void handle_init(const HandlerContext& ctx, const std::string& content) {
    MutexGuard lock(connlist_mutex);
    auto conn_it = ctx.state->conn_list.find(ctx.fd);
    if (conn_it == ctx.state->conn_list.end()) return;

    if (!conn_it->second.is_start_game) {
        std::istringstream stream(content);
        std::string token;
        int first = 0;
        bool isY = false;
        int flag = 0;

        while (std::getline(stream, token, ',')) {
            if (token.empty()) continue;
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
}

static void handle_attack(const HandlerContext& ctx, const std::string& content) {
    std::vector<std::pair<int, std::string>> out_msgs;
    {
        std::scoped_lock lock(connlist_mutex, game_state_mutex);

        auto mit = ctx.state->matched_users.find(ctx.fd);
        if (mit == ctx.state->matched_users.end()) return;
        int rival_fd = mit->second;
        if (ctx.state->turn_owner[ctx.fd] != ctx.fd) return;

        auto rival_it = ctx.state->conn_list.find(rival_fd);
        auto self_it = ctx.state->conn_list.find(ctx.fd);
        if (rival_it == ctx.state->conn_list.end() || self_it == ctx.state->conn_list.end()) return;

        auto& rival_grids = rival_it->second.grids;
        std::istringstream stream(content);
        std::string token;
        int first = 0;
        bool isY = false;

        while (std::getline(stream, token, ',')) {
            if (token.empty()) continue;
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
                out_msgs.emplace_back(ctx.fd, "hit," + rs + "," + std::to_string(first) + "," + std::to_string(y));
                out_msgs.emplace_back(rival_fd, "behited," + rs + "," + std::to_string(first) + "," + std::to_string(y));
                isY = false;
            } else {
                first = std::stoi(token);
                isY = true;
            }
        }

        // Check if this is a splitbrain match
        auto gt_it = ctx.state->match_game_type.find(ctx.fd);
        bool is_splitbrain = (gt_it != ctx.state->match_game_type.end() && gt_it->second == "splitbrain");

        if (is_splitbrain) {
            // Mark bombing done for this turn
            ctx.state->splitbrain_bomb_done[ctx.fd] = true;

            // Check if bombing game is over (all heads destroyed)
            if (all_heads_destroyed(rival_grids)) {
                out_msgs.emplace_back(ctx.fd, "gameover,win,headshot");
                out_msgs.emplace_back(rival_fd, "gameover,lose,headshot");
                self_it->second.grids.clear();
                rival_it->second.grids.clear();
                self_it->second.is_start_game = false;
                self_it->second.isplaying = false;
                rival_it->second.is_start_game = false;
                rival_it->second.isplaying = false;
                cleanup_splitbrain_match(ctx.state, ctx.fd, rival_fd);
            }
            // If both actions done, switch turn
            else if (ctx.state->splitbrain_bomb_done[ctx.fd] && ctx.state->splitbrain_gomoku_done[ctx.fd]) {
                ctx.state->turn_owner[ctx.fd] = rival_fd;
                ctx.state->turn_owner[rival_fd] = rival_fd;
                int min_fd = std::min(ctx.fd, rival_fd);
                ctx.state->game_turn_start[min_fd] = time(nullptr);

                // Clear action tracking for next turn
                ctx.state->splitbrain_bomb_done[ctx.fd] = false;
                ctx.state->splitbrain_gomoku_done[ctx.fd] = false;

                std::string turn_msg = "turn," + rival_it->second.username;
                out_msgs.emplace_back(ctx.fd, turn_msg);
                out_msgs.emplace_back(rival_fd, turn_msg);
            }
        } else {
            // Normal bombing match
            if (all_heads_destroyed(rival_grids)) {
                out_msgs.emplace_back(ctx.fd, "gameover,win,headshot");
                out_msgs.emplace_back(rival_fd, "gameover,lose,headshot");

                self_it->second.grids.clear();
                rival_it->second.grids.clear();
                self_it->second.is_start_game = false;
                self_it->second.isplaying = false;
                rival_it->second.is_start_game = false;
                rival_it->second.isplaying = false;

                cleanup_bomb_match(ctx.state, ctx.fd, rival_fd);
            } else {
                ctx.state->turn_owner[ctx.fd] = rival_fd;
                ctx.state->turn_owner[rival_fd] = rival_fd;
                int min_fd = std::min(ctx.fd, rival_fd);
                ctx.state->game_turn_start[min_fd] = time(nullptr);
                std::string turn_msg = "turn," + rival_it->second.username;
                out_msgs.emplace_back(ctx.fd, turn_msg);
                out_msgs.emplace_back(rival_fd, turn_msg);
            }
        }
    }

    for (const auto& item : out_msgs) {
        ctx.send(item.first, item.second);
    }
}

static void handle_timeout(const HandlerContext& ctx, const std::string& /*content*/) {
    std::vector<std::pair<int, std::string>> out_msgs;
    {
        std::scoped_lock lock(connlist_mutex, game_state_mutex);
        auto mit = ctx.state->matched_users.find(ctx.fd);
        if (mit == ctx.state->matched_users.end()) return;
        int rival_fd = mit->second;

        auto rival_it = ctx.state->conn_list.find(rival_fd);
        auto self_it = ctx.state->conn_list.find(ctx.fd);
        if (rival_it == ctx.state->conn_list.end() || self_it == ctx.state->conn_list.end()) return;

        auto gt_it2 = ctx.state->match_game_type.find(ctx.fd);
        bool is_splitbrain = (gt_it2 != ctx.state->match_game_type.end() && gt_it2->second == "splitbrain");
        bool is_gomoku = (gt_it2 != ctx.state->match_game_type.end() && gt_it2->second == "gomoku");

        if (is_splitbrain) {
            // Timeout in splitbrain: loser loses everything
            out_msgs.emplace_back(ctx.fd, "gameover,lose,timeout");
            out_msgs.emplace_back(rival_fd, "gameover,win,timeout");

            self_it->second.grids.clear();
            rival_it->second.grids.clear();
            self_it->second.is_start_game = false;
            self_it->second.isplaying = false;
            rival_it->second.is_start_game = false;
            rival_it->second.isplaying = false;
            cleanup_splitbrain_match(ctx.state, ctx.fd, rival_fd);
        } else if (is_gomoku) {
            // Timeout in gomoku: loser loses
            out_msgs.emplace_back(ctx.fd, "gameover,lose,timeout");
            out_msgs.emplace_back(rival_fd, "gameover,win,timeout");

            self_it->second.is_start_game = false;
            self_it->second.isplaying = false;
            rival_it->second.is_start_game = false;
            rival_it->second.isplaying = false;

            ctx.state->matched_users.erase(ctx.fd);
            ctx.state->matched_users.erase(rival_fd);
            ctx.state->turn_owner.erase(ctx.fd);
            ctx.state->turn_owner.erase(rival_fd);
            ctx.state->match_game_type.erase(ctx.fd);
            ctx.state->match_game_type.erase(rival_fd);
            ctx.state->gomoku_stone_color.erase(ctx.fd);
            ctx.state->gomoku_stone_color.erase(rival_fd);
            int min_fd = std::min(ctx.fd, rival_fd);
            ctx.state->gomoku_boards.erase(min_fd);
            ctx.state->game_turn_start.erase(min_fd);
        } else {
            // Normal bombing timeout: switch turn
            ctx.state->turn_owner[ctx.fd] = rival_fd;
            ctx.state->turn_owner[rival_fd] = rival_fd;
            int min_fd = std::min(ctx.fd, rival_fd);
            ctx.state->game_turn_start[min_fd] = time(nullptr);

            std::string turn_msg = "turn," + rival_it->second.username;
            out_msgs.emplace_back(ctx.fd, turn_msg);
            out_msgs.emplace_back(rival_fd, turn_msg);
            out_msgs.emplace_back(ctx.fd, "timeout");
            out_msgs.emplace_back(rival_fd, "timeout");
        }
    }

    for (const auto& item : out_msgs) {
        ctx.send(item.first, item.second);
    }
}

void register_game_handlers(MessageDispatcher& dispatcher) {
    dispatcher.register_handler("ready",   handle_ready);
    dispatcher.register_handler("init",    handle_init);
    dispatcher.register_handler("attack",  handle_attack);
    dispatcher.register_handler("timeout", handle_timeout);
}
