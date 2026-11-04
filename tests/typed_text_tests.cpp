#include "features/streaming/domain/typed_text.hpp"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using fog::stream::TypedText;

namespace {
std::string label(const TypedText::Item& item) {
    return (item.kind == TypedText::Kind::Key ? "<" + item.value + ">" : item.value);
}
}

int main() {
    using Clock = TypedText::Clock;
    using Result = TypedText::Result;
    using std::chrono::milliseconds;
    const auto t0 = Clock::time_point{} + std::chrono::seconds(100);
    {
        // Segments leave in order, one per interval; empty segments are ignored.
        TypedText text;
        std::vector<std::string> sent;
        auto accept = [&](const TypedText::Item& item) { sent.push_back(label(item)); return true; };
        assert(text.pump(t0, accept) == Result::Idle && !text.active());
        text.appendText({"ab", "", "b", "c"}, t0);
        assert(text.remaining() == 3);
        assert(text.pump(t0, accept) == Result::Pending && sent == std::vector<std::string>{"ab"});
        assert(text.pump(t0 + milliseconds(119), accept) == Result::Pending && sent.size() == 1);
        assert(text.pump(t0 + milliseconds(120), accept) == Result::Pending && sent.size() == 2 && sent.back() == "b");
        assert(text.pump(t0 + milliseconds(240), accept) == Result::Completed && sent.back() == "c");
        assert(!text.active() && text.pump(t0 + std::chrono::seconds(1), accept) == Result::Idle);
    }
    {
        // A refused item is retried, never skipped, and fails only after the limit.
        TypedText text;
        bool open = false;
        int calls = 0;
        std::vector<std::string> sent;
        auto send = [&](const TypedText::Item& item) { ++calls; if (open) sent.push_back(label(item)); return open; };
        text.appendText({"x", "y"}, t0);
        assert(text.pump(t0, send) == Result::Pending && calls == 1);
        assert(text.pump(t0 + milliseconds(1999), send) == Result::Pending && calls == 2);
        open = true;
        assert(text.pump(t0 + milliseconds(2000), send) == Result::Pending && sent == std::vector<std::string>{"x"});
        open = false;
        assert(text.pump(t0 + milliseconds(2120), send) == Result::Pending);
        assert(text.pump(t0 + milliseconds(4119), send) == Result::Pending && text.active());
        assert(text.pump(t0 + milliseconds(4120), send) == Result::Failed && !text.active());
        assert(sent.size() == 1);
    }
    {
        // Clearing (transport closed) drops the remaining text without sending it.
        TypedText text;
        int calls = 0;
        auto count = [&](const TypedText::Item&) { ++calls; return true; };
        text.appendText({"a", "a"}, t0);
        assert(text.pump(t0, count) == Result::Pending && calls == 1);
        text.clear();
        assert(text.pump(t0 + std::chrono::seconds(1), count) == Result::Idle && calls == 1);
    }
    {
        // Overlay keys and a further text queue behind pending text; nothing
        // overtakes it (validation: "a<Tab>a" must become "aa" then <Tab>).
        TypedText text;
        std::vector<std::string> sent;
        auto accept = [&](const TypedText::Item& item) { sent.push_back(label(item)); return true; };
        text.appendText({"a", "a"}, t0);
        assert(text.pump(t0, accept) == Result::Pending);
        text.appendKey("Tab", t0 + milliseconds(10));
        text.appendText({"next"}, t0 + milliseconds(20));
        text.appendKey("Enter", t0 + milliseconds(30));
        auto now = t0;
        Result result = Result::Pending;
        while (result == Result::Pending) { now += milliseconds(120); result = text.pump(now, accept); }
        assert(result == Result::Completed);
        assert((sent == std::vector<std::string>{"a", "a", "<Tab>", "next", "<Enter>"}));
    }
    {
        // Cancel drops everything unsent (other input reached the PC) and reports it.
        TypedText text;
        std::vector<std::string> sent;
        auto accept = [&](const TypedText::Item& item) { sent.push_back(label(item)); return true; };
        text.appendText({"a", "a", "a"}, t0);
        text.appendKey("Enter", t0);
        assert(text.pump(t0, accept) == Result::Pending);
        assert(text.cancel() == 3 && !text.active());
        assert(text.pump(t0 + std::chrono::seconds(1), accept) == Result::Idle && sent.size() == 1);
        assert(text.cancel() == 0);
        // A key with nothing pending is due immediately (the owner sends such keys directly).
        text.appendKey("Tab", t0 + std::chrono::seconds(2));
        assert(text.pump(t0 + std::chrono::seconds(2), accept) == Result::Completed && sent.back() == "<Tab>");
    }
    std::cout << "Typed text pacing tests passed (interval, order, keys and text queued behind text, retry limit, failure, clear, cancel)\n";
}
