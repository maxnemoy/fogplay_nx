#include "features/streaming/infrastructure/moonlight/app_selection.hpp"
#include <cassert>
#include <iostream>

int main() {
    using moonlight::selectApplication;
    assert(!selectApplication({}));
    assert(selectApplication({{1, "Desktop"}, {2, "Playnite для геймпада"}, {3, "Steam"}}) == 1);
    assert(selectApplication({{1, "Steam"}, {2, "Playnite"}, {3, "Playnite для геймпада"}}) == 3);
    assert(selectApplication({{3, "Playnite для геймпада"}, {2, "Playnite"}, {1, "Steam"}}) == 3);
    assert(selectApplication({{1, "Steam"}, {2, " \tPLAYNITE Fullscreen \n"}}) == 2);
    assert(selectApplication({{1, "Steam Big Picture"}, {2, "Playnite"}}) == 2);
    assert(selectApplication({{1, "Steam"}, {2, "Unrelated game"}}) == 1);
    assert(selectApplication({{1, "Only custom launcher"}}) == 1);
    assert(!selectApplication({{1, "First"}, {2, "Second"}}));
    assert(!selectApplication({{1, "Desktop"}, {2, "Desktop"}}));
    assert(!selectApplication({{1, "Playnite"}, {2, "PLAYNITE"}, {3, "Steam"}}));
    assert(!selectApplication({{1, "Playnite gamepad"}, {2, "Playnite для геймпада"}}));
    assert(!selectApplication({{1, "Steam game title"}, {2, "Playnite helper"}}));
    assert(!selectApplication({{0, "Desktop"}, {-1, "Steam"}, {1, "  "}}));
    std::cout << "Moonlight Desktop/FH Playnite/Steam selection tests passed\n";
}
