#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace stream {
// Fixed labels and numeric states only: never record remote messages or credentials.
class TransportHistory {
public:
    enum class Kind { Peer, Ice, Gathering, ChannelOpen, Closed, Failure };
    static constexpr std::size_t Capacity = 16;
    void record(Kind kind, int state, std::uint64_t at, bool terminal = false,
                long long videoAge = -1, long long audioAge = -1, long long controlAge = -1) noexcept {
        const Entry entry{kind, state, at};
        entries_[next_] = entry;
        next_ = (next_ + 1) % Capacity;
        if (size_ < Capacity) ++size_;
        if (terminal && !hasTerminal_) {
            hasTerminal_ = true;
            terminal_ = entry;
            terminalVideoAge_ = videoAge;
            terminalAudioAge_ = audioAge;
            terminalControlAge_ = controlAge;
        }
    }
    std::string diagnostics() const {
        std::string result = " firstTerminal=";
        result += hasTerminal_ ? text(terminal_) : "none";
        if (hasTerminal_) {
            result += " terminalRtpAgeMs=" + std::to_string(terminalVideoAge_) +
                " terminalAudioAgeMs=" + std::to_string(terminalAudioAge_) +
                " terminalControlAgeMs=" + std::to_string(terminalControlAge_);
        }
        result += " transportHistory=";
        for (std::size_t i = 0; i < size_; ++i) {
            if (i) result += ',';
            result += text(entries_[(next_ + Capacity - size_ + i) % Capacity]);
        }
        return result;
    }
private:
    struct Entry { Kind kind = Kind::Peer; int state = 0; std::uint64_t at = 0; };
    static const char* name(Kind kind) {
        switch (kind) {
        case Kind::Peer: return "peer";
        case Kind::Ice: return "ice";
        case Kind::Gathering: return "gathering";
        case Kind::ChannelOpen: return "channel-open";
        case Kind::Closed: return "ws-closed";
        case Kind::Failure: return "callback-failure";
        }
        return "unknown";
    }
    static std::string text(const Entry& entry) {
        return std::string(name(entry.kind)) + ':' + std::to_string(entry.state) + '@' + std::to_string(entry.at);
    }
    std::array<Entry, Capacity> entries_{};
    std::size_t next_ = 0, size_ = 0;
    bool hasTerminal_ = false;
    Entry terminal_{};
    long long terminalVideoAge_ = -1, terminalAudioAge_ = -1, terminalControlAge_ = -1;
};
}
