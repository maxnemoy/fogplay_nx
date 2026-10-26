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
    if (loginAuth().capabilities().method == fog::LoginMethod::Credentials) {
        diagnostic("Login selected: provider=FH method=credentials");
        screen_ = Screen::Profile;
        status_.clear();
        try {
            auto identifier = keyboard("FogHub: login", "", 128);
            if (!identifier || identifier->empty()) {
                status_ = ui_.controllerHints(identifier ? "FogHub: логин не введён. A — повторить." :
                    "FogHub: ввод логина не подтверждён. A — повторить.");
                diagnostic(identifier ? "Login stopped: provider=FH field=login reason=empty" :
                    "Login stopped: provider=FH field=login reason=not_confirmed");
                return;
            }
            auto secret = keyboard("FogHub: password", "", 256, true);
            if (!secret || secret->empty()) {
                status_ = ui_.controllerHints(secret ? "FogHub: пароль не введён. A — повторить." :
                    "FogHub: ввод пароля не подтверждён. A — повторить.");
                diagnostic(secret ? "Login stopped: provider=FH field=password reason=empty" :
                    "Login stopped: provider=FH field=password reason=not_confirmed");
                return;
            }
            fog::LoginRequest request{std::move(*identifier),std::move(*secret)};
            loginAuth().validateLogin(request);
            screen_ = Screen::Auth; authStep_ = fog::AuthStep::Unknown;
            start(Task::Phone,[this,request=std::move(request)]() mutable {
                struct Erase { std::string& value; ~Erase() { std::fill(value.begin(),value.end(),'\0'); } } erase{request.secret};
                return loginAuth().begin(request);
            });
        } catch (...) {
            screen_ = Screen::Profile;
            diagnostic("Login failed: provider=FH stage=credentials; details omitted.");
            throw;
        }
        return;
    }
    const auto phone = keyboard("Phone number +7...", "+7", 20);
    if (!phone || phone->empty()) {
        screen_ = sessionPending_ ? Screen::Recovery : Screen::Games;
        selection_ = 0;
        if (!sessionPending_ && catalog_.items().empty()) loadGames();
        return;
    }
    const auto enteredPhone = *phone;
    loginAuth().validateLogin({enteredPhone,{}});
    screen_ = Screen::Auth;
    authStep_ = fog::AuthStep::Unknown;
    start(Task::Phone, [this, enteredPhone] { return loginAuth().begin({enteredPhone,{}}); });
}

void ApplicationController::enterCode() {
    if (authStep_ == fog::AuthStep::Captcha) {
        const auto code = keyboard("Text from captcha image", "", 32);
        if (code && !code->empty()) start(Task::Captcha, [this, value=*code] { return loginAuth().answer({fog::AuthStep::Captcha,value}); });
    } else if (authStep_ == fog::AuthStep::Otp) {
        const auto code = keyboard("SMS code", "", 12, true);
        if (code && !code->empty()) start(Task::Otp, [this, value=*code] { return loginAuth().answer({fog::AuthStep::Otp,value}); });
    }
}

}
