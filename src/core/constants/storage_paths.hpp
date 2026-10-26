#pragma once
#include <string>

namespace fog::constants::storage {
inline constexpr char SdRoot[] = "sdmc:/switch";
inline constexpr char Directory[] = "sdmc:/switch/fogplay_nx";
inline constexpr char Auth[] = "/auth.json";
inline constexpr char PendingSession[] = "/pending-session.json";
inline constexpr char PendingOrder[] = "/pending-order.json";
inline constexpr char VideoSettings[] = "/video-settings.json";
inline constexpr char ConnectionSettings[] = "/connection-settings.json";
inline constexpr char ControllerSettings[] = "/controller-settings.json";
inline constexpr char RecentGames[] = "/recent-games.json";
inline constexpr char DeviceId[] = "/device-id.txt";
inline constexpr char StorageProbe[] = "/storage-probe.txt";
inline constexpr char ArtworkCache[] = "/cache/artwork";
inline constexpr char SteamCache[] = "/cache/steam";
inline constexpr char SteamMappings[] = "/mappings.json";

inline std::string path(const char* suffix) { return std::string(Directory) + suffix; }
}
