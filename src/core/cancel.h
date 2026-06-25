#pragma once
#include <atomic>
namespace suji {
struct CancelToken {
    std::atomic<bool> cancelled{false};
    void cancel()           { cancelled.store(true, std::memory_order_release); }
    bool is_cancelled() const { return cancelled.load(std::memory_order_acquire); }
};
}
