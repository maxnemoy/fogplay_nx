#include "features/streaming/infrastructure/incoming_control_diagnostics.hpp"
#include <cassert>
#include <iostream>

using Json = nlohmann::json;
using Diagnostics = stream::IncomingControlDiagnostics;

static void descriptions() {
    const auto unknown = Diagnostics::describe(Json{{"id", "new_event-v2"}, {"enabled", true},
        {"level", 73519}, {"nested", {{"mode", "private-mode"}, {"items", Json::array({1, 2, 3})}}}}, 123);
    assert(unknown.key.find("id=new_event-v2") != std::string::npos);
    assert(unknown.sample.find("enabled:bool") != std::string::npos);
    assert(unknown.sample.find("level:integer") != std::string::npos);
    assert(unknown.sample.find("nested:{items:[integer,integer,...],mode:string}") != std::string::npos);
    assert(unknown.sample.find("private-mode") == std::string::npos && unknown.sample.find("73519") == std::string::npos);
    Json secrets{{"id", "mystery"}, {"outer", {{"accessToken", "secret-token"},
        {"Cookie", "secret-cookie"}, {"password", "secret-password"},
        {"authorization", "secret-auth"}, {"url", "https://secret.example"},
        {"sdp", "secret-sdp"}, {"candidate", "secret-candidate"}, {"ip", "192.0.2.1"},
        {"address", "secret-address"}, {"host", "secret-host"}}}};
    const auto hidden = Diagnostics::describe(secrets, 456);
    assert(hidden.sample.find("secret") == std::string::npos && hidden.sample.find("192.0.2.1") == std::string::npos);
    assert(hidden.sample.find("accessToken:<redacted>") != std::string::npos);
    for (const auto* field : {"session", "hash", "email", "phone", "pin", "text", "clipboard", "body"}) {
        const auto nested = Diagnostics::describe(Json{{"id", "event"}, {"outer", {{field, {{"raw", "nested-secret"}, {"number", 123456}}}}}}, 1);
        assert(nested.sample.find(std::string(field) + ":<redacted>") != std::string::npos);
        assert(nested.sample.find("raw") == std::string::npos && nested.sample.find("nested-secret") == std::string::npos);
    }
    for (const auto& invalid : {std::string("bad\nid"), std::string("bad\rkey"), std::string("escape\x1b[31m"),
             std::string("with spaces"), std::string(65, 'a'), std::string("nonascii\xc3\xa9")}) {
        const auto description = Diagnostics::describe(Json{{"id", invalid}, {invalid, "secret"}}, 1);
        assert(description.key.find("id=<invalid>") != std::string::npos);
        assert(description.sample.find(invalid) == std::string::npos && description.sample.find("secret") == std::string::npos);
        assert(description.sample.find('\n') == std::string::npos && description.sample.find('\r') == std::string::npos);
    }
    const auto malformed = Diagnostics::describe(Json::parse("{malformed", nullptr, false), 65537);
    assert(malformed.key == "id=<missing> schema=invalid" && malformed.sample.find("bytes=65537") != std::string::npos);
    assert(Diagnostics::describe(Json{{"id", 5}}, 1).key.find("id=<invalid>") != std::string::npos);
    auto rumble = Json{{"id", "gamepad_notification"}, {"index", 0}, {"large_motor", 255}, {"small_motor", 128}};
    const auto first = Diagnostics::describe(rumble, 100);
    assert(first.sample.find("large_motor:255") != std::string::npos && first.sample.find("small_motor:128") != std::string::npos);
    rumble["large_motor"] = 0;
    const auto stop = Diagnostics::describe(rumble, 101);
    assert(first.key == stop.key && first.sample != stop.sample);
    rumble["large_motor"] = 65536; rumble["small_motor"] = -1; rumble["index"] = true;
    const auto badRumble = Diagnostics::describe(rumble, 102);
    assert(badRumble.sample.find("large_motor:integer") != std::string::npos);
    assert(badRumble.sample.find("small_motor:integer") != std::string::npos && badRumble.sample.find("index:bool") != std::string::npos);
    assert(badRumble.sample.find("65536") == std::string::npos);
    rumble["id"] = "unknown"; rumble["large_motor"] = 255;
    assert(Diagnostics::describe(rumble, 100).sample.find("large_motor:255") == std::string::npos);
    Json large = Json::object(); large["id"] = "large";
    for (unsigned n = 0; n < 1000; ++n) large["field_" + std::to_string(n) + std::string(48, 'x')] = std::string(1000, 's');
    const auto limited = Diagnostics::describe(large, 1000000);
    assert(limited.key.size() < 800 && limited.sample.size() < 800 && limited.sample.find("...") != std::string::npos);
    Json deep{{"id", "deep"}, {"one", {{"two", {{"three", {{"four", "deep-secret"}}}}}}}};
    const auto depth = Diagnostics::describe(deep, 100);
    assert(depth.sample.find("object{...}") != std::string::npos && depth.sample.find("four") == std::string::npos);
}

static void rateAndCoalescing() {
    Diagnostics incoming;
    std::string event;
    const auto description = Diagnostics::describe(Json{{"id", "event"}, {"value", 42}}, 100);
    assert(!incoming.poll(event, 0, true));
    for (unsigned n = 0; n < 1000; ++n) incoming.receive(description, 100 + n);
    assert(incoming.poll(event, 1100));
    assert(event.find("count=1000 pending=1000 firstMs=100 lastMs=1099") != std::string::npos);
    assert(!incoming.poll(event, 1100));
    incoming.receive(description, 1101); assert(incoming.poll(event, 1101));
    incoming.receive(description, 1102); assert(incoming.poll(event, 1102));
    incoming.receive(description, 1103); assert(!incoming.poll(event, 6101));
    assert(!incoming.poll(event, 0)); // A rewound clock cannot bypass throttling.
    assert(incoming.poll(event, 6102));
    assert(event.find("pending=1") != std::string::npos && !incoming.poll(event, 6103));
    incoming.receive(description, 6103); assert(!incoming.poll(event, 6103));
    assert(incoming.poll(event, 6103, true));
    assert(!incoming.poll(event, 6103, true));
    assert(incoming.diagnostics().find("incomingEvents=1004 ") != std::string::npos);
    assert(incoming.diagnostics().find("incomingSchemas=1 ") != std::string::npos);
    assert(incoming.diagnostics().find("incomingCoalesced=999 ") != std::string::npos);
    assert(incoming.diagnostics().find("event") == std::string::npos);
    Diagnostics pulses;
    for (unsigned n = 0; n < 1000; ++n)
        pulses.receive(Diagnostics::describe(Json{{"id", "gamepad_notification"}, {"index", 0},
            {"large_motor", n % 256}, {"small_motor", 0}}, n), n);
    assert(pulses.poll(event, 1000) && event.find("large_motor:231") != std::string::npos);
    assert(event.find("count=1000") != std::string::npos && !pulses.poll(event, 1001));
    assert(event.size() <= 1024);
}

static void schemaLimit() {
    Diagnostics incoming;
    std::string event;
    for (unsigned n = 0; n < 1000; ++n)
        incoming.receive(Diagnostics::describe(Json{{"id", "event_" + std::to_string(n)}, {"field", 1}}, 100), n);
    assert(incoming.diagnostics().find("incomingSchemas=32 ") != std::string::npos);
    assert(incoming.diagnostics().find("incomingSchemaOverflow=968") != std::string::npos);
    unsigned emitted = 0, overflow = 0;
    while (incoming.poll(event, 1000, true)) {
        ++emitted; overflow += event.find("schemaOverflow=968") != std::string::npos;
        assert(event.size() <= 1024 && emitted <= 33);
    }
    assert(emitted == 33 && overflow == 1);
    assert(!incoming.poll(event, 1001, true));
    for (unsigned n = 0; n < 4; ++n) {
        incoming.receive(Diagnostics::describe(Json{{"id", "overflow_new"}}, 1), 1001 + n);
        const auto available = incoming.poll(event, 1001 + n);
        assert(available == (n < 2)); // Forced initial summary already consumed one of its first three emissions.
    }
    assert(incoming.poll(event, 1005, true) && event.find("pending=2") != std::string::npos);
    assert(!incoming.poll(event, 1005, true));
}

int main() {
    descriptions(); rateAndCoalescing(); schemaLimit();
    std::cout << "Incoming control diagnostics: redacted schema discovery, labels, bounds, coalescing, rate limits, overflow and final flush PASS\n";
}
