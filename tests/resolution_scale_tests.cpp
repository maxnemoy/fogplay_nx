#include "features/streaming/domain/resolution_scale.hpp"
#include <cassert>
#include <iostream>

int main() {
    using Scale = stream::ResolutionScale;
    Scale scale;
    assert(!scale.pending());
    assert(!scale.observe({}) && !scale.observe({{1920, 0}, {1280, 720}}));
    assert(!scale.observe({{1920, 1080}, {0, 0}, {1280, 720}}));
    assert(!scale.observe(Scale::Table(Scale::MaxEntries + 1, Scale::Size{1280, 720})));
    const Scale::Table first{{1920, 1080}, {1600, 900}, {1280, 720}};
    assert(scale.observe(first) && scale.pending() == 2);
    scale.submitted(true);
    assert(scale.attempts() == 1 && scale.accepted() == 1 && !scale.pending());
    assert(!scale.observe(first) && !scale.pending());
    assert(scale.observe({{1920, 1080}, {1024, 576}}) && !scale.pending());
    assert(scale.observe(first) && !scale.pending()); // No retry when an old table returns.
    assert(scale.observe({{0, 0}, {1280, 720}}) && scale.pending() == 1);
    scale.submitted(false);
    assert(scale.attempts() == 2 && scale.rejected() == 1 && !scale.pending());
    assert(scale.observe({{1280, 720}, {1920, 1080}}) && scale.pending() == 0);
    scale.submitted(true);
    assert(scale.attempts() == 3 && scale.accepted() == 2 && !scale.pending());
    assert(scale.observe({{2560, 1440}, {1280, 720}}) && !scale.pending());
    Scale connection;
    assert(connection.observe(first) && connection.pending() == 2); // New connection gets a fresh budget.
    std::cout << "Announced resolution index, missing/malformed targets, repeated tables and bounded queue rejection passed\n";
}
