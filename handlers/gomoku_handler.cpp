#include "gomoku_handler.h"
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

bool check_five_in_row(const std::vector<std::vector<int>>& board, int x, int y, int color) {
    const int dx[] = {1, 0, 1, 1};
    const int dy[] = {0, 1, 1, -1};
    for (int d = 0; d < 4; ++d) {
        int count = 1;
        for (int i = 1; i < 5; ++i) {
            int nx = x + dx[d] * i, ny = y + dy[d] * i;
            if (nx >= 0 && nx < 15 && ny >= 0 && ny < 15 && board[nx][ny] == color) ++count;
            else break;
        }
        for (int i = 1; i < 5; ++i) {
            int nx = x - dx[d] * i, ny = y - dy[d] * i;
            if (nx >= 0 && nx < 15 && ny >= 0 && ny < 15 && board[nx][ny] == color) ++count;
            else break;
        }
        if (count >= 5) return true;
    }
    return false;
}

void cleanup_gomoku_match(const std::shared_ptr<shared_state_t>& state, int fd1, int fd2) {
    state->matched_users.erase(fd1);
    state->matched_users.erase(fd2);
    state->turn_owner.erase(fd1);
    state->turn_owner.erase(fd2);
    state->match_game_type.erase(fd1);
    state->match_game_type.erase(fd2);
    state->gomoku_stone_color.erase(fd1);
    state->gomoku_stone_color.erase(fd2);
    int min_fd = std::min(fd1, fd2);
    state->gomoku_boards.erase(min_fd);
    state->game_turn_start.erase(min_fd);
}

} // namespace

static void handle_gomoku(const HandlerContext& ctx, const std::string& content) {
    // Parse x,y
    std::istringstream stream(content);
    std::string token;
    int x = -1, y = -1;
    if (std::getline(stream, token, ',')) x = std::stoi(token);
    if (std::getline(stream, token, ',')) y = std::stoi(token);
    if (x < 0 || x >= 15 || y < 0 || y >= 15) return;

    std::vector<std::pair<int, std::string>> out_msgs;
    {
        std::scoped_lock lock(connlist_mutex, game_state_mutex);

        auto mit = ctx.state->matched_users.find(ctx.fd);
        if (mit == ctx.state->matched_users.end()) return;
        int rival_fd = mit->second;

        // Check it's this player's turn
        if (ctx.state->turn_owner[ctx.fd] != ctx.fd) return;

        int min_fd = std::min(ctx.fd, rival_fd);
        auto& board = ctx.state->gomoku_boards[min_fd];
        if (board.empty()) {
            board.assign(15, std::vector<int>(15, GOMOKU_EMPTY));
        }

        // Check position is empty
        if (board[x][y] != GOMOKU_EMPTY) return;

        // Determine color from existing stone count: even=BLACK, odd=WHITE
        int stone_count = 0;
        for (int i = 0; i < 15; ++i)
            for (int j = 0; j < 15; ++j)
                if (board[i][j] != GOMOKU_EMPTY) ++stone_count;
        int color = (stone_count % 2 == 0) ? GOMOKU_BLACK : GOMOKU_WHITE;
        board[x][y] = color;

        std::string color_str = (color == GOMOKU_BLACK) ? "black" : "white";
        out_msgs.emplace_back(ctx.fd, "gomoku_hit," + std::to_string(x) + "," + std::to_string(y) + "," + color_str);
        out_msgs.emplace_back(rival_fd, "gomoku_hit," + std::to_string(x) + "," + std::to_string(y) + "," + color_str);

        if (check_five_in_row(board, x, y, color)) {
            out_msgs.emplace_back(ctx.fd, "gameover,win,five_in_row");
            out_msgs.emplace_back(rival_fd, "gameover,lose,five_in_row");

            auto self_it = ctx.state->conn_list.find(ctx.fd);
            auto rival_it = ctx.state->conn_list.find(rival_fd);
            if (self_it != ctx.state->conn_list.end()) {
                self_it->second.is_start_game = false;
                self_it->second.isplaying = false;
            }
            if (rival_it != ctx.state->conn_list.end()) {
                rival_it->second.is_start_game = false;
                rival_it->second.isplaying = false;
            }
            cleanup_gomoku_match(ctx.state, ctx.fd, rival_fd);
        } else {
            ctx.state->turn_owner[ctx.fd] = rival_fd;
            ctx.state->turn_owner[rival_fd] = rival_fd;
            ctx.state->game_turn_start[min_fd] = time(nullptr);

            auto rival_it = ctx.state->conn_list.find(rival_fd);
            if (rival_it != ctx.state->conn_list.end()) {
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

// Handle gomoku move in splitbrain mode
static void handle_gomoku_splitbrain(const HandlerContext& ctx, const std::string& content) {
    std::istringstream stream(content);
    std::string token;
    int x = -1, y = -1;
    if (std::getline(stream, token, ',')) x = std::stoi(token);
    if (std::getline(stream, token, ',')) y = std::stoi(token);
    if (x < 0 || x >= 15 || y < 0 || y >= 15) return;

    std::vector<std::pair<int, std::string>> out_msgs;
    {
        std::scoped_lock lock(connlist_mutex, game_state_mutex);

        auto mit = ctx.state->matched_users.find(ctx.fd);
        if (mit == ctx.state->matched_users.end()) return;
        int rival_fd = mit->second;

        auto gt_it = ctx.state->match_game_type.find(ctx.fd);
        if (gt_it == ctx.state->match_game_type.end() || gt_it->second != "splitbrain") return;
        if (ctx.state->turn_owner[ctx.fd] != ctx.fd) return;

        int min_fd = std::min(ctx.fd, rival_fd);
        auto& board = ctx.state->gomoku_boards[min_fd];
        if (board.empty()) {
            board.assign(15, std::vector<int>(15, GOMOKU_EMPTY));
        }

        if (board[x][y] != GOMOKU_EMPTY) return;

        // Determine color from existing stone count: even=BLACK, odd=WHITE
        int stone_count = 0;
        for (int i = 0; i < 15; ++i)
            for (int j = 0; j < 15; ++j)
                if (board[i][j] != GOMOKU_EMPTY) ++stone_count;
        int color = (stone_count % 2 == 0) ? GOMOKU_BLACK : GOMOKU_WHITE;
        board[x][y] = color;

        std::string color_str = (color == GOMOKU_BLACK) ? "black" : "white";
        out_msgs.emplace_back(ctx.fd, "gomoku_hit," + std::to_string(x) + "," + std::to_string(y) + "," + color_str);
        out_msgs.emplace_back(rival_fd, "gomoku_hit," + std::to_string(x) + "," + std::to_string(y) + "," + color_str);

        // Mark gomoku done for this turn
        ctx.state->splitbrain_gomoku_done[ctx.fd] = true;

        // Check gomoku win
        if (check_five_in_row(board, x, y, color)) {
            out_msgs.emplace_back(ctx.fd, "gameover,win,five_in_row");
            out_msgs.emplace_back(rival_fd, "gameover,lose,five_in_row");

            auto self_it = ctx.state->conn_list.find(ctx.fd);
            auto rival_it = ctx.state->conn_list.find(rival_fd);
            if (self_it != ctx.state->conn_list.end()) {
                self_it->second.is_start_game = false;
                self_it->second.isplaying = false;
                self_it->second.grids.clear();
            }
            if (rival_it != ctx.state->conn_list.end()) {
                rival_it->second.is_start_game = false;
                rival_it->second.isplaying = false;
                rival_it->second.grids.clear();
            }
            cleanup_gomoku_match(ctx.state, ctx.fd, rival_fd);
            ctx.state->splitbrain_bomb_done.erase(ctx.fd);
            ctx.state->splitbrain_gomoku_done.erase(ctx.fd);
            ctx.state->splitbrain_bomb_done.erase(rival_fd);
            ctx.state->splitbrain_gomoku_done.erase(rival_fd);
        }
        // If both actions done, switch turn
        else if (ctx.state->splitbrain_bomb_done[ctx.fd] && ctx.state->splitbrain_gomoku_done[ctx.fd]) {
            ctx.state->turn_owner[ctx.fd] = rival_fd;
            ctx.state->turn_owner[rival_fd] = rival_fd;
            ctx.state->game_turn_start[min_fd] = time(nullptr);

            // Clear action tracking for both
            ctx.state->splitbrain_bomb_done[ctx.fd] = false;
            ctx.state->splitbrain_gomoku_done[ctx.fd] = false;

            auto rival_it = ctx.state->conn_list.find(rival_fd);
            if (rival_it != ctx.state->conn_list.end()) {
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

void register_gomoku_handlers(MessageDispatcher& dispatcher) {
    dispatcher.register_handler("gomoku", handle_gomoku);
    dispatcher.register_handler("gomoku_splitbrain", handle_gomoku_splitbrain);
}
