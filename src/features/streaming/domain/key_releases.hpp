#pragma once
#include <array>
#include <string>

namespace fog::stream {
// Only the four overlay keys exist. A key-up belongs to the transport that
// accepted its key-down and survives video suspension or an enqueue rejection.
class KeyReleases {
public:
    void reset() { pending_ = {}; }
    void pressed(const std::string& code) {
        for (std::size_t i=0;i<codes_.size();++i) if (code == codes_[i]) pending_[i] = true;
    }
    template<class Send> bool release(const std::string& code, Send&& send) {
        for (std::size_t i=0;i<codes_.size();++i) if (code == codes_[i]) {
            pending_[i] = true;
            if (!send(code)) return false;
            pending_[i] = false;
            return true;
        }
        return false;
    }
    template<class Send> void retry(Send&& send) {
        for (std::size_t i=0;i<codes_.size();++i)
            if (pending_[i] && send(codes_[i])) pending_[i] = false;
    }
    bool pending(const std::string& code) const {
        for (std::size_t i=0;i<codes_.size();++i) if (code == codes_[i]) return pending_[i];
        return false;
    }
private:
    inline static constexpr std::array<const char*,4> codes_{"Enter","Tab","Backspace","Escape"};
    std::array<bool,4> pending_{};
};
}
