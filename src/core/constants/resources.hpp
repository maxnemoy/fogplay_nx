#pragma once

namespace fog::constants::resources {
inline constexpr char CertificateBundle[] = "romfs:/cacert.pem";
}

namespace fog::constants::ui::Static {
#ifdef __SWITCH__
inline constexpr char Directory[] = "romfs:/ui/";
#else
inline constexpr char Directory[] = "romfs/ui/";
#endif
inline constexpr char Logo[] = "logo.png";
inline constexpr char Search[] = "magnifying-glass.svg";
inline constexpr char Games[] = "game-controller.svg";
inline constexpr char PlayStation[] = "playstation.svg";
inline constexpr char History[] = "writing.svg";
inline constexpr char Settings[] = "setting.svg";
}
