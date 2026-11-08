#pragma once
#include <cstdint>
namespace uvco {
namespace detail {
// Pure injected-time guard. A recovered queue resets the entire congestion episode.
class KcpOutputPressure {
  public:
    bool observe(std::uint64_t now, std::uint64_t totalDrops, bool room, std::uint64_t duration,
                 std::uint64_t threshold) noexcept {
        if (room || totalDrops < lastDrops_ || (active_ && now < since_)) {
            active_ = false;
            drops_ = 0;
            lastDrops_ = totalDrops;
            return false;
        }
        auto added = totalDrops - lastDrops_;
        lastDrops_ = totalDrops;
        if (!active_ && added != 0) {
            active_ = true;
            since_ = now;
            drops_ = 0;
        }
        // Saturate rather than wrap diagnostic accounting during a long episode.
        if (UINT64_MAX - drops_ < added)
            drops_ = UINT64_MAX;
        else
            drops_ += added;
        return active_ && drops_ >= threshold && now - since_ >= duration;
    }

  private:
    std::uint64_t since_ = 0, lastDrops_ = 0, drops_ = 0;
    bool active_ = false;
};
}
}
