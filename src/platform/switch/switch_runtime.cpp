#if defined(__SWITCH__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform/switch/switch_runtime.hpp"
#include "app/app_diagnostics.hpp"
#include "core/storage/storage.hpp"
#include "features/streaming/infrastructure/stream.hpp"
#include <SDL_image.h>
#include <algorithm>
#include <array>
#include <cerrno>
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
    const std::string path = std::string(dataDir) + "/storage-probe.txt";
    try {
        fog::replaceStorage(path, "first");
        fog::replaceStorage(path, "updated");
        std::ifstream input(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (input.bad() || content != "updated") throw std::runtime_error("SD storage preflight readback failed");
        input.close();
        fog::clearStorage(path);
    } catch (...) {
        try { fog::clearStorage(path); } catch (...) {}
        throw;
    }
    diagnostic("SD preflight OK: create, replace, readback and remove. No rental requested.");
}

std::string deviceId() {
    for (const char* path : {"sdmc:/switch", dataDir}) {
        if (mkdir(path, 0777) != 0 && errno != EEXIST) throw std::runtime_error("Cannot create application folder on SD card.");
    }
    const std::string path = std::string(dataDir) + "/device-id.txt";
    if (auto* file = std::fopen(path.c_str(), "r")) {
        char value[128]{};
        const auto* read = std::fgets(value, sizeof(value), file);
        std::fclose(file);
        const std::string id = read ? std::string(value).substr(0, std::strcspn(value, "\r\n")) : "";
        if (id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return id;
        throw std::runtime_error("Invalid SD device-id.txt; rename it and restart.");
    }
    if (errno != ENOENT) throw std::runtime_error("Cannot read device-id.txt.");
    std::array<unsigned char, 16> bytes{};
    randomGet(bytes.data(), bytes.size());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 15) | 64);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 63) | 128);
    const char* hex = "0123456789abcdef";
    std::string id;
    for (const auto byte : bytes) { id += hex[byte >> 4]; id += hex[byte & 15]; }
    auto* out = std::fopen((path + ".tmp").c_str(), "w");
    if (!out) throw std::runtime_error("Cannot write device ID to SD card.");
    const bool written = std::fprintf(out, "%s\n", id.c_str()) > 0;
    const bool closed = std::fclose(out) == 0;
    if (!written || !closed || std::rename((path + ".tmp").c_str(), path.c_str()) != 0)
        throw std::runtime_error("Cannot save device ID to SD card.");
    return id;
}

void Runtime::init() {
    checkResult(romfsInit(), "RomFS"); romfs = true;
    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.num_bsd_sessions = 12;
    config.sb_efficiency = 8;
    checkResult(socketInitialize(&config), "Sockets"); sockets = true;
    checkResult(nifmInitialize(NifmServiceType_User), "Network interface service"); nifm = true;
    checkResult(plInitialize(PlServiceType_User), "Shared font service"); pl = true;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) throw std::runtime_error(SDL_GetError());
    sdl = true;
    if (TTF_Init() != 0) throw std::runtime_error(TTF_GetError());
    ttf = true;
    image = true;
    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) throw std::runtime_error(IMG_GetError());
    window = SDL_CreateWindow("FogPlay NX", 0, 0, 1280, 720, SDL_WINDOW_SHOWN);
    if (!window) throw std::runtime_error(SDL_GetError());
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
    PlFontData data{};
    checkResult(plGetSharedFontByType(&data, PlSharedFontType_Standard), "System font");
    font = TTF_OpenFontRW(SDL_RWFromConstMem(data.address, static_cast<int>(data.size)), 1, 23);
    titleFont = TTF_OpenFontRW(SDL_RWFromConstMem(data.address, static_cast<int>(data.size)), 1, 34);
    if (!font || !titleFont) throw std::runtime_error(TTF_GetError());
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
                auto* surface = TTF_RenderUTF8_Blended(font, "Waiting for network cleanup. Please wait.", white);
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
    SwkbdConfig config{};
    checkResult(swkbdCreate(&config, 0), "Software keyboard");
    if (password) swkbdConfigMakePresetPassword(&config);
    else swkbdConfigMakePresetDefault(&config);
    swkbdConfigSetHeaderText(&config, title);
    swkbdConfigSetInitialText(&config, initial.c_str());
    swkbdConfigSetStringLenMax(&config, static_cast<u32>(maximum));
    swkbdConfigSetReturnButtonFlag(&config, false);
    char result[512]{};
    const Result rc = swkbdShow(&config, result, sizeof(result));
    swkbdClose(&config);
    if (R_FAILED(rc)) return std::nullopt;
    return std::string(result);
}

void fatalScreen(const std::string& message) {
    diagnostic("Fatal: " + message);
    consoleInit(nullptr);
    std::printf("FogPlay NX startup failed\n\n%s\n\nUse full application mode (hold R when launching a game).\nCheck SD card and firmware date/time.\nPress + to exit.\n", safeText(message).c_str());
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
