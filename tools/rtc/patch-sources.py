from pathlib import Path
root = Path(__file__).resolve().parents[2]
p = root / 'third_party/rtc/libdatachannel/deps/libjuice/src/random.c'
s = p.read_text()
old = '#if defined(__linux__) && !defined(__ANDROID__) && (!defined(__GLIBC__) || __GLIBC__ > 2 || __GLIBC_MINOR__ >= 25)'
new = '''#if defined(__SWITCH__)
#include <switch.h>
static int random_bytes(void *buf, size_t size) {
    randomGet(buf, size);
    return 0;
}
#elif defined(__linux__) && !defined(__ANDROID__) && (!defined(__GLIBC__) || __GLIBC__ > 2 || __GLIBC_MINOR__ >= 25)'''
if new not in s:
    if old not in s: raise SystemExit('Unrecognized pinned libjuice random.c')
    p.write_text(s.replace(old, new, 1))
p = root / 'third_party/rtc/libdatachannel/deps/usrsctp/usrsctplib/user_environment.c'
s = p.read_text()
# Literal source match; no behavior changes on other targets.
new = '''#elif defined(__SWITCH__)
#include <switch.h>
void init_random(void) {}
void read_random(void *buf, size_t size) { randomGet(buf, size); }
void finish_random(void) {}
#else
#error "Unknown platform. Please provide platform specific RNG."
#endif'''
if new not in s:
    old = '#else' + chr(10) + '#error "Unknown platform. Please provide platform specific RNG."' + chr(10) + '#endif'
    if old not in s: raise SystemExit('Unrecognized pinned usrsctp RNG')
    p.write_text(s.replace(old, new, 1))

# The Switch wakeup pipe is a connected UDP pair. BSD Read/Write are not
# implemented by Eden; use socket operations only at these wakeup sites.
def patch_wakeup(path, read_old, read_fd, write_old, write_fd):
    source = path.read_text()
    read_new = ("#ifdef __SWITCH__\n" + "\t\twhile (::recv(" + read_fd + ", &dummy, 1, 0) > 0) {\n"
                "#else\n" + read_old + "\n#endif")
    write_new = ("#ifdef __SWITCH__\n" + "\tif (::send(" + write_fd + ", &dummy, 1, 0) < 0 && errno != EAGAIN &&\n"
                 "\t    errno != EWOULDBLOCK) {\n#else\n" + write_old + "\n#endif")
    # C sources have no scope-resolution operator.
    if path.suffix == '.c':
        read_new = read_new.replace('::recv', 'recv')
        write_new = write_new.replace('::send', 'send')
    for old, new in ((read_old, read_new), (write_old, write_new)):
        if new not in source:
            if source.count(old) != 1:
                raise SystemExit('Unrecognized pinned wakeup site in ' + str(path))
            source = source.replace(old, new, 1)
    path.write_text(source)

patch_wakeup(root / 'third_party/rtc/libdatachannel/src/impl/pollinterrupter.cpp',
             '\t\twhile (::read(pfd.fd, &dummy, 1) > 0) {', 'pfd.fd',
             '\tif (::write(mPipeOut, &dummy, 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {',
             'mPipeOut')
patch_wakeup(root / 'third_party/rtc/libdatachannel/deps/libjuice/src/conn_poll.c',
             '\t\twhile (read(interrupt_pfd->fd, &dummy, 1) > 0) {', 'interrupt_pfd->fd',
             '\tif (write(registry_impl->interrupt_pipe_out, &dummy, 1) < 0 && errno != EAGAIN &&\n'
             '\t    errno != EWOULDBLOCK) {', 'registry_impl->interrupt_pipe_out')

# Roll back before returning a startup exception: no TokenPayload exists yet
# to schedule the usual asynchronous cleanup.
p = root / 'third_party/rtc/libdatachannel/src/impl/init.cpp'
s = p.read_text()
old = """\t\tInit::Instance().doInit();
\t\tif (cleanupFuture)
\t\t\t*cleanupFuture = cleanupPromise.get_future().share();"""
new = """\t\tauto future = cleanupPromise.get_future().share();
\t\tInit::Instance().doInit();
\t\tif (cleanupFuture)
\t\t\t*cleanupFuture = std::move(future);"""
if new not in s and 'completionFuture = cleanupPromise.get_future().share();' not in s:
    if s.count(old) != 1:
        raise SystemExit('Unrecognized pinned init token constructor')
    s = s.replace(old, new, 1)
start = s.index('void Init::doInit() {')
end = s.index('\nvoid Init::doCleanup()', start)
old_init = s[start:end]
new_init = """void Init::doInit() {
\t// mMutex needs to be locked. Do not call doCleanup() from rollback:
\t// it takes the same mutex and assumes every global service was initialized.
\tif (mInitialized)
\t\treturn;

\t// Refuse reuse if an earlier rollback could not finish safely.
\tmCleanupFuture.get();

\tbool sctpStarted = false;
\tbool dtlsStarted = false;
#if RTC_ENABLE_WEBSOCKET
\tbool pollStarted = false;
\tbool tlsStarted = false;
#endif
#if RTC_ENABLE_MEDIA
\tbool mediaStarted = false;
#endif
\tbool iceStarted = false;
#ifdef _WIN32
\tbool winsockStarted = false;
#endif

\ttry {
\t\tPLOG_DEBUG << "Global initialization";

#ifdef _WIN32
\t\tWSADATA wsaData;
\t\tif (WSAStartup(MAKEWORD(2, 2), &wsaData))
\t\t\tthrow std::runtime_error("WSAStartup failed, error=" + std::to_string(WSAGetLastError()));
\t\twinsockStarted = true;
#endif

\t\tunsigned int count = mThreadPoolSize > 0 ? mThreadPoolSize : std::thread::hardware_concurrency();
\t\tcount = std::max(count, MIN_THREADPOOL_SIZE);
\t\tPLOG_DEBUG << "Spawning " << count << " threads";
\t\tThreadPool::Instance().spawn(count);

#if RTC_ENABLE_WEBSOCKET
\t\tPollService::Instance().start();
\t\tpollStarted = true;
#endif

#if USE_GNUTLS
\t\t// Nothing to do
#elif USE_MBEDTLS
\t\t// Nothing to do
#else
\t\topenssl::init();
#endif

\t\t// Init starts usrsctp before allocating its Instances set, so even
\t\t// an allocation failure inside Init must finish the SCTP globals.
\t\tsctpStarted = true;
\t\tSctpTransport::Init();
\t\tSctpTransport::SetSettings(mCurrentSctpSettings);
\t\tDtlsTransport::Init();
\t\tdtlsStarted = true;
#if RTC_ENABLE_WEBSOCKET
\t\tTlsTransport::Init();
\t\ttlsStarted = true;
#endif
#if RTC_ENABLE_MEDIA
\t\tDtlsSrtpTransport::Init();
\t\tmediaStarted = true;
#endif
\t\tIceTransport::Init();
\t\ticeStarted = true;
\t\tmInitialized = true;
\t} catch (...) {
\t\ttry {
\t\t\t// spawn may have created some workers before throwing.
\t\t\tThreadPool::Instance().join();
\t\t\tThreadPool::Instance().clear();
#if RTC_ENABLE_WEBSOCKET
\t\t\tif (pollStarted)
\t\t\t\tPollService::Instance().join();
#endif
\t\t\tif (sctpStarted)
\t\t\t\tSctpTransport::Cleanup();
\t\t\tif (dtlsStarted)
\t\t\t\tDtlsTransport::Cleanup();
#if RTC_ENABLE_WEBSOCKET
\t\t\tif (tlsStarted)
\t\t\t\tTlsTransport::Cleanup();
#endif
#if RTC_ENABLE_MEDIA
\t\t\tif (mediaStarted)
\t\t\t\tDtlsSrtpTransport::Cleanup();
#endif
\t\t\tif (iceStarted)
\t\t\t\tIceTransport::Cleanup();
#ifdef _WIN32
\t\t\tif (winsockStarted)
\t\t\t\tWSACleanup();
#endif
\t\t} catch (...) {
\t\t\t// Cleanup callers must not mistake a failed rollback for readiness.
\t\t\tstd::promise<void> failedCleanup;
\t\t\tfailedCleanup.set_exception(std::current_exception());
\t\t\tmCleanupFuture = failedCleanup.get_future().share();
\t\t}
\t\tthrow;
\t}
}
"""
extended_init = new_init.replace('\tif (mInitialized)', '''#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
\tif (mCleanupFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
\t\tmCleanupFuture.get();
#endif
\tif (mInitialized)''', 1)
if old_init not in (new_init, extended_init):
    import hashlib
    if hashlib.sha256(old_init.encode()).hexdigest() != '9fcd11ed0857a8f83167bbcecee1698cde895d3453035ccd919d9abfe947f230':
        raise SystemExit('Unrecognized pinned global initialization')
    s = s[:start] + new_init + s[end:]
p.write_text(s)

# start() may fail creating its interrupter or its thread. Leave join() safe
# and release even the resources allocated before that failing operation.
p = root / 'third_party/rtc/libdatachannel/src/impl/pollservice.cpp'
s = p.read_text()
old = """void PollService::start() {
\tmSocks = std::make_unique<SocketMap>();
\tmInterrupter = std::make_unique<PollInterrupter>();
\tmStopped = false;
\tmThread = std::thread(&PollService::runLoop, this);
}"""
new = """void PollService::start() {
\ttry {
\t\tmSocks = std::make_unique<SocketMap>();
\t\tmInterrupter = std::make_unique<PollInterrupter>();
\t\tmStopped = false;
\t\tmThread = std::thread(&PollService::runLoop, this);
\t} catch (...) {
\t\tmStopped = true;
\t\tmInterrupter.reset();
\t\tmSocks.reset();
\t\tthrow;
\t}
}"""
if new not in s:
    if s.count(old) != 1:
        raise SystemExit('Unrecognized pinned poll service startup')
    s = s.replace(old, new, 1)
p.write_text(s)


# AF_CONN addresses must have the same family-byte position as libnx sockaddr.
# Public and private layouts must agree even outside this CMake build.
p = root / 'third_party/rtc/libdatachannel/deps/usrsctp/usrsctplib/usrsctp.h'
s = p.read_text()
old = r'''#if defined(__APPLE__) || defined(__Bitrig__) || defined(__DragonFly__) || \
    defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
struct sockaddr_conn {'''
new = old.replace('defined(__APPLE__)', 'defined(__APPLE__) || defined(__SWITCH__)', 1)
if new not in s:
    if s.count(old) != 1:
        raise SystemExit('Unrecognized pinned public sockaddr_conn layout')
    s = s.replace(old, new, 1)
p.write_text(s)
p = root / 'third_party/rtc/libdatachannel/deps/usrsctp/usrsctplib/netinet/sctp_os_userspace.h'
s = p.read_text()
old = 'struct sockaddr_conn {' + chr(10) + '#ifdef HAVE_SCONN_LEN'
new = 'struct sockaddr_conn {' + chr(10) + '#if defined(HAVE_SCONN_LEN) || defined(__SWITCH__)'
if new not in s:
    if s.count(old) != 1:
        raise SystemExit('Unrecognized pinned private sockaddr_conn layout')
    s = s.replace(old, new, 1)
p.write_text(s)

# Switch/newlib has no pthread detach syscall. Retain cleanup thread ownership
# until callers explicitly join after Cleanup is ready, before get() and NRO unload.
def patch_literal(path, old, new, label):
    source = path.read_text()
    if new not in source:
        if source.count(old) != 1:
            raise SystemExit('Unrecognized pinned ' + label)
        path.write_text(source.replace(old, new, 1))

init = root / 'third_party/rtc/libdatachannel/src/impl/init.cpp'
patch_literal(init, """\t\tauto future = cleanupPromise.get_future().share();
\t\tInit::Instance().doInit();
\t\tif (cleanupFuture)
\t\t\t*cleanupFuture = std::move(future);""",
"""\t\tcompletionFuture = cleanupPromise.get_future().share();
\t\tInit::Instance().doInit();
\t\tif (cleanupFuture)
\t\t\t*cleanupFuture = completionFuture;""", 'cleanup completion future')
patch_literal(init, """\t~TokenPayload() {
\t\tstd::thread t(""", """\t~TokenPayload() {
#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
\t\tInit::Instance().scheduleCleanup(std::move(cleanupPromise), completionFuture);
#else
\t\tstd::thread t(""", 'cleanup scheduling')
patch_literal(init, """\t\tt.detach();
\t}

\tstd::promise<void> cleanupPromise;""", """\t\tt.detach();
#endif
\t}

\tstd::promise<void> cleanupPromise;
\tstd::shared_future<void> completionFuture;""", 'cleanup thread ownership')
for method in ('token', 'preload'):
    declaration = 'init_token Init::token() {' if method == 'token' else 'void Init::preload() {'
    # Migrate the previous owned-cleanup patch as well as pristine upstream.
    prior = declaration + """
#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
\tjoinCleanupThreads(false);
#endif
\tstd::lock_guard lock(mMutex);"""
    updated = prior.replace('std::lock_guard lock(mMutex);', 'auto lock = lockForInit();')
    source = init.read_text()
    if prior in source:
        init.write_text(source.replace(prior, updated, 1))
    patch_literal(init, declaration + '\n\tstd::lock_guard lock(mMutex);',
        updated, 'cleanup reaping before ' + method)
patch_literal(init, """\tif (mInitialized)
\t\treturn;

\t// Refuse reuse if an earlier rollback could not finish safely.""",
"""#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
\tif (mCleanupFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
\t\tmCleanupFuture.get();
#endif
\tif (mInitialized)
\t\treturn;

\t// Refuse reuse if an earlier rollback could not finish safely.""", 'failed cleanup reuse guard')

cleanup_methods = """
#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
void Init::scheduleCleanup(std::promise<void> promise, std::shared_future<void> future) noexcept {
\tstd::shared_ptr<std::promise<void>> completion;
\ttry {
\t\tcompletion = std::make_shared<std::promise<void>>(std::move(promise));
\t\tauto record = std::make_shared<CleanupThread>();
\t\trecord->future = std::move(future);
\t\tstd::lock_guard lock(mCleanupThreadsMutex);
\t\t// Allocate and register ownership before any joinable thread exists.
\t\tmCleanupThreads.push_back(record);
\t\ttry {
\t\t\trecord->thread = std::thread([completion] {
\t\t\t\tutils::this_thread::set_name("RTC cleanup");
\t\t\t\ttry {
\t\t\t\t\tInit::Instance().doCleanup();
\t\t\t\t\tcompletion->set_value();
\t\t\t\t} catch (...) {
\t\t\t\t\tcompletion->set_exception(std::current_exception());
\t\t\t\t}
\t\t\t});
\t\t} catch (...) {
\t\t\tmCleanupThreads.pop_back();
\t\t\tthrow;
\t\t}
\t} catch (...) {
\t\ttry {
\t\t\tif (completion)
\t\t\t\tcompletion->set_exception(std::current_exception());
\t\t\telse
\t\t\t\tpromise.set_exception(std::current_exception());
\t\t} catch (...) {
\t\t\t// Destruction still supplies broken_promise if signaling itself fails.
\t\t}
\t}
}

void Init::joinCleanupThreads(bool waitForAll) {
\tstd::unique_lock joinLock(mCleanupJoinMutex, std::defer_lock);
\tif (waitForAll)
\t\tjoinLock.lock();
\telse if (!joinLock.try_lock())
\t\treturn; // A pool worker must not wait for a barrier joining that pool.

\tstd::vector<std::shared_ptr<CleanupThread>> records;
\t{
\t\tstd::lock_guard lock(mCleanupThreadsMutex);
\t\tfor (const auto &record : mCleanupThreads)
\t\t\tif (waitForAll || record->future.wait_for(std::chrono::seconds(0)) ==
\t\t\t                      std::future_status::ready)
\t\t\t\trecords.push_back(record);
\t}

\tstd::exception_ptr failure;
\tfor (const auto &record : records) {
\t\ttry {
\t\t\tif (record->thread.joinable())
\t\t\t\trecord->thread.join();
\t\t\trecord->future.get();
\t\t} catch (...) {
\t\t\tif (!failure)
\t\t\t\tfailure = std::current_exception();
\t\t}
\t\tif (!record->thread.joinable()) {
\t\t\tstd::lock_guard lock(mCleanupThreadsMutex);
\t\t\tmCleanupThreads.erase(
\t\t\t    std::remove(mCleanupThreads.begin(), mCleanupThreads.end(), record),
\t\t\t    mCleanupThreads.end());
\t\t}
\t}
\t// A failed join keeps the record owned by Init. Attempt every other join
\t// before reporting failure; never destroy a still-joinable local thread.
\tif (failure)
\t\tstd::rethrow_exception(failure);
}
#endif
"""
patch_literal(init, '\nvoid Init::doInit() {', cleanup_methods + '\nvoid Init::doInit() {', 'owned cleanup methods')

header = root / 'third_party/rtc/libdatachannel/src/impl/init.hpp'
patch_literal(header, '#include <mutex>', '#include <mutex>\n#include <thread>\n#include <vector>', 'cleanup header includes')
patch_literal(header, '\tstd::shared_future<void> cleanup();',
"""\tstd::shared_future<void> cleanup();
#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
\tvoid joinCleanupThreads(bool waitForAll = true);
#endif""", 'cleanup join interface')
patch_literal(header, '\tvoid doCleanup();', """\tvoid doCleanup();
#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
\tvoid scheduleCleanup(std::promise<void> promise, std::shared_future<void> future) noexcept;
\tstruct CleanupThread {
\t\tstd::shared_future<void> future;
\t\tstd::thread thread;
\t};
\tstd::mutex mCleanupThreadsMutex;
\tstd::mutex mCleanupJoinMutex;
\tstd::vector<std::shared_ptr<CleanupThread>> mCleanupThreads;
#endif""", 'cleanup join storage')

# Pending cleanup generations may be older than the one which actually tore
# down globals. Never wait for their future while holding doCleanup's mutex.
patch_literal(init, '\n} // namespace rtc::impl', """
std::unique_lock<std::mutex> Init::lockForInit() {
\tstd::unique_lock lock(mMutex);
\twhile (!mInitialized &&
\t       mCleanupFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
\t\tauto completion = mCleanupFuture;
\t\tlock.unlock();
\t\tcompletion.get();
\t\tlock.lock();
\t}
\treturn lock;
}

} // namespace rtc::impl""", 'cleanup future wait outside init mutex')
patch_literal(header, '\tvoid doInit();',
    '\tstd::unique_lock<std::mutex> lockForInit();\n\tvoid doInit();', 'initialization lock helper')

header = root / 'third_party/rtc/libdatachannel/include/rtc/global.hpp'
patch_literal(header, 'RTC_CPP_EXPORT std::shared_future<void> Cleanup();',
"""RTC_CPP_EXPORT std::shared_future<void> Cleanup();
// Fork extension: call after the Cleanup future is ready, before get() or platform teardown.
// Joins owned cleanup threads on Switch (or the host test configuration).
// Do not create new RTC objects concurrently with this final barrier.
RTC_CPP_EXPORT void JoinCleanupThreads();""", 'public cleanup join barrier')
global_source = root / 'third_party/rtc/libdatachannel/src/global.cpp'
patch_literal(global_source,
'std::shared_future<void> Cleanup() { return impl::Init::Instance().cleanup(); }',
"""std::shared_future<void> Cleanup() { return impl::Init::Instance().cleanup(); }
void JoinCleanupThreads() {
#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
\timpl::Init::Instance().joinCleanupThreads();
#endif
}""", 'public cleanup join implementation')

# Numeric candidates stay supported. Reject unresolved candidates explicitly:
# Switch has no detach support and synchronous DNS has no bounded timeout.
peer = root / 'third_party/rtc/libdatachannel/src/impl/peerconnection.cpp'
patch_literal(peer, """\t} else {
\t\t// We might need a lookup, do it asynchronously""", """\t} else {
#if defined(__SWITCH__) || defined(RTC_TEST_JOINABLE_CLEANUP)
\t\tthrow std::runtime_error("Hostname ICE candidates are not supported on this platform");
#else
\t\t// We might need a lookup, do it asynchronously""", 'Switch candidate lookup rejection')
patch_literal(peer, """\t\t\tt.detach();
\t\t}
\t}
}

string PeerConnection::localBundleMid()""", """\t\t\tt.detach();
\t\t}
#endif
\t}
}

string PeerConnection::localBundleMid()""", 'Switch candidate lookup boundary')

patch_literal(init, '#include <thread>', '#include <algorithm>\n#include <thread>', 'cleanup algorithm include')

# Keep mbedTLS idle TCP markers distinct from TLS EOF (WSS ping timeout).
tls = root / 'third_party/rtc/libdatachannel/src/impl/tlstransport.cpp'
patch_literal(tls, 'int TlsTransport::ReadCallback(void *ctx, unsigned char *buf, size_t len) {\n\tTlsTransport *t = static_cast<TlsTransport *>(ctx);\n\ttry {\n\t\tmessage_ptr &message = t->mIncomingMessage;\n\t\tsize_t &position = t->mIncomingMessagePosition;\n\n\t\tif (message && position >= message->size())\n\t\t\tmessage.reset();\n\n\t\tif (!message) {\n\t\t\tposition = 0;\n\t\t\twhile (auto next = t->mIncomingQueue.pop()) {\n\t\t\t\tmessage = *next;\n\t\t\t\tif (message->size() > 0)\n\t\t\t\t\tbreak;\n\t\t\t\telse\n\t\t\t\t\tt->recv(message); // Pass zero-sized messages through\n', 'int TlsTransport::ReadCallback(void *ctx, unsigned char *buf, size_t len) {\n\tTlsTransport *t = static_cast<TlsTransport *>(ctx);\n\ttry {\n\t\tmessage_ptr &message = t->mIncomingMessage;\n\t\tsize_t &position = t->mIncomingMessagePosition;\n\n\t\tif (message && position >= message->size())\n\t\t\tmessage.reset();\n\n\t\tif (!message) {\n\t\t\tposition = 0;\n\t\t\twhile (auto next = t->mIncomingQueue.pop()) {\n\t\t\t\tmessage = *next;\n\t\t\t\tif (message->size() > 0)\n\t\t\t\t\tbreak;\n\t\t\t\telse {\n\t\t\t\t\tt->recv(message); // Pass zero-sized idle notifications through\n\t\t\t\t\t// An idle marker is not TLS input or EOF. Once forwarded, an\n\t\t\t\t\t// empty running queue must return WANT_READ, not zero bytes.\n\t\t\t\t\tmessage.reset();\n\t\t\t\t}\n', 'mbedTLS idle marker reset')

# Per-agent ICE diagnostics and the UDP fairness-budget regression fix.
import runpy
runpy.run_path(str(root / 'tools/rtc/patch-ice-diagnostics.py'), run_name='__main__')
runpy.run_path(str(root / 'tools/rtc/patch-dtls-diagnostics.py'), run_name='__main__')

runpy.run_path(str(root / 'tools/rtc/patch-dtls-receive.py'), run_name='__main__')
runpy.run_path(str(root / 'tools/rtc/patch-dtls-ingress.py'), run_name='__main__')

runpy.run_path(str(root / 'tools/rtc/patch-rtcp-feedback.py'), run_name='__main__')
runpy.run_path(str(root / 'tools/rtc/patch-media-send-diagnostics.py'), run_name='__main__')
runpy.run_path(str(root / 'tools/rtc/patch-thread-observation.py'), run_name='__main__')
