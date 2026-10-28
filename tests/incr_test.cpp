#include "incr.hpp"

#include <cstdlib>
#include <iostream>

using clipsync::IncrTransfers;
namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    using namespace std::chrono_literals;
    const auto now = IncrTransfers::Clock::now();
    IncrTransfers transfers(3213813, 65536);
    check(transfers.begin({1, 10}, now), "slow reader did not start");
    check(transfers.begin({2, 10}, now), "concurrent reader did not start");
    check(transfers.begin({1, 11}, now), "different property on same window did not start");
    check(!transfers.begin({1, 10}, now), "active property was overwritten");
    check(!transfers.deleted({99, 10}, now), "unrelated window advanced a transfer");
    check(!transfers.deleted({1, 99}, now), "unrelated property advanced a transfer");
    for (const auto key : {IncrTransfers::Key{2, 10}, IncrTransfers::Key{1, 11}}) {
        std::size_t received = 0;
        while (true) {
            const auto chunk = transfers.deleted(key, now + 1s);
            check(chunk.has_value(), "stalled reader blocked another transfer");
            check(chunk->offset == received && chunk->size <= 65536, "wrong chunk boundaries");
            received += chunk->size;
            if (!chunk->size) break;
        }
        check(received == 3213813, "large PNG bytes lost");
        check(!transfers.contains(key), "completed transfer leaked");
    }
    check(transfers.contains({1, 10}), "other readers advanced the stalled transfer");
    check(transfers.expire(now + 9s).empty(), "premature timeout");
    check(transfers.expire(now + 10s) == std::vector<IncrTransfers::Key>{{1, 10}}, "stalled transfer not expired");
    check(transfers.begin({1, 10}, now + 11s), "expired property cannot be reused");
    check(transfers.begin({1, 11}, now + 11s), "reader did not start");
    check(transfers.begin({2, 10}, now + 11s), "other reader did not start");
    transfers.destroyed(1);
    check(!transfers.watches(1) && transfers.watches(2), "destroyed-window cleanup affected another reader");
    (void)transfers.deleted({2, 10}, now + 19s);
    check(transfers.expire(now + 22s).empty(), "active transfer was expired by its total age");
    check(transfers.expire(now + 29s).size() == 1 && transfers.empty(), "idle timeout was not refreshed");

    IncrTransfers exact(131072, 65536);
    exact.begin({1, 1}, now);
    check(exact.deleted({1, 1}, now)->size == 65536, "first exact chunk");
    check(exact.deleted({1, 1}, now)->size == 65536, "second exact chunk");
    check(exact.contains({1, 1}), "missing final empty-property handshake");
    check(exact.deleted({1, 1}, now)->size == 0 && exact.empty(), "missing final empty chunk");

    IncrTransfers bounded(100, 10);
    for (unsigned i = 1; i <= 64; ++i) check(bounded.begin({i, 1}, now), "capacity too small");
    check(!bounded.begin({65, 1}, now), "unbounded pending transfers");
    bounded.destroyed(1);
    check(bounded.begin({65, 1}, now), "capacity not released");
    std::cout << "INCR concurrent readers, interruption, expiry and bounds passed\n";
}
