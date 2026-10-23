"""Static safety boundaries for Switch-only application glue (not runtime proof)."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
playback = (root / 'src/features/streaming/presentation/playback_controller.cpp').read_text()
app = (root / 'src/app/application.cpp').read_text()
inputs = (root / 'src/features/streaming/presentation/stream_input_controller.cpp').read_text()
rental = (root / 'src/features/session/presentation/rental_controller.cpp').read_text()


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
retry = method(playback, 'retryTransport')
pump = method(playback, 'pumpStream')
close = method(playback, 'closeTransport')
final = method(playback, 'closeStream')
for glue in (transport, retry, pump, close):
    for forbidden in ('guard_.adopt', 'checkout_.create', 'startStream(', 'disarmExitHandling(',
                      'transportReconnect_.reset()', 'sessionPending_ = false', 'recentLaunch_.close()'):
        assert forbidden not in glue, forbidden
assert 'stream_->start(streamHash_,' in transport
assert 'streamHash_ = hash;' in method(playback, 'startStream')
assert 'if (final) stream_->close();' in close
assert 'else if (!stream_->disconnectForRetry()) {\n                retryAllowed = false;' in close
assert 'catch (...) { retryAllowed = false; }' in close
assert 'return final || retryAllowed;' in close
assert close.index('if (!final && !retryAllowed) return false;') < close.index('stream_.reset();')
for site in (retry, pump):
    assert 'if (!closeTransport(false)) {' in site
    assert 'beginFinish("transport-retry-terminal"); return;' in site
assert 'streamHash_.clear();' in final and 'transportReconnect_.cancel();' in final
assert 'stream_ || transportReconnect_.recovering()' in app
assert 'stream_ || !streamHash_.empty() || transportReconnect_.recovering()' in app
assert pump.index('handleStreamOverlay(') < pump.index('transportReconnect_.poll(')
assert pump.index('stream_->pump();') < pump.index('transportReconnect_.poll(')
assert pump.index('stream_->retryableError()') < pump.index('transportReconnect_.poll(')
assert 'stream_->ready() && !stream_->disconnected()' in pump
assert 'if (stream_->retryableError()) retryTransport(' in pump
assert 'transportReconnect_.recovering() || videoRecovery_.active()' in pump
assert 'return !transportReconnect_.recovering() && stream_' in inputs
finish = method(rental, 'beginFinish')
assert finish.index('stopAfterTask_ = true;') < finish.index('start(Task::Finish')
assert finish.index('start(Task::CheckoutStop') < finish.index('closeStream();')
assert finish.index('start(Task::Finish') < finish.index('closeStream();')
shutdown = method(rental, 'shutdownSession')
assert shutdown.index('beginFinish(') < shutdown.index('closeStream();')
assert 'if (sessionPending_ && !stopAfterTask_ && !focused)' in app
assert 'beginFinish("focus-lost")' in app and 'beginFinish("application-exit")' in app
print('Same-rental reconnect application safety boundaries passed (static only)')
