#include "features/playstation/domain/controller.hpp"
#include <cassert>
#include <iostream>
int main() {
    std::array<int,18> buttons{};
    fog::playstation::applyExtraButtons(buttons,false);
    for (int b : buttons) assert(b==0);
    buttons[0]=1; buttons[4]=1; buttons[8]=1;
    fog::playstation::applyExtraButtons(buttons,true);
    assert(buttons[0]==1 && buttons[4]==1 && buttons[8]==1 && buttons[16]==0 && buttons[17]==1);
    buttons[5]=1;
    fog::playstation::applyExtraButtons(buttons,false);
    assert(buttons[0]==1 && buttons[4]==0 && buttons[5]==0 && buttons[8]==0 && buttons[16]==1 && buttons[17]==0);
    buttons.fill(0);
    fog::playstation::applyExtraButtons(buttons,false);
    assert(buttons[16]==0 && buttons[17]==0);
    std::cout << "PlayStation system/touchpad controls: PASS\n";
}
