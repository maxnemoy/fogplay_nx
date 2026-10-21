#include "features/streaming/domain/media_annexb.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using Bytes = std::vector<unsigned char>;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void ue(std::vector<bool>& bits, unsigned value) {
    const auto coded = value + 1;
    unsigned width = 0;
    for (unsigned remaining = coded; remaining; remaining >>= 1) ++width;
    for (unsigned i = 1; i < width; ++i) bits.push_back(false);
    for (unsigned i = width; i; --i) bits.push_back((coded >> (i - 1)) & 1);
}
static Bytes nal(unsigned type, unsigned id = 0, unsigned parent = 0, unsigned version = 31) {
    std::vector<bool> bits;
    if (type == 7) {
        for (unsigned value : {66U, 0U, version})
            for (unsigned i = 8; i; --i) bits.push_back((value >> (i - 1)) & 1);
    }
    if (type == 5) { ue(bits, 0); ue(bits, 2); }
    ue(bits, id);
    if (type == 8) ue(bits, parent);
    bits.push_back(true);
    while (bits.size() % 8) bits.push_back(false);
    Bytes output{0, 0, 0, 1, static_cast<unsigned char>(0x60 | type)};
    unsigned zeros = 0;
    for (std::size_t i = 0; i < bits.size(); i += 8) {
        unsigned value = 0;
        for (unsigned j = 0; j < 8; ++j) value = (value << 1) | bits[i + j];
        if (zeros >= 2 && value <= 3) { output.push_back(3); zeros = 0; }
        output.push_back(value);
        zeros = value == 0 ? zeros + 1 : 0;
    }
    return output;
}
static Bytes join(const Bytes& a, const Bytes& b) {
    auto output = a;
    output.insert(output.end(), b.begin(), b.end());
    return output;
}
static void observe(fog::detail::VideoParameters& parameters, const Bytes& packet) {
    parameters.observe(fog::detail::inspectVideo(packet.data(), packet.size()));
}
static bool prefix(const fog::detail::VideoParameters& parameters, const Bytes& packet) {
    Bytes result;
    return parameters.recoveryPrefix(fog::detail::inspectVideo(packet.data(), packet.size()), result);
}
int main() {
    try {
        fog::detail::VideoParameters parameters;
        check(!prefix(parameters, nal(5)), "empty cache accepted IDR");
        observe(parameters, join(nal(8), nal(7)));
        check(prefix(parameters, nal(5)), "reversed parameter order rejected");
        check(!prefix(parameters, nal(5, 1)), "unknown PPS accepted");
        check(!prefix(parameters, nal(1)), "delta frame accepted");
        observe(parameters, nal(7, 0, 0, 40));
        check(!prefix(parameters, nal(5)), "changed SPS retained old PPS");
        observe(parameters, nal(8));
        check(prefix(parameters, nal(5)), "fresh PPS did not restore pair");
        observe(parameters, nal(8, 1, 1));
        check(!prefix(parameters, nal(5, 1)), "PPS without parent SPS accepted");
        observe(parameters, nal(7, 1));
        check(!prefix(parameters, nal(5, 1)), "new SPS reused preexisting PPS");
        observe(parameters, nal(8, 1, 1));
        check(prefix(parameters, join(nal(5), nal(5, 1))), "multiple slice parameter pairs failed");
        observe(parameters, join(nal(7, 31), nal(8, 255, 31)));
        check(prefix(parameters, nal(5, 255)), "maximum valid IDs rejected");
        observe(parameters, nal(7, 32));
        check(!prefix(parameters, nal(5)), "invalid SPS ID did not invalidate cache");
        observe(parameters, join(nal(7), nal(8, 256)));
        check(!prefix(parameters, nal(5)), "invalid PPS ID accepted");
        observe(parameters, join(nal(7), nal(8)));
        Bytes forbidden = nal(7);
        forbidden[4] |= 128;
        observe(parameters, forbidden);
        check(!prefix(parameters, nal(5)), "forbidden NAL header accepted");

        // Logical and allocated cache bounds remain independent of ID churn.
        for (unsigned id = 0; id < 32; ++id) {
            auto sps = nal(7, id);
            sps.resize(15 * 1024, 0x55);
            observe(parameters, join(sps, nal(8, id, id)));
        }
        check(!prefix(parameters, nal(5)), "cache overflow retained oldest SPS");
        check(prefix(parameters, nal(5, 31)), "cache overflow lost newest pair");
        Bytes tooMany;
        for (unsigned i = 0; i < 257; ++i) tooMany = join(tooMany, nal(5, 31));
        check(!fog::detail::inspectVideo(tooMany.data(), tooMany.size()).complete, "NAL count unbounded");
        observe(parameters, tooMany);
        check(!prefix(parameters, nal(5, 31)), "incomplete scan retained cache");

        auto threeByte = nal(7);
        threeByte.erase(threeByte.begin());
        threeByte.insert(threeByte.end(), {0, 0, 0});
        const auto scanned = fog::detail::inspectVideo(threeByte.data(), threeByte.size());
        check(scanned.complete && scanned.nals.size() == 1 && scanned.nals[0].size == nal(7).size() - 4,
            "Annex B start code or trailing zeros wrong");
        const Bytes missingStart{0x65, 0xB8};
        check(!fog::detail::inspectVideo(missingStart.data(), missingStart.size()).complete, "non-Annex B accepted");
        const Bytes noHeader{0, 0, 1};
        check(!prefix(parameters, noHeader), "empty NAL accepted");
        unsigned seed = 1234567;
        for (unsigned iteration = 0; iteration < 4000; ++iteration) {
            Bytes packet(iteration % 256);
            for (auto& value : packet) { seed = seed * 1664525U + 1013904223U; value = seed >> 24; }
            observe(parameters, packet);
            (void)prefix(parameters, packet);
        }
        std::cout << "Annex B tests passed: IDs, changed SPS/PPS, malformed/oversized counts, cache eviction, 4000 bounded fuzz packets\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
