#include "core/constants/ui_text.hpp"
#include "core/constants/error_text.hpp"
#include "core/constants/app_info.hpp"
#if defined(__SWITCH__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform/switch/switch_runtime.hpp"
#include "app/app_diagnostics.hpp"
#include "app/presentation/startup_screen.hpp"
#include "core/storage/storage.hpp"
#include "features/streaming/infrastructure/stream.hpp"
#include <SDL_image.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>

namespace fogplay::platform {
using diagnostics::diagnostic;
using diagnostics::safeText;
namespace { constexpr SDL_Color white{234, 238, 245, 255}; }

void checkResult(Result rc, const char* operation) {
    if (R_FAILED(rc)) {
        char message[128];
        std::snprintf(message, sizeof(message), "%s: 0x%08x", operation, rc);
        throw std::runtime_error(message);
    }
}

void checkStorage() {
    const std::string path = fog::constants::storage::path(fog::constants::storage::StorageProbe);
    try {
        fog::replaceStorage(path, "first");
        fog::replaceStorage(path, "updated");
        std::ifstream input(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (input.bad() || content != "updated") throw std::runtime_error(fog::constants::error::Internal::StoragePreflightReadbackFailed);
        input.close();
        fog::clearStorage(path);
    } catch (...) {
        try { fog::clearStorage(path); } catch (...) {}
        throw;
    }
    diagnostic("SD preflight OK: create, replace, readback and remove. No rental requested.");
}

std::string deviceId() {
    for (const char* path : {fog::constants::storage::SdRoot, dataDir}) {
        if (mkdir(path, 0777) != 0 && errno != EEXIST) throw std::runtime_error(fog::constants::error::Internal::CannotCreateApplicationFolder);
    }
    const std::string path = fog::constants::storage::path(fog::constants::storage::DeviceId);
    if (auto* file = std::fopen(path.c_str(), "r")) {
        char value[128]{};
        const auto* read = std::fgets(value, sizeof(value), file);
        std::fclose(file);
        const std::string id = read ? std::string(value).substr(0, std::strcspn(value, "\r\n")) : "";
        if (id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return id;
        throw std::runtime_error(fog::constants::error::Internal::InvalidDeviceIdFile);
    }
    if (errno != ENOENT) throw std::runtime_error(fog::constants::error::Internal::CannotReadDeviceIdFile);
    std::array<unsigned char, 16> bytes{};
    randomGet(bytes.data(), bytes.size());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 15) | 64);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 63) | 128);
    const char* hex = "0123456789abcdef";
    std::string id;
    for (const auto byte : bytes) { id += hex[byte >> 4]; id += hex[byte & 15]; }
    auto* out = std::fopen((path + ".tmp").c_str(), "w");
    if (!out) throw std::runtime_error(fog::constants::error::Internal::CannotWriteDeviceId);
    const bool written = std::fprintf(out, "%s\n", id.c_str()) > 0;
    const bool closed = std::fclose(out) == 0;
    if (!written || !closed || std::rename((path + ".tmp").c_str(), path.c_str()) != 0)
        throw std::runtime_error(fog::constants::error::Internal::CannotSaveDeviceId);
    return id;
}

void Runtime::initGraphics() {
    using diagnostics::StartupStage;
    StartupStage graphics("graphics");
    { StartupStage stage("romfs"); checkResult(romfsInit(), "RomFS"); romfs = true; }
    {
        StartupStage stage("sdl_video");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) throw std::runtime_error(SDL_GetError());
        sdl = true;
    }
    {
        StartupStage stage("window");
        window = SDL_CreateWindow(fog::constants::common::Name, 0, 0, 1280, 720, SDL_WINDOW_SHOWN);
        if (!window) throw std::runtime_error(SDL_GetError());
    }
    {
        StartupStage stage("renderer");
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(renderer, 1280, 720);
        SDL_RendererInfo renderInfo{};
        if (SDL_GetRendererInfo(renderer,&renderInfo) == 0) {
            bool iyuv = false, nv12 = false;
            for (Uint32 i=0; i<renderInfo.num_texture_formats; ++i) {
                iyuv |= renderInfo.texture_formats[i] == SDL_PIXELFORMAT_IYUV;
                nv12 |= renderInfo.texture_formats[i] == SDL_PIXELFORMAT_NV12;
            }
            diagnostic("Renderer name=" + std::string(renderInfo.name ? renderInfo.name : "unknown") +
                " flags=" + std::to_string(renderInfo.flags) + " nativeIYUV=" + std::to_string(iyuv) +
                " nativeNV12=" + std::to_string(nv12));
        }
    }
    {
        StartupStage stage("system_fonts");
        checkResult(plInitialize(PlServiceType_User), "Shared font service"); pl = true;
        if (TTF_Init() != 0) throw std::runtime_error(TTF_GetError());
        ttf = true;
        PlFontData data{};
        checkResult(plGetSharedFontByType(&data, PlSharedFontType_Standard), "System font");
        font = TTF_OpenFontRW(SDL_RWFromConstMem(data.address, static_cast<int>(data.size)), 1, 23);
        titleFont = TTF_OpenFontRW(SDL_RWFromConstMem(data.address, static_cast<int>(data.size)), 1, 34);
        if (!font || !titleFont) throw std::runtime_error(TTF_GetError());
    }
    { StartupStage stage("first_splash"); showStartup(fog::constants::ui::Startup::StartingApplication); }
    diagnostic("Startup first_splash_presented");
}
void Runtime::showStartup(const char* message) {
    fog::ui::drawStartupScreen(renderer,font,titleFont,message);
}
void Runtime::initServices() {
    using diagnostics::StartupStage;
    StartupStage services("services");
    showStartup(fog::constants::ui::Startup::ConnectingServices);
    {
        StartupStage stage("sockets");
        SocketInitConfig config = *socketGetDefaultInitConfig();
        config.num_bsd_sessions = 12;
        config.sb_efficiency = 8;
        checkResult(socketInitialize(&config), "Sockets"); sockets = true;
    }
    { StartupStage stage("network_interface"); checkResult(nifmInitialize(NifmServiceType_User), "Network interface service"); nifm = true; }
    { StartupStage stage("sdl_audio"); if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) throw std::runtime_error(SDL_GetError()); }
    {
        StartupStage stage("image_codecs"); image = true;
        if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) throw std::runtime_error(IMG_GetError());
    }
}

Runtime::~Runtime() {
    diagnostic("RTC runtime cleanup begin; all application clients released.");
    for (;;) {
        try { stream::Client::cleanupRuntime(); break; }
        catch (...) {
            diagnostic("RTC cleanup unconfirmed; retaining platform services and retrying.");
            // Never unload the NRO while a cleanup thread may still execute it.
            appletMainLoop();
            if (renderer && font && appletGetFocusState() == AppletFocusState_InFocus) {
                SDL_SetRenderDrawColor(renderer, 12, 16, 28, 255);
                SDL_RenderClear(renderer);
                auto* surface = TTF_RenderUTF8_Blended(font, fog::constants::ui::Startup::WaitingForCleanup, white);
                if (surface) {
                    auto* texture = SDL_CreateTextureFromSurface(renderer, surface);
                    SDL_Rect rect{32, 300, surface->w, surface->h};
                    if (texture) { SDL_RenderCopy(renderer, texture, nullptr, &rect); SDL_DestroyTexture(texture); }
                    SDL_FreeSurface(surface);
                }
                SDL_RenderPresent(renderer);
            }
            svcSleepThread(1000000000);
        }
    }
    diagnostic("RTC runtime cleanup complete; threads joined.");
    if (titleFont) TTF_CloseFont(titleFont);
    if (font) TTF_CloseFont(font);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    if (image) IMG_Quit();
    if (ttf) TTF_Quit();
    if (sdl) SDL_Quit();
    if (pl) plExit();
    if (nifm) nifmExit();
    if (sockets) { diagnostic("socketExit begin"); socketExit(); diagnostic("socketExit complete"); }
    if (romfs) romfsExit();
}

std::optional<std::string> keyboard(const char* title, const std::string& initial, int maximum, bool password) {
    diagnostic(std::string("Keyboard open requested; password=") + (password ? "1" : "0"));
    // App controls can map physical B to Confirm. The system applet uses the
    // native layout, where B cancels; do not carry the activating press into it.
    PadState launchPad;
    padInitializeDefault(&launchPad);
    const auto releasedBy = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    constexpr u64 activationButtons = HidNpadButton_A | HidNpadButton_B | HidNpadButton_X |
        HidNpadButton_Y | HidNpadButton_Plus | HidNpadButton_Minus;
    for (;;) {
        padUpdate(&launchPad);
        if (!(padGetButtons(&launchPad) & activationButtons)) break;
        if (!appletMainLoop() || appletGetFocusState() != AppletFocusState_InFocus ||
            std::chrono::steady_clock::now() >= releasedBy) {
            diagnostic("Keyboard activation cancelled before applet launch");
            return std::nullopt;
        }
        svcSleepThread(10000000);
    }
    SwkbdConfig config{};
    const Result created = swkbdCreate(&config, 0);
    diagnostic("Keyboard create result=" + std::to_string(created));
    checkResult(created, "Software keyboard creation");
    if (password) swkbdConfigMakePresetPassword(&config);
    else swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, title);
    swkbdConfigSetInitialText(&config, initial.c_str());
    swkbdConfigSetStringLenMax(&config, static_cast<u32>(maximum));
    swkbdConfigSetReturnButtonFlag(&config, false);
    char result[512]{};
    const Result rc = swkbdShow(&config, result, sizeof(result));
    swkbdClose(&config);
    diagnostic("Keyboard show result=" + std::to_string(rc));
    // libnx uses this for a non-confirmed applet close as well as abnormal exit.
    // Retain its numeric result for diagnosis; it is not proof of a user cancel.
    if (rc == MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit)) return std::nullopt;
    checkResult(rc, "Software keyboard display");
    const std::string text(result);
    if (password) for (volatile char& value : result) value = 0;
    return text;
}

void fatalScreen(const std::string& message) {
    diagnostic("Fatal: " + message);
    consoleInit(nullptr);
    std::printf(fog::constants::ui::Startup::FailureFormat, fog::constants::common::Name, safeText(message).c_str());
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}
}
