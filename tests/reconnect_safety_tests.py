"""Static ownership/bridge checks; runtime recovery/ordering lives in real Player suites."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
player=(root/'src/features/streaming/application/player.cpp').read_text()
header=(root/'src/features/streaming/application/player.hpp').read_text()
app_header=(root/'src/app/application_controller.hpp').read_text()
app=(root/'src/app/application.cpp').read_text()
playback=(root/'src/features/streaming/presentation/playback_controller.cpp').read_text()
rental=(root/'src/features/session/presentation/rental_controller.cpp').read_text()
def method(source,signature):
    begin=source.index(signature);brace=source.index('{',begin);depth,end=1,brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[begin:end]
for forbidden in ('RentalSessionCoordinator','RentalRepository','Services','SessionJournal','CheckoutGuard','PairingClient'):
    assert forbidden not in player and forbidden not in header,forbidden
for field in ('retryStream_','transportTeardown_','transportReconnect_','videoRecovery_','streamRental_','lastVideoGeneration_','firstFramePresented_'):
    assert field not in app_header,field
assert 'TransportTeardown<::stream::StreamingClient> teardown;' in player
assert 'std::unique_ptr<::stream::StreamingClient> client,retained;' in player
transport=method(player,'    bool startTransport(')
close=method(player,'    bool closeTransport(')
advance=method(player,'    bool advance(')
assert transport.index('teardown.pending()')<transport.index('decoder=medias.create()')
assert 'client->start({start.rental,resumeOnly})' in transport
assert close.index('decoder->stopWorkers()')<close.index('input.beforeTransportClose(')<close.index('client->close()')
assert close.index('if (!final && !retryAllowed) return false;')<close.index('retained=std::move(client)')
assert close.index('teardown.begin(client,!final)')<close.index('client->close()')
assert advance.index('teardown.pending()')<advance.index('startTransport(true)')
for signature in ('void Player::observe(','void Player::pollTeardown(','void Player::resumeVerdict('):
    assert 'startTransport(' not in method(player,signature)
assert 'client=std::move(result.client)' in method(player,'    void observeTeardown(')
assert 'stop(StopReason::RetryTerminal)' in method(player,'    void observeTeardown(')
pump=method(playback,'void ApplicationController::pumpStream(')
assert pump.index('player_.observe(')<pump.index('handleStreamOverlay(')<pump.index('player_.beforeMedia(')<pump.index('sendStreamInput(')<pump.index('player_.media(')
for removed_algorithm in ('PlaybackDrainBudget','preparePresentation(','resetVideoDecoder(','startTransport('):
    assert removed_algorithm not in pump,removed_algorithm
finish=method(rental,'void ApplicationController::beginFinish(')
assert finish.index('rentalSession_.request();')<finish.index('player_.cancel();')<finish.index('startRental(')
dispatch=method(rental,'bool ApplicationController::startRental(')
assert dispatch.index('rentalSession_.plan(')<dispatch.index('start(Task::RentalOperation')<dispatch.index('closeStream();')
for target in ('SessionStop','CheckoutStop','PairedStop'):assert 'Rental::Operation::'+target in dispatch
shutdown=method(rental,'void ApplicationController::shutdownSession(')
assert shutdown.index('services_->cancel();')<shutdown.index('closeStream();')
assert shutdown.index('disarmExitHandling();')<shutdown.index('closeStream();')
assert 'beginFinish(' not in shutdown and 'while (' not in shutdown
joined=method(playback,'bool ApplicationController::completePlayerInspection(')
assert 'player_.resumeVerdict(' in joined and 'startTransport(' not in joined
coordinator=(root/'src/features/session/application/rental_session_coordinator.cpp').read_text()
assert 'm.paired->inspectForResume()' in coordinator and 'case Operation::PairedPoll: case Operation::QualityInspection:' in coordinator
assert 'beginFinish("focus-lost")' in app and 'beginFinish("application-exit")' not in app
ui=(root/'src/core/constants/ui_text.hpp').read_text()
for notice in ('PaidRentalNotice','AdoptRentalNotice'):
    line=next(line for line in ui.splitlines() if f'char {notice}[]' in line)
    assert 'Закрытие из HOME может оставить аренду активной' in line and 'проверьте сайт сервиса' in line
print('Player ownership, store-only completion, post-overlay recovery, same-rental start, single reaper, stop/shutdown dispatch boundaries PASS (static; actual runtime matrix in player.sh)')