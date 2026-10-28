#pragma once
#include <array>
#include <cstddef>

namespace fog::stream {
// Accepted means queued by the transport, not acknowledged by the remote PC.
// Releases survive UI transitions but belong only to the current transport.
class MouseButtons {
public:
    void reset() { accepted_ = {}; releasePending_ = {}; }

    bool differs(bool left, bool right) const {
        return accepted_[0] != left || accepted_[1] != right ||
            releasePending_[0] || releasePending_[1];
    }

    void releaseAll() {
        for (std::size_t i = 0; i < accepted_.size(); ++i)
            releasePending_[i] = releasePending_[i] || accepted_[i];
    }

    template<class Send>
    void retryReleases(Send&& send) {
        for (std::size_t i = 0; i < accepted_.size(); ++i)
            if (releasePending_[i] && send(false, button(i))) {
                accepted_[i] = false;
                releasePending_[i] = false;
            }
    }

    template<class Send>
    void update(bool left, bool right, Send&& send) {
        const std::array<bool, 2> desired{left, right};
        for (std::size_t i = 0; i < accepted_.size(); ++i) {
            // Finish a previous gesture before considering another press. A
            // rejected press is never retained after the physical release.
            if (releasePending_[i]) {
                if (send(false, button(i))) {
                    accepted_[i] = false;
                    releasePending_[i] = false;
                }
                continue;
            }
            if (desired[i] == accepted_[i]) continue;
            if (!desired[i]) releasePending_[i] = true;
            if (send(desired[i], button(i))) {
                accepted_[i] = desired[i];
                releasePending_[i] = false;
            }
        }
    }

private:
    static int button(std::size_t index) { return index == 0 ? 0 : 2; }
    std::array<bool, 2> accepted_{};
    std::array<bool, 2> releasePending_{};
};
}
