#include "features/streaming/domain/resolution_scale.hpp"
#include <cassert>
#include <iostream>
#include <string_view>

int main() {
    using Scale = stream::ResolutionScale;
    using Size = Scale::Size;
    Scale scale;
    assert(!scale.pending());
    assert(std::string_view(scale.pendingReason()) == "no-target");
    assert(scale.pendingState() == Scale::PendingReason::NoTarget);
    assert(scale.tableRevision() == 0 && scale.tableEntries() == 0);
    assert((scale.nativeSize() == Size{0, 0}) && !scale.lastSubmittedPreset());
    assert(!scale.observe({}) && !scale.observe({{1920, 0}, {1280, 720}}));
    assert(!scale.observe({{1920, 1080}, {0, 0}, {1280, 720}}));
    assert(!scale.observe(Scale::Table(Scale::MaxEntries + 1, Scale::Size{1280, 720})));
    const Scale::Table first{{1920, 1080}, {1600, 900}, {1280, 720}};
    assert(scale.observe(first) && scale.pending() == 2);
    assert(std::string_view(scale.pendingReason()) == "ready");
    assert(scale.pendingState() == Scale::PendingReason::Ready);
    assert(scale.tableRevision() == 1 && scale.tableEntries() == 3);
    assert((scale.nativeSize() == Size{1920, 1080}));
    scale.submitted(true);
    assert(scale.attempts() == 1 && scale.accepted() == 1 && !scale.pending());
    assert(scale.lastSubmittedPreset() == 2);
    assert(std::string_view(scale.pendingReason()) == "duplicate-last");
    assert(scale.pendingState() == Scale::PendingReason::DuplicateLast);
    scale.submitted(false); // No pending request: counters and last submission stay intact.
    assert(scale.attempts() == 1 && scale.rejected() == 0);
    assert(!scale.observe(first) && !scale.pending());
    assert(scale.tableRevision() == 1);
    assert(scale.observe({{1920, 1080}, {1024, 576}}) && !scale.pending());
    assert(std::string_view(scale.pendingReason()) == "no-target");
    assert(!scale.observe({{0, 720}})); // A malformed update cannot erase the valid table.
    assert(scale.tableRevision() == 2 && scale.tableEntries() == 2 && !scale.preset());
    assert(scale.observe(first) && !scale.pending()); // No intervening submission: no retry.
    assert(scale.tableRevision() == 3 && scale.lastSubmittedPreset() == 2);
    assert(scale.observe({{0, 0}, {1280, 720}}) && scale.pending() == 1);
    assert((scale.nativeSize() == Size{0, 0}));
    scale.submitted(false);
    assert(scale.attempts() == 2 && scale.rejected() == 1 && !scale.pending());
    assert(scale.lastSubmittedPreset() == 1);
    assert(std::string_view(scale.pendingReason()) == "duplicate-last");
    assert(scale.observe({{1280, 720}, {1920, 1080}}) && scale.pending() == 0);
    scale.submitted(true);
    assert(scale.attempts() == 3 && scale.accepted() == 2 && !scale.pending());
    assert(scale.lastSubmittedPreset() == 0);
    assert(std::string_view(scale.pendingReason()) == "limit");
    assert(scale.pendingState() == Scale::PendingReason::Limit);
    assert(scale.observe({{2560, 1440}, {1280, 720}}) && !scale.pending());
    assert(scale.tableRevision() == 6 && scale.preset() == 1);
    scale.submitted(true);
    assert(scale.attempts() == Scale::MaxRequests && scale.lastSubmittedPreset() == 0);
    Scale transition;
    assert(transition.observe(first) && transition.pending() == 2);
    transition.submitted(true);
    assert(transition.observe({{1280, 720}, {1024, 576}}) && transition.pending() == 0);
    transition.submitted(true);
    assert(transition.observe(first) && transition.pending() == 2);
    transition.submitted(true);
    assert(transition.attempts() == Scale::MaxRequests && !transition.pending());
    assert(transition.accepted() == 3 && transition.rejected() == 0);
    assert(transition.tableRevision() == 3 && transition.lastSubmittedPreset() == 2);
    assert(transition.observe({{1280, 720}, {1024, 576}}) && !transition.pending());
    assert(transition.observe(first) && !transition.pending()); // Alternating tables cannot reset the budget.
    assert(std::string_view(transition.pendingReason()) == "limit");

    Scale rejection;
    assert(rejection.observe(first) && rejection.pending() == 2);
    rejection.submitted(false);
    assert(!rejection.pending() && !rejection.observe(first));
    assert(rejection.attempts() == 1 && rejection.rejected() == 1);
    assert(rejection.observe({{1280, 720}, {1024, 576}}) && rejection.pending() == 0);
    rejection.submitted(false);
    assert(rejection.observe(first) && rejection.pending() == 2);
    rejection.submitted(true);
    assert(rejection.attempts() == 3 && rejection.rejected() == 2 && rejection.accepted() == 1);

    Scale unsubmitted;
    assert(unsubmitted.observe(first) && unsubmitted.pending() == 2);
    unsubmitted.submitted(true);
    assert(unsubmitted.observe({{1280, 720}, {1024, 576}}) && unsubmitted.pending() == 0);
    assert(unsubmitted.observe(first) && !unsubmitted.pending());
    assert(unsubmitted.attempts() == 1 && unsubmitted.lastSubmittedPreset() == 2);

    Scale changedNative;
    assert(changedNative.observe(first) && changedNative.pending() == 2);
    changedNative.submitted(true);
    assert(changedNative.observe({{2560, 1440}, {1600, 900}, {1280, 720}}));
    assert(changedNative.pending() == 2); // Deduplicate the table, not only the preset index.
    assert((changedNative.nativeSize() == Size{2560, 1440}));

    Scale connection;
    assert(connection.observe(first) && connection.pending() == 2); // New connection gets a fresh budget.
    assert(connection.tableRevision() == 1 && connection.attempts() == 0);
    std::cout << "Resolution table transitions, last-submitted deduplication, diagnostics and bounded rejection passed\n";
}
