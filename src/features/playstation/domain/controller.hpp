#pragma once
#include <array>

namespace fog::playstation {
inline void applyExtraButtons(std::array<int,18>& buttons, bool touching) {
    const bool system = buttons[4] && buttons[5] && buttons[8];
    buttons[16] = system ? 1 : 0;
    buttons[17] = touching ? 1 : 0;
    if (system) buttons[4] = buttons[5] = buttons[8] = 0;
}
}
