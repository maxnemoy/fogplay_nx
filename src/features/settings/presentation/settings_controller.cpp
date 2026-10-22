#include "app/application_controller.hpp"

namespace fogplay {

void ApplicationController::changeVideoSettings(const fog::video::Settings& value, bool live) {
    videoSettings_ = value;
    bool saved = false;
    try {
        fog::video::saveSettings(std::string(dataDir) + "/video-settings.json", videoSettings_);
        saved = true;
    } catch (...) { diagnostic("Video settings save failed; no settings payload logged."); }
    videoSaveStatus_ = saved ? "Сохранено на SD." : "Не сохранено на SD. После перезапуска изменения потеряются.";
    if (live) {
        const bool requested = stream_ && stream_->updateVideoSettings(videoSettings_);
        videoSaveStatus_ += requested ? " Запрошено; подтверждение сервера не гарантируется." :
            " Канал недоступен; для текущей сессии запрос не отправлен.";
    }
    overlay_.setVideoSettings(videoSettings_);
    overlay_.setSaveStatus(videoSaveStatus_);
}

void ApplicationController::inputImageSettings(u64 keys) {
    if (keys & HidNpadButton_B) { screen_ = Screen::Settings; selection_ = 0; return; }
    if (keys & (HidNpadButton_Up | HidNpadButton_StickLUp)) selection_ = (selection_ + 3) % 4;
    if (keys & (HidNpadButton_Down | HidNpadButton_StickLDown)) selection_ = (selection_ + 1) % 4;
    int direction = 0;
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) direction = -1;
    else if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A)) direction = 1;
    if (!direction) return;
    const std::array<fog::video::Field, 4> fields{
        fog::video::Field::Codec, fog::video::Field::Quality, fog::video::Field::Bitrate, fog::video::Field::Keyframes};
    auto value = videoSettings_;
    if (fog::video::adjust(value, fields[selection_], direction, fog::Media::supportsH265()))
        changeVideoSettings(value, false);
    else if (selection_ == 0) videoSaveStatus_ = "H.265 недоступен в этой сборке. Используется H.264.";
}

void ApplicationController::drawImageSettings() {
    const auto area = ui_.content();
    const std::array<std::string, 4> labels{
        "Кодек: " + fog::video::codecLabel(videoSettings_),
        "Качество сервера: " + fog::video::qualityLabel(videoSettings_),
        "Запрошенный предел: " + std::to_string(videoSettings_.bitrateMbps) + " Мбит/с",
        "Ключевые кадры: " + fog::video::keyframeLabel(videoSettings_)};
    const std::array<std::string, 4> hints{
        fog::Media::supportsH265() ? "H.265 экспериментальный. Изменяется только до подключения." : "H.265 недоступен в этой сборке.",
        "Авто / Низкое / Среднее / Высокое / Ультра — запрос серверу.",
        "5–40 Мбит/с. Сервер может использовать другой фактический битрейт.",
        "При потере или каждые 2 / 5 / 10 с — интервал запроса, не частота видео."};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const int y = 145 + static_cast<int>(i) * 108;
        ui_.button(labels[i], {area.x, y, area.w, 57}, selection_ == i);
        ui_.text(hints[i], area.x + 14, y + 65, area.w - 28, muted);
    }
    ui_.text(videoSaveStatus_, area.x, 596, area.w, accent, false, 2);
}

void ApplicationController::inputConnectionSettings(u64 keys) {
    if (keys & HidNpadButton_B) { screen_ = Screen::Settings; selection_ = 2; return; }
    int direction = 0;
    if (keys & (HidNpadButton_Left | HidNpadButton_StickLLeft)) direction = -1;
    else if (keys & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_A)) direction = 1;
    auto next = connectionSettings_;
    if (!fog::connection::adjust(next, direction)) return;
    try {
        fog::connection::saveSettings(std::string(dataDir) + "/connection-settings.json", next);
        connectionSettings_ = next;
        connectionSaveStatus_ = "Сохранено на SD. Применится при следующем подключении.";
    } catch (...) {
        connectionSaveStatus_ = "Не удалось сохранить на SD. Режим не изменён.";
        diagnostic("Connection settings save failed; no settings payload logged.");
    }
}

void ApplicationController::drawConnectionSettings() {
    const auto area = ui_.content();
    ui_.button("Режим: " + fog::connection::label(connectionSettings_), {area.x, 154, area.w, 57}, true);
    ui_.text("Обычный: автоматический выбор пути, как в прежних версиях.", area.x + 14, 240, area.w - 28, muted);
    ui_.text("Авто: TURN после 2 обрывов за 2 мин, до конца текущей аренды.", area.x + 14, 310, area.w - 28, muted);
    ui_.text("Повторные неудачные попытки одного восстановления не считаются новым обрывом.", area.x + 14, 365, area.w - 28, muted, false, 2);
    ui_.text("Через TURN: подключаться через сервер ретрансляции с самого начала.", area.x + 14, 440, area.w - 28, muted);
    ui_.text(connectionSaveStatus_, area.x, 560, area.w, accent, false, 2);
}

}
