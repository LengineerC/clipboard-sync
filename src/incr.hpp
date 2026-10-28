#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace clipsync {
// An INCR handshake belongs to (requestor window, property), not to the
// clipboard as a whole. One slow reader must never block other readers.
class IncrTransfers {
   public:
    using Clock = std::chrono::steady_clock;
    using Key = std::pair<std::uint32_t, std::uint32_t>;
    struct Chunk {
        std::size_t offset;
        std::size_t size;
    };
    IncrTransfers(std::size_t size, std::size_t chunk_size,
                  std::chrono::milliseconds timeout = std::chrono::seconds(10))
        : size_(size), chunk_size_(std::max<std::size_t>(1, chunk_size)), timeout_(timeout) {}

    bool contains(Key key) const { return transfers_.count(key) != 0; }
    bool empty() const { return transfers_.empty(); }
    bool begin(Key key, Clock::time_point now) {
        if (transfers_.size() >= 64) return false;
        return transfers_.emplace(key, Transfer{0, now}).second;
    }
    std::optional<Chunk> deleted(Key key, Clock::time_point now) {
        const auto it = transfers_.find(key);
        if (it == transfers_.end()) return std::nullopt;
        auto& transfer = it->second;
        const Chunk chunk{transfer.offset, std::min(chunk_size_, size_ - transfer.offset)};
        transfer.offset += chunk.size;
        transfer.updated = now;
        // The final zero-length property is a separate handshake, including
        // when the payload size is an exact multiple of the chunk size.
        if (!chunk.size) transfers_.erase(it);
        return chunk;
    }
    void erase(Key key) { transfers_.erase(key); }
    bool watches(std::uint32_t window) const {
        const auto it = transfers_.lower_bound({window, 0});
        return it != transfers_.end() && it->first.first == window;
    }
    void destroyed(std::uint32_t window) {
        auto it = transfers_.lower_bound({window, 0});
        while (it != transfers_.end() && it->first.first == window) it = transfers_.erase(it);
    }
    std::vector<Key> expire(Clock::time_point now) {
        std::vector<Key> expired;
        for (auto it = transfers_.begin(); it != transfers_.end();) {
            if (now - it->second.updated >= timeout_) {
                expired.push_back(it->first);
                it = transfers_.erase(it);
            } else {
                ++it;
            }
        }
        return expired;
    }

   private:
    struct Transfer {
        std::size_t offset;
        Clock::time_point updated;
    };
    const std::size_t size_, chunk_size_;
    const std::chrono::milliseconds timeout_;
    std::map<Key, Transfer> transfers_;
};
}  // namespace clipsync
