#pragma once
#include "core/constants/storage_paths.hpp"
#include "core/constants/ui_text.hpp"
#include "app/services.hpp"
#include "features/playstation/domain/computer.hpp"
#include "features/playstation/application/catalog_progress.hpp"

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
#include "features/settings/infrastructure/rumble_settings_storage.hpp"
#include "features/session/application/session.hpp"
#include "features/streaming/infrastructure/media.hpp"
#include "features/streaming/domain/video_recovery.hpp"
#include "features/streaming/domain/transport_reconnect.hpp"
#include "features/streaming/domain/stream_input_gate.hpp"
#include "features/streaming/domain/mouse_buttons.hpp"
#include "features/streaming/domain/loop_timing.hpp"
#include "core/logging/run_log.hpp"
#include "features/streaming/domain/streaming_client.hpp"
#include "features/session/application/checkout.hpp"
#include "features/session/application/paired_checkout.hpp"
#include "features/session/presentation/session_rating.hpp"
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
#include <variant>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/switch/switch_runtime.hpp"
#include "platform/switch/ps_sensors.hpp"
#include "platform/switch/rumble.hpp"
#include "features/playstation/domain/touch_trace.hpp"
#include "features/playstation/infrastructure/native_input.hpp"
#include "app/app_diagnostics.hpp"
#include "app/presentation/presentation_text.hpp"

namespace uiText = ::fog::constants::ui;
namespace storagePaths = ::fog::constants::storage;

namespace fogplay {

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

enum class Screen { Profile, Games, GameDetail, Computers, Details, History, Settings, ImageSettings, ConnectionSettings, Checkout, ManualResolution, Consent, Auth, Sessions, ConnectConsent, Streaming, StopConfirm, Stopping, Recovery, Genres, PlayStation, PlayStationDetails, SessionRating, RumbleSettings, ControllerSettings };
enum class Task { Games, GameDetail, Computers, History, Create, CheckoutPoll, CheckoutStop, ManualResolution, Phone, Captcha, Otp, Profile, Sessions, Finish, Recovery, RestoreAuth, PlayStation, PairedCreate, PairedPoll, PairedStop, ProviderProfile, ProviderForget, SessionRatingLoad, SessionRatingSubmit };

struct RestoredProviders {
    bool mts = false, foghub = false;
    std::optional<fog::UserProfile> mtsProfile, foghubProfile;
    bool mtsFailed = false, foghubFailed = false;
};
struct RecoveryResult { bool matched = false; };
struct VerifiedOffer { fog::StartRentalRequest request; };
struct ChangedOffer { std::vector<fog::Computer> computers; };
using TaskResult = std::variant<std::monostate, RestoredProviders, fog::RestoreAuthResult, fog::AuthResult,
    fog::UserProfile, fog::GameDetail, fog::Page<fog::Computer>, std::vector<fog::HistoryEntry>,
    std::vector<fog::Rental>, fog::FinishResult, RecoveryResult, VerifiedOffer, ChangedOffer,
    std::optional<fog::SessionRatingForm>, fog::SessionRatingResult>;

// Main-thread presentation coordinator. Workers are joined before their state is read.
class ApplicationController {
public:
    ApplicationController(Runtime& runtime, std::unique_ptr<Services> services);
    ~ApplicationController();
    void run();
private:
    Services* providerServices(fog::ProviderId id) const;
    fog::AuthRepository& loginAuth();
    fog::AccountRepository& loginAccount();
    void openProfile();
    void refreshProfile();
    RestoredProviders restoreProviders();
    void handlePairedCheckout();
    bool openSessionRating(std::optional<fog::RentalId> rental = std::nullopt);
    bool offerSessionRating(const fog::PairedCheckoutResult& result);
    void completeSessionRatingLoad(std::optional<fog::SessionRatingForm> form);
    void completeSessionRatingSubmit(const fog::SessionRatingResult& result);
    void submitSessionRating();
    void inputSessionRating(u64 keys);
    void drawSessionRating();
    bool pairedPending() const { return pairedPending_; }
    void armExitHandling();
    void disarmExitHandling();
    std::string playbackTimings() const;
    void logOutputTimings(bool final);
    void logIncomingControl(bool final = false) noexcept;
    void closeStream() noexcept;
    void stopRumble() noexcept;
    void updateRumble(PadState& pad, bool focused);
    void samplePlayStationInput(PadState& pad);
    void logPlayStationInput(bool final);
    void dispatchPlayStationInput(bool allowed);
    void releasePlayStationInput();
    bool closeTransport(bool final) noexcept;
    bool startTransport(bool resumeOnly = false);
    bool requestPairedVideoReconnect();
    bool completePairedVideoReconnect();
    void requestPairedTransportReconnect();
    bool transportRecoveryAllowed() const;
    void stopTransportRecovery(const char* reason);
    void recordTransportBreak();
    bool handleTransportState(std::chrono::steady_clock::time_point now);
    bool advanceTransportRecovery(std::chrono::steady_clock::time_point now);
    void requestTransportRecovery();
    void completeTransportRecovery(fog::stream::TransportReconnect::Preparation preparation);
    bool completePairedTransportReconnect();
    void retryTransport(const char* reason);
    void shutdownSession() noexcept;
    void syncPending();
    void logCheckoutResult(Task phase);
    void queueCheckoutStop();
    void handleCheckout();
    void pollCheckout(bool manual);
    void createSession();
    void createPairedSession(const fog::Computer& pc);
    void loadHistory();
    void loadGameDetail();
    void loadSessions();
    void recoverSession();
    void connectSession();
    void startStream(const fog::RentalId& rental, bool createdFromCatalog = true,
                     const fog::EntityId& computer = {});
    void rememberDecodedGame() noexcept;
    void beginFinish(const char* reason = "rental-guard");
    void recoverVideoError(const fog::VideoDecodeError& error, const stream::Packet* packet);
    void pumpStream(PadState& pad, u64 held, u64 down);
    void feedStreamAudio(const stream::Packet& packet);
    void drainStreamAudio();
    SDL_Rect videoRect() const;
    bool sendRemote(const fog::remote::Input& command);
    void releaseMouse() noexcept;
    void retryMouseReleases() noexcept;
    void neutralizeStreamInput();
    void suspendVideoInput();
    void overlayTransition();
    void traceStreamMenu(const char* event, u64 held = 0, u64 down = 0);
    void observeStreamMenuInput(bool focused, u64 held, u64 down);
    void streamMenuPresented(bool videoDrawn);
    void changeVideoSettings(const fog::video::Settings& value, bool live);
    void inputImageSettings(u64 keys);
    void drawImageSettings();
    void inputConnectionSettings(u64 keys);
    void drawConnectionSettings();
    void inputRumbleSettings(u64 keys);
    void inputControllerSettings(u64 keys);
    void drawControllerSettings();
    void drawRumbleSettings();
    void changeRumbleSettings(const fog::rumble::Settings& value);
    void overlayAction(const fog::stream::OverlayAction& action);
    bool handleStreamOverlay(PadState& pad, u64 held, u64 down);
    void updateMouse(PadState& pad,u64 held,u64 down,const HidTouchScreenState& touch);
    void drawStreamOverlay();
    static float axis(int value);
    void text(const std::string& value, int x, int y, SDL_Color color = white, int width = 1190, bool title = false);
    void box(int x, int y, int w, int h, SDL_Color color);
    void start(Task kind, std::function<TaskResult()> work);
    void loadGames();
    void requestCatalog(bool manual = false);
    void pollCatalog();
    void retryCatalog();
    void moveCatalog(u64 keys);
    void selectCatalogGame(const fog::Game& game);
    void loadComputers();
    void loadPlayStation();
    void pollPlayStation();
    void cancelPlayStation();
    void setCaptcha(const fog::AuthResult& response);
    void poll();
    void beginLogin();
    void enterCode();
    void moveGrid(u64 keys, std::size_t count, std::size_t columns = 4);
    void searchGames();
    std::string latencyText(const fog::Computer& computer) const;
    std::size_t selectedGenreIndex() const;
    std::size_t selectedGenreChip() const;
    void chooseGenre(int id);
    void inputGenres(u64 keys);
    void inputGenreBar(u64 keys);
    void input(u64 keys);
    SDL_Texture* cover(const fog::Game& game);
    void draw();
    Runtime& rt_;
    std::unique_ptr<Services> services_;
    fog::AuthRepository& auth_;
    fog::AccountRepository& account_;
    fog::CatalogRepository& catalogRepository_;
    fog::RentalRepository& rentals_;
    Services* pairedServices_ = nullptr;
    std::unique_ptr<fog::PairedCheckoutGuard> paired_;
    bool pairedPending_ = false;
    bool pairedVideoReconnectPending_ = false;
    bool pairedTransportReconnectPending_ = false;
    fog::rating::Presentation rating_;
    bool ratingFocusLost_ = false;
    fog::ProviderId loginProvider_ = fog::ProviderId::Mts;
    std::optional<fog::UserProfile> mtsProfile_, foghubProfile_;
    fog::Catalog catalog_;
    fog::LatencyService latency_;
    int genreId_ = 0;
    bool genreBarFocused_ = false;
    std::size_t genreChip_ = 5, genreDialogSelection_ = 0;
    fog::HomeCatalogNavigation homeCatalog_;
    fog::RecentGames recentGames_{storagePaths::path(storagePaths::RecentGames)};
    fog::RecentLaunch recentLaunch_;
    std::future<bool> recentSave_;
    bool recentSavePending_ = false;
    void pollRecentSave() noexcept;
    std::optional<fog::RecentGame> selectedRecentGame_;
    std::shared_ptr<fog::CatalogRepository> catalogApi_;
    std::future<fog::Page<fog::Game>> catalogPending_;
    fog::Catalog::Request catalogRequest_{0, 1, {}};
    fog::SessionGuard guard_;
    fog::CheckoutGuard checkout_;
    fog::ui::Ui ui_;
    fog::ArtworkCache artwork_;
    fog::SteamArtworkResolver steamArtwork_;
    std::vector<fog::HistoryEntry> history_;
    fog::Game detail_;
    fog::AutomaticComputers autoComputers_;
    bool detailsFromAutomatic_ = false;
    bool selectedPlayStation_ = false, streamPlayStation_ = false;
    fog::EntityId selectedComputerId_;
    platform::PsSensors psSensors_;
    platform::Rumble rumble_;
    std::uint64_t rumbleLogAtMs_ = 0;
    fog::playstation::TouchTrace psTouchTrace_;
    fog::playstation::NativeInput psNativeInput_;
    bool psTouchLastSubmitted_ = false;
    float psGyroPeak_ = 0, psAccelPeak_ = 0;
    std::uint64_t psMotionMovedFrames_ = 0;
    std::uint64_t psLogAtMs_ = 0, psGamepadAccepted_ = 0, psGamepadRejected_ = 0;
    std::vector<fog::Computer> playStationComputers_;
    std::future<std::vector<fog::Computer>> playStationPending_;
    std::shared_ptr<fog::CatalogRepository> playStationRepository_;
    std::shared_ptr<fog::CatalogRepository> featuredRepository_;
    std::shared_ptr<fog::PlayStationCatalogProgressChannel> playStationUpdates_;
    std::string playStationError_;
    std::chrono::steady_clock::time_point playStationLoadedAt_{};
    std::size_t automaticSelection_ = 1;
    bool ownSteam_ = false, sidebar_ = false, checkoutAutoPoll_ = false, stopAfterTask_ = false;
    bool authSaveFailed_ = false;
    int nav_ = 1;
    Screen loginReturn_ = Screen::Games;
    std::chrono::steady_clock::time_point checkoutAt_{};
    std::unique_ptr<stream::StreamingClient> stream_;
    fog::stream::Overlay overlay_;
    fog::video::Settings videoSettings_;
    fog::connection::Settings connectionSettings_;
    fog::connection::RelaySelection relaySelection_;
    fog::controller::Settings controllerSettings_;
    std::string controllerSaveStatus_;
    fog::rumble::Settings rumbleSettings_;
    std::string rumbleSaveStatus_;
    bool firstFramePresented_ = false;
    std::string videoSaveStatus_ = uiText::Settings::AutoSaveNotice;
    std::string connectionSaveStatus_ = uiText::Settings::ConnectionAutoSaveNotice;
    fog::stream::StreamInputGate streamInputGate_;
    // Main-thread diagnostics. Only the menu chord is observed; remote input/text is excluded.
    std::uint64_t menuPolls_ = 0, menuHandlers_ = 0, menuTransitions_ = 0;
    std::uint64_t menuPaints_ = 0, menuPresents_ = 0, menuPaintedTransition_ = 0, menuPresentedTransition_ = 0;
    std::uint64_t menuSuppressedTransition_ = 0;
    u64 menuHeld_ = 0;
    bool menuObservationActive_ = false, menuFocused_ = false;
    std::chrono::steady_clock::time_point menuHeartbeatAt_{}, menuTransitionAt_{};
    bool touchWasDown_ = false, touchDrag_ = false;
    fog::stream::MouseButtons mouseButtons_;
    float pointerX_ = 640.0f, pointerY_ = 360.0f;
    std::chrono::steady_clock::time_point mouseAt_{}, lastPointerSent_{}, overlayNoticeAt_{}, overlayHintAt_{};
    std::string overlayNotice_;
    std::string lastStreamState_, lastStreamDiagnostics_;
    std::unique_ptr<fog::Media> media_;
    fog::VideoRecovery videoRecovery_;
    fog::stream::TransportReconnect transportReconnect_;
    // Private rental identity, retained only until final close; never logged.
    fog::RentalId streamRental_;
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
    std::vector<fog::Rental> sessions_;
    bool sessionPending_ = false, exitLocked_ = false, exitAfterStop_ = false, shutdownDone_ = false;
    bool stopStartedOnExit_ = false;
    std::chrono::steady_clock::time_point streamStarted_{}, lastVideo_{}, lastInput_{}, retryAt_{}, lastMediaLog_{};
    std::future<TaskResult> pending_;
    Task task_ = Task::RestoreAuth;
    Screen screen_ = Screen::Games;
    std::vector<fog::Computer> computers_;
    std::size_t selection_ = 0;
    int computerPage_ = 1;
    fog::EntityId selectedGameId_;
    std::string search_, selectedGameName_, status_;
    std::string checkoutStatus_ = uiText::Server::WaitingReady;
    fog::AuthStep authStep_ = fog::AuthStep::Unknown;
    SDL_Texture* captcha_ = nullptr;
    int captchaWidth_ = 1, captchaHeight_ = 1;
};
}
