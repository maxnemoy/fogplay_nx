#pragma once
#include "features/session/infrastructure/api.hpp"
#include "features/catalog/domain/catalog.hpp"
#include "features/settings/domain/controller_layout.hpp"
#include "features/settings/infrastructure/controller_layout_storage.hpp"
#include "features/catalog/infrastructure/latency.hpp"
#include "features/catalog/presentation/home_catalog.hpp"
#include "features/catalog/infrastructure/recent_games.hpp"
#include "features/streaming/application/remote_input.hpp"
#include "features/streaming/presentation/stream_overlay.hpp"
#include "features/settings/domain/video_settings.hpp"
#include "features/settings/infrastructure/video_settings_storage.hpp"
#include "features/settings/infrastructure/connection_settings_storage.hpp"
#include "features/session/application/session.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/domain/video_recovery.hpp"
#include "features/streaming/domain/transport_reconnect.hpp"
#include "features/streaming/domain/stream_input_gate.hpp"
#include "features/streaming/domain/loop_timing.hpp"
#include "core/logging/run_log.hpp"
#include "features/streaming/infrastructure/stream.hpp"
#include "features/session/application/checkout.hpp"
#include "core/storage/storage.hpp"
#include "app/presentation/ui.hpp"
#include "features/catalog/infrastructure/artwork.hpp"
#include "features/catalog/infrastructure/steam_artwork.hpp"
#include "features/catalog/presentation/catalog_artwork.hpp"
#include <switch.h>
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/switch/switch_runtime.hpp"
#include "app/app_diagnostics.hpp"
#include "app/presentation/presentation_text.hpp"

namespace fogplay {
using fog::Json;
using platform::Runtime;
using platform::checkResult;
using platform::checkStorage;
using platform::keyboard;
using diagnostics::diagnostic;
using diagnostics::safeText;
using diagnostics::runLog;
using diagnostics::PlaybackTiming;
using diagnostics::diagnosticTiming;
inline constexpr const char* dataDir = platform::dataDir;
inline constexpr fog::controller::FaceButtonMasks faceMasks{HidNpadButton_A,HidNpadButton_B,HidNpadButton_X,HidNpadButton_Y};
inline constexpr SDL_Color white{234, 238, 245, 255};
inline constexpr SDL_Color muted{163, 178, 200, 255};
inline constexpr SDL_Color accent{107, 216, 190, 255};

enum class Screen { Games, GameDetail, Computers, Details, History, Settings, ImageSettings, ConnectionSettings, Checkout, ManualResolution, Consent, Auth, Sessions, ConnectConsent, Streaming, StopConfirm, Stopping, Recovery, Genres };
enum class Task { Games, GameDetail, Computers, History, Create, CheckoutPoll, CheckoutStop, ManualResolution, Phone, Captcha, Otp, Profile, Sessions, Finish, Recovery, RestoreAuth };

// Main-thread presentation coordinator. Workers are joined before their state is read.
class ApplicationController {
public:
    ApplicationController(Runtime& runtime, const std::string& id);
    ~ApplicationController();
    void run();
private:
    void armExitHandling();
    void disarmExitHandling();
    std::string playbackTimings() const;
    void logOutputTimings(bool final);
    void closeStream() noexcept;
    bool closeTransport(bool final) noexcept;
    bool startTransport();
    void retryTransport(const char* reason);
    void shutdownSession() noexcept;
    void syncPending();
    void logCheckoutResult(Task phase);
    void queueCheckoutStop();
    void handleCheckout();
    void pollCheckout(bool manual);
    void createSession();
    void loadHistory();
    void loadGameDetail();
    void loadSessions();
    void recoverSession();
    void connectSession();
    void startStream(const std::string& hash, bool createdFromCatalog = true);
    void rememberDecodedGame() noexcept;
    void beginFinish(const char* reason = "rental-guard");
    void recoverVideoError(const fog::VideoDecodeError& error, const stream::Packet* packet);
    void pumpStream(PadState& pad, u64 held, u64 down);
    void feedStreamAudio(const stream::Packet& packet);
    void drainStreamAudio();
    SDL_Rect videoRect() const;
    bool sendRemote(const Json& command);
    void releaseMouse() noexcept;
    void neutralizeStreamInput();
    void suspendVideoInput();
    void overlayTransition();
    void changeVideoSettings(const fog::video::Settings& value, bool live);
    void inputImageSettings(u64 keys);
    void drawImageSettings();
    void inputConnectionSettings(u64 keys);
    void drawConnectionSettings();
    void overlayAction(const fog::stream::OverlayAction& action);
    bool handleStreamOverlay(PadState& pad, u64 held, u64 down);
    void updateMouse(PadState& pad,u64 held,u64 down,const HidTouchScreenState& touch);
    void drawStreamOverlay();
    static float axis(int value);
    void text(const std::string& value, int x, int y, SDL_Color color = white, int width = 1190, bool title = false);
    void box(int x, int y, int w, int h, SDL_Color color);
    void start(Task kind, std::function<Json()> work);
    void loadGames();
    void requestCatalog(bool manual = false);
    void pollCatalog();
    void retryCatalog();
    void moveCatalog(u64 keys);
    void selectCatalogGame(const Json& game);
    void loadComputers();
    void setCaptcha(const Json& response);
    void poll();
    void beginLogin();
    void enterCode();
    void moveGrid(u64 keys, std::size_t count, std::size_t columns = 4);
    void searchGames();
    std::string latencyText(const Json& computer) const;
    std::size_t selectedGenreIndex() const;
    std::size_t selectedGenreChip() const;
    void chooseGenre(int id);
    void inputGenres(u64 keys);
    void inputGenreBar(u64 keys);
    void input(u64 keys);
    SDL_Texture* cover(const Json& game);
    void draw();
    Runtime& rt_;
    fog::Api api_;
    const std::string catalogDeviceId_;
    fog::Catalog catalog_;
    fog::LatencyService latency_;
    int genreId_ = 0;
    bool genreBarFocused_ = false;
    std::size_t genreChip_ = 5, genreDialogSelection_ = 0;
    fog::HomeCatalogNavigation homeCatalog_;
    fog::RecentGames recentGames_{std::string(dataDir) + "/recent-games.json"};
    fog::RecentLaunch recentLaunch_;
    std::future<bool> recentSave_;
    bool recentSavePending_ = false;
    void pollRecentSave() noexcept;
    std::optional<fog::RecentGame> selectedRecentGame_;
    std::shared_ptr<fog::Api> catalogApi_;
    std::future<Json> catalogPending_;
    fog::Catalog::Request catalogRequest_{0, 1, {}};
    fog::SessionGuard guard_;
    fog::CheckoutGuard checkout_;
    fog::ui::Ui ui_;
    fog::ArtworkCache artwork_;
    fog::SteamArtworkResolver steamArtwork_;
    Json history_ = Json::array(), detail_ = Json::object(), autoComputers_ = Json::object();
    bool detailsFromAutomatic_ = false;
    std::size_t automaticSelection_ = 1;
    bool ownSteam_ = false, sidebar_ = false, checkoutAutoPoll_ = false, stopAfterTask_ = false;
    bool authSaveFailed_ = false;
    int nav_ = 1;
    Screen loginReturn_ = Screen::Games;
    std::chrono::steady_clock::time_point checkoutAt_{};
    std::unique_ptr<stream::Client> stream_;
    fog::stream::Overlay overlay_;
    fog::video::Settings videoSettings_;
    fog::connection::Settings connectionSettings_;
    fog::connection::RelaySelection relaySelection_;
    fog::controller::Settings controllerSettings_;
    bool firstFramePresented_ = false;
    std::string videoSaveStatus_ = "Изменения сохраняются сразу на SD.";
    std::string connectionSaveStatus_ = "Изменения сохраняются на SD. Применяются при следующем подключении.";
    fog::stream::StreamInputGate streamInputGate_;
    bool touchWasDown_ = false, touchDrag_ = false;
    bool mouseLeft_ = false, mouseRight_ = false;
    float pointerX_ = 640.0f, pointerY_ = 360.0f;
    std::chrono::steady_clock::time_point mouseAt_{}, lastPointerSent_{}, overlayNoticeAt_{}, overlayHintAt_{};
    std::string overlayNotice_;
    std::string lastStreamState_, lastStreamDiagnostics_;
    std::unique_ptr<fog::Media> media_;
    fog::VideoRecovery videoRecovery_;
    fog::stream::TransportReconnect transportReconnect_;
    // Private rental identity, retained only until final close; never logged.
    std::string streamHash_;
    std::uint64_t videoWorkUs_ = 0, videoDecodeErrors_ = 0, lastVideoGeneration_ = 0;
    PlaybackTiming drainTiming_, presentTiming_;
    fog::stream::LoopTiming loopTiming_;
    std::chrono::steady_clock::time_point loopTimingWindowAt_{};
    std::chrono::steady_clock::time_point loopTimingLifetimeAt_{};
    PlaybackTiming transportPumpTiming_, videoFeedTiming_, decoderResetTiming_, audioFeedTiming_, prepareTiming_;
    std::uint64_t drainVideoTotal_ = 0, drainVideoMax_ = 0, drainVideoBudgetHits_ = 0;
    std::uint64_t videoQueueWaitMaxMs_ = 0, audioQueueWaitMaxMs_ = 0;
    std::uint64_t lastPumpWorkUs_ = 0, slowPlaybackCycles_ = 0;
    std::chrono::steady_clock::time_point lastSlowPlaybackLog_{};
    std::uint64_t pumpGapMaxUs_ = 0, outsideMeasuredPumpMaxUs_ = 0, drainBudgetHits_ = 0, catchupWindows_ = 0;
    std::chrono::steady_clock::time_point lastPumpAt_{}, lastProtocolLog_{};
    bool videoLossWaiting_ = false;
    std::chrono::steady_clock::time_point lastVideoEventLog_{};
    unsigned sessionSequence_ = 0;
    Json sessions_ = Json::array();
    bool sessionPending_ = false, exitLocked_ = false, exitAfterStop_ = false, shutdownDone_ = false;
    bool stopStartedOnExit_ = false;
    std::chrono::steady_clock::time_point streamStarted_{}, lastVideo_{}, lastInput_{}, retryAt_{}, lastMediaLog_{};
    std::future<Json> pending_;
    Task task_ = Task::RestoreAuth;
    Screen screen_ = Screen::Games;
    Json computers_ = Json::array();
    std::size_t selection_ = 0;
    int computerPage_ = 1, selectedGameId_ = 0;
    std::string search_, selectedGameName_, status_ = "Готово";
    std::string checkoutStatus_ = "Ожидаем готовность сервера…";
    fog::AuthStep authStep_ = fog::AuthStep::Unknown;
    SDL_Texture* captcha_ = nullptr;
    int captchaWidth_ = 1, captchaHeight_ = 1;
};
}
