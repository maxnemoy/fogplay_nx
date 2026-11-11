#include "player_fakes.hpp"
#include <iostream>
using namespace player_test;
static unsigned preparedEvents(Fixture& f) {
    unsigned prepared=0;while(const auto event=f.owner.takeEvent())if(event->kind==Player::EventKind::Prepared)++prepared;return prepared;
}
static void start(Fixture& f) { f.owner.start(f.request(fog::ProviderId::FogHub),{});f.client()->decoderWaiting=true; }
static void decoded(Fixture& f) { f.media()->pending=true;f.media()->progress.decoded=1;f.media()->progress.waiting=false; }
static void resumed(Fixture& f) {
    assert(!f.client()->decoderWaiting&&!f.owner.snapshot().lossWaiting&&!f.owner.snapshot().videoRecovering&&f.owner.snapshot().firstPrepared);
    assert(f.client()->decoderClears==1&&preparedEvents(f)==1&&!f.input.waiting&&f.owner.snapshot().inputAllowed);
}
int main() {
    Fixture startup;start(startup);decoded(startup);startup.turn();resumed(startup);
    Fixture recovery;start(recovery);recovery.media()->prepareFailures=1;recovery.turn();decoded(recovery);recovery.turn();resumed(recovery);
    Fixture generation;start(generation);generation.turn();
    assert(generation.client()->decoderWaiting&&generation.client()->decoderClears==0&&preparedEvents(generation)==0&&!generation.owner.snapshot().firstPrepared);
    decoded(generation);generation.turn();resumed(generation);
    Fixture pending;start(pending);pending.media()->prepareFailures=1;pending.turn();pending.turn();
    assert(pending.owner.snapshot().videoRecovering&&pending.client()->decoderWaiting&&pending.client()->decoderClears==0&&pending.input.waiting);
    Fixture assembly;start(assembly);decoded(assembly);assembly.client()->lossWaiting=true;assembly.turn();
    assert(!assembly.client()->decoderWaiting&&assembly.client()->lossWaiting&&assembly.owner.snapshot().lossWaiting&&assembly.input.waiting);
    Fixture disconnected;start(disconnected);decoded(disconnected);disconnected.client()->disconnected=true;disconnected.turn();
    assert(!disconnected.client()->decoderWaiting&&disconnected.owner.snapshot().recovering&&disconnected.input.waiting);
    Fixture expired;start(expired);expired.media()->prepareFailures=1;expired.turn();decoded(expired);expired.now+=11s;expired.turn();
    assert(expired.owner.snapshot().cancelled&&expired.client()->decoderWaiting&&expired.client()->decoderClears==0&&!expired.owner.snapshot().inputAllowed);
    Fixture failed;start(failed);failed.media()->prepareFailures=1;failed.turn();
    assert(failed.owner.snapshot().decodeErrors==1&&failed.owner.snapshot().videoRecovering&&failed.client()->decoderWaiting&&failed.client()->decoderClears==0&&failed.input.waiting);
    std::cout<<"Actual Player decoder waiting: all8 prior startup/recovery/retained-texture/loss/disconnect/expired/failed-prepare scenarios PASS\n";
}
