"""Static safety boundaries for Switch-only application glue (not runtime proof)."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
playback = (root / 'src/features/streaming/presentation/playback_controller.cpp').read_text()
app = (root / 'src/app/application.cpp').read_text()
inputs = (root / 'src/features/streaming/presentation/stream_input_controller.cpp').read_text()
rental = (root / 'src/features/session/presentation/rental_controller.cpp').read_text()
recovery = (root / 'src/features/streaming/presentation/transport_recovery_controller.cpp').read_text()
paired = (root / 'src/features/streaming/presentation/paired_transport_recovery_controller.cpp').read_text()


def method(source, name):
    start = source.index('ApplicationController::' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


transport = method(playback, 'startTransport')
retry = method(recovery, 'retryTransport')
advance = method(recovery, 'advanceTransportRecovery')
state = method(recovery, 'handleTransportState')
complete = method(recovery, 'completeTransportRecovery')
pump = method(playback, 'pumpStream')
close = method(playback, 'closeTransport')
final = method(playback, 'closeStream')
for glue in (transport, recovery, paired, pump, close):
    for forbidden in ('guard_.adopt', 'checkout_.create', 'startStream(', 'disarmExitHandling(',
                      'transportReconnect_.reset()', 'sessionPending_ = false', 'recentLaunch_.close()'):
        assert forbidden not in glue, forbidden
assert 'stream_->start({streamRental_, resumeOnly})' in transport
assert close.index('media_->stopWorkers()') < close.index('stream_->close()')
assert 'streamRental_ = rental;' in method(playback, 'startStream')
assert 'if (final) stream_->close();' in close
assert 'else if (!stream_->disconnectForRetry()) {\n                retryAllowed = false;' in close
assert 'catch (...) { retryAllowed = false; }' in close
assert 'return final || retryAllowed;' in close
assert close.index('if (!final && !retryAllowed) return false;') < close.index('stream_.reset();')
assert 'if (!closeTransport(false)) {' in retry
assert 'stopTransportRecovery("transport-retry-terminal"); return;' in retry
assert 'streamRental_ = {};' in final and 'transportReconnect_.cancel();' in final
assert 'stream_ || transportReconnect_.recovering()' in app
assert 'stream_ || !streamRental_.empty() || transportReconnect_.recovering()' in app
assert pump.index('handleStreamOverlay(') < pump.index('advanceTransportRecovery(')
assert pump.index('stream_->pump();') < pump.index('handleTransportState(') < pump.index('advanceTransportRecovery(')
assert 'stream_->ready() && !stream_->disconnected()' in pump
assert 'switch (stream_->recoveryState())' in state
assert 'transportReconnect_.recovering() || videoRecovery_.active()' in pump
assert 'return !transportReconnect_.recovering() && stream_' in inputs
finish = method(rental, 'beginFinish')
assert finish.index('stopAfterTask_ = true;') < finish.index('start(Task::Finish')
assert finish.index('start(Task::CheckoutStop') < finish.index('closeStream();')
assert finish.index('start(Task::Finish') < finish.index('closeStream();')
shutdown = method(rental, 'shutdownSession')
assert shutdown.index('services_->cancel();') < shutdown.index('closeStream();')
assert shutdown.index('disarmExitHandling();') < shutdown.index('closeStream();')
assert 'beginFinish(' not in shutdown and 'armExitHandling();' not in shutdown.replace('disarmExitHandling();', '')
assert 'while (' not in shutdown
assert 'if (sessionPending_ && !recoveryPaused_ && !stopAfterTask_ && !focused)' in app
assert 'beginFinish("focus-lost")' in app and 'beginFinish("application-exit")' not in app
ui_text = (root / 'src/core/constants/ui_text.hpp').read_text()
for notice in ('PaidRentalNotice', 'AdoptRentalNotice'):
    line = next(line for line in ui_text.splitlines() if f'char {notice}[]' in line)
    assert 'Закрытие из HOME может оставить аренду активной' in line
    assert 'проверьте сайт сервиса' in line
print('Same-rental reconnect application safety boundaries passed (static only)')

# Both providers execute the same state, retry, backoff, start and cancellation code.
for shared in (retry, advance, state, complete):
    assert 'ProviderId::' not in shared
assert 'transportReconnect_.poll(now)' in advance
assert 'requestTransportRecovery()' in advance
assert 'neutralizeStreamInput();' in retry
stop = method(recovery, 'stopTransportRecovery')
assert stop.index('stopAfterTask_ = true;') < stop.index('beginFinish(')
assert 'transportReconnect_.cancel();' in stop
request = method(paired, 'requestPairedTransportReconnect')
preflight = method(paired, 'completePairedTransportReconnect')
assert 'paired_->inspectForResume()' in request
assert 'result.rental == streamRental_' in preflight
assert 'completeTransportRecovery(preparation)' in preflight
assert 'startTransport(' not in paired and 'transportReconnect_.poll(' not in paired
assert 'startTransport(true)' in complete
assert complete.index('transportReconnect_.poll(') < complete.index('startTransport(true)')
assert pump.index('retryMouseReleases();') > pump.index('stream_->pump();')
assert 'mouseButtons_.reset();' in close
print('Shared MTS/FH recovery, provider preflight and mouse release hooks passed (static only)')
