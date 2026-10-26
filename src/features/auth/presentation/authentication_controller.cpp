#include "app/application_controller.hpp"

namespace fogplay {

void ApplicationController::setCaptcha(const fog::AuthResult& response) {
    if (captcha_) { SDL_DestroyTexture(captcha_); captcha_ = nullptr; }
    const auto& image = response.captchaImage;
    SDL_RWops* input = SDL_RWFromConstMem(image.data(), static_cast<int>(image.size()));
    if (!input) throw std::runtime_error("Cannot read captcha image.");
    SDL_Surface* surface = IMG_Load_RW(input, 1);
    if (!surface) throw std::runtime_error("Cannot decode captcha PNG.");
    if (surface->w > 2048 || surface->h > 2048) { SDL_FreeSurface(surface); throw std::runtime_error("Captcha dimensions exceed limit."); }
    captchaWidth_ = surface->w; captchaHeight_ = surface->h;
    captcha_ = SDL_CreateTextureFromSurface(rt_.renderer, surface);
    SDL_FreeSurface(surface);
    if (!captcha_) throw std::runtime_error("Cannot create captcha texture.");
}

void ApplicationController::beginLogin() {
    const auto phone = keyboard("Phone number +7...", "+7", 20);
    if (!phone || phone->empty()) {
        screen_ = sessionPending_ ? Screen::Recovery : Screen::Games;
        selection_ = 0;
        if (!sessionPending_ && catalog_.items().empty()) loadGames();
        return;
    }
    const auto enteredPhone = *phone;
    auth_.validateLogin({enteredPhone,{}});
    screen_ = Screen::Auth;
    authStep_ = fog::AuthStep::Unknown;
    start(Task::Phone, [this, enteredPhone] { return auth_.begin({enteredPhone,{}}); });
}

void ApplicationController::enterCode() {
    if (authStep_ == fog::AuthStep::Captcha) {
        const auto code = keyboard("Text from captcha image", "", 32);
        if (code && !code->empty()) start(Task::Captcha, [this, value=*code] { return auth_.answer({fog::AuthStep::Captcha,value}); });
    } else if (authStep_ == fog::AuthStep::Otp) {
        const auto code = keyboard("SMS code", "", 12, true);
        if (code && !code->empty()) start(Task::Otp, [this, value=*code] { return auth_.answer({fog::AuthStep::Otp,value}); });
    }
}

}
