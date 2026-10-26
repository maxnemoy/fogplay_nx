#pragma once
#include "features/session/domain/paired_rental.hpp"
#include "features/streaming/domain/streaming_client.hpp"
#include <memory>
#include <string>

namespace moonlight {
// Own one authenticated host across checkout and streaming. Create on the
// application thread; pairing/connection/network work runs on owned workers.
// The instance must outlive its PairingClient/StreamingClientFactory references.
class MoonlightSession final : public fog::PairingClient,
                               public fog::PairingPinGenerator,
                               public stream::StreamingClientFactory {
public:
    explicit MoonlightSession(std::string identityDirectory =
        "sdmc:/switch/fogplay_nx/moonlight-identities");
    ~MoonlightSession() override;
    MoonlightSession(const MoonlightSession&) = delete;
    MoonlightSession& operator=(const MoonlightSession&) = delete;
    bool begin(const fog::PairedEndpoint&, const std::string& pin) override;
    bool waitReady() override;
    bool submissionReady() const noexcept override;
    std::string diagnostics() const override;
    bool complete() override;
    // Joins pairing only. Successfully authenticated host state remains usable.
    void cancelAndJoin() noexcept override;
    std::string generate() override;
    bool bindRental(const fog::EntityId& rental);
    std::unique_ptr<stream::StreamingClient> create() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
