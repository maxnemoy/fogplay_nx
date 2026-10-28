#pragma once
#include "features/session/presentation/session_rating.hpp"
#include "features/session/application/paired_checkout.hpp"
#include <functional>
#include <future>
#include <stdexcept>
#include <variant>
using u64 = unsigned long long;
inline constexpr u64 HidNpadButton_A=1,HidNpadButton_B=2,HidNpadButton_Up=4,HidNpadButton_Down=8,
    HidNpadButton_Left=16,HidNpadButton_Right=32,HidNpadButton_StickLUp=64,HidNpadButton_StickLDown=128,
    HidNpadButton_StickLLeft=256,HidNpadButton_StickLRight=512;
inline bool ratingTestFocused = true;
inline constexpr int AppletFocusState_InFocus = 1;
inline int appletGetFocusState() { return ratingTestFocused ? AppletFocusState_InFocus : 0; }
namespace fogplay {
enum class Screen { Profile, PlayStation, SessionRating };
enum class Task { SessionRatingLoad, SessionRatingSubmit };
using TaskResult = std::variant<std::monostate,std::optional<fog::SessionRatingForm>,fog::SessionRatingResult>;
struct TestUi {};
struct TestRatingRepository {
    bool supported = true, loadThrows = false, sendThrows = false;
    unsigned loads = 0, sends = 0;
    std::optional<fog::RentalId> requested;
    std::optional<fog::SessionRatingForm> response;
    fog::SessionRatingRequest submitted;
    bool sessionRatingSupported() const noexcept { return supported; }
    std::optional<fog::SessionRatingForm> ratingForm(const std::optional<fog::RentalId>& target) {
        ++loads; requested = target;
        if (loadThrows) throw std::runtime_error("synthetic private provider reply");
        return response;
    }
    fog::SessionRatingResult submitRating(const fog::SessionRatingRequest& value) {
        ++sends; submitted = value;
        if (sendThrows) throw std::runtime_error("synthetic private provider reply");
        return {value.rental,value.score};
    }
};
struct TestAuth { bool active = true; bool authenticated() const { return active; } };
struct TestServices {
    TestRatingRepository repository;
    TestAuth authentication;
    TestRatingRepository& rentals() { return repository; }
    TestAuth& auth() { return authentication; }
};
std::optional<std::string> keyboard(const char*,const std::string&,int,bool=false);
class ApplicationController {
public:
    bool openSessionRating(std::optional<fog::RentalId> rental = std::nullopt);
    bool offerSessionRating(const fog::PairedCheckoutResult& result);
    void completeSessionRatingLoad(std::optional<fog::SessionRatingForm>);
    void completeSessionRatingSubmit(const fog::SessionRatingResult&);
    void submitSessionRating();
    void inputSessionRating(u64);
    void drawSessionRating();
    void start(Task task,std::function<TaskResult()> work) {
        if (dispatchThrows) throw std::runtime_error("synthetic dispatch failure");
        task_ = task; ++dispatches; pending_ = std::async(std::launch::deferred,std::move(work));
    }
    TaskResult join() { return pending_.get(); }
    TestServices service;
    TestServices* pairedServices_ = &service;
    bool sessionPending_ = false, exitAfterStop_ = false, shutdownDone_ = false, sidebar_ = true;
    bool dispatchThrows = false;
    bool ratingFocusLost_ = false, stopAfterTask_ = false;
    unsigned dispatches = 0;
    Screen screen_ = Screen::Profile;
    Task task_ = Task::SessionRatingLoad;
    std::future<TaskResult> pending_;
    fog::rating::Presentation rating_;
    TestUi ui_;
    std::string status_;
};
}
