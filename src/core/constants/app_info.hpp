#pragma once

// Read by Makefile and package.py as well as the application.
#define FOGPLAY_APP_VERSION "0.6.21"
#define FOGPLAY_APP_NAME "FogPlay NX"
#define FOGPLAY_APP_AUTHOR "maxnemoy"

namespace fog::constants::common {
inline constexpr char Version[] = FOGPLAY_APP_VERSION;
inline constexpr char Name[] = FOGPLAY_APP_NAME;
inline constexpr char Author[] = FOGPLAY_APP_AUTHOR;
inline constexpr char Title[] = FOGPLAY_APP_NAME " " FOGPLAY_APP_VERSION;
inline constexpr char UserAgent[] = "FogPlayNX/" FOGPLAY_APP_VERSION;
}
