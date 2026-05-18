#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <arpa/inet.h>

// Encode payload into length-prefixed frame: [4-byte big-endian len][payload]
inline std::string encode_frame(const std::string& payload) {
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    std::string frame;
    frame.reserve(4 + payload.size());
    frame.append(reinterpret_cast<const char*>(&len), 4);
    frame.append(payload);
    return frame;
}

// Decode complete frames from buffer. Incomplete frame data stays in buffer.
inline std::vector<std::string> decode_frames(std::string& buffer) {
    std::vector<std::string> frames;
    while (buffer.size() >= 4) {
        uint32_t len;
        std::memcpy(&len, buffer.data(), 4);
        len = ntohl(len);
        if (buffer.size() < 4 + len) break;
        frames.emplace_back(buffer.substr(4, len));
        buffer.erase(0, 4 + len);
    }
    return frames;
}
