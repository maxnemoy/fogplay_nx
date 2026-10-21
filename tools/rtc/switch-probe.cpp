#include <switch.h>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <rtc/rtc.hpp>
#include <usrsctp.h>
#include <cstddef>
#include <cstring>
static void probeNumericFailure(int line, const char *condition, const char *host, const char *service);
#define RTC_NUMERIC_FAILURE probeNumericFailure
#include "numeric-contract.h"
#include "wakeup-check.h"
static std::atomic<bool> probeData{false}, probeMedia{false}, probeWebSocket{false};
#define RTC_LOOPBACK_REPORT(data, media, ws) do { probeData = (data); probeMedia = (media); probeWebSocket = (ws); } while (false)
static void probeRtcLog(rtc::LogLevel, std::string);
static void probeRtcStage(const char *);
#define RTC_LOOPBACK_STAGE(text) probeRtcStage(text)
#define RTC_LOOPBACK_LOG probeRtcLog
#define main rtc_probe_loopback
#include "host-loopback.cpp"
#undef main
extern "C" int rtc_switch_pipe(int fds[2]);
extern "C" int rtc_switch_poll(struct pollfd *fds, nfds_t count, int timeout);
static FILE *probeLog;
static bool probeConsoleActive = true;
static std::mutex probeLogMutex;
static unsigned probeLogCount;
static void probeRtcStage(const char *text) {
    // Worker-thread diagnostics must not touch the console/framebuffer.
    std::lock_guard<std::mutex> lock(probeLogMutex);
    if (probeLog) {
        std::fprintf(probeLog, "RTC stage: %.160s\n", text);
        std::fflush(probeLog);
        fsync(fileno(probeLog));
        fsdevCommitDevice("sdmc");
    }
}
static void probeRtcLog(rtc::LogLevel level, std::string message) {
    if (level > rtc::LogLevel::Warning) return;
    std::lock_guard<std::mutex> lock(probeLogMutex);
    if (probeLog && probeLogCount++ < 120) {
        std::fprintf(probeLog, "RTC warning/error: %.256s\n", message.c_str());
        std::fflush(probeLog);
    }
}
static void report(const char *text) {
    std::printf("%s\n", text);
    if (probeConsoleActive && appletGetFocusState() == AppletFocusState_InFocus) consoleUpdate(NULL);
    std::lock_guard<std::mutex> lock(probeLogMutex);
    if (probeLog) {
        std::fprintf(probeLog, "%s\n", text);
        std::fflush(probeLog);
        fsync(fileno(probeLog));
        fsdevCommitDevice("sdmc");
    }
}
static void probeNumericFailure(int line, const char *condition, const char *host, const char *service) {
    char message[320];
    std::snprintf(message, sizeof(message), "Numeric failure line=%d host=[%.79s] service=[%.15s] check=%.160s", line, host, service, condition);
    report(message);
}
static int wakeupSend(void *, int fd, char value) { return send(fd, &value, 1, 0); }
static int wakeupRecv(void *, int fd, char *value) { return recv(fd, value, 1, 0); }
static int wakeupPoll(void *, struct pollfd *fd, int timeout) { return rtc_switch_poll(fd, 1, timeout); }
static int64_t wakeupNow(void *) { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static int checkWakeup() {
    int fds[2];
    if (rtc_switch_pipe(fds) != 0) {
        const int savedError = errno;
        char message[128];
        std::snprintf(message,sizeof(message),"UDP pair creation failed errno=%d EAGAIN=%d EWOULDBLOCK=%d",savedError,EAGAIN,EWOULDBLOCK);
        report(message); return 1;
    }
    const int readFlags = fcntl(fds[0], F_GETFL);
    const int readFlagsError = readFlags < 0 ? errno : 0;
    const int writeFlags = fcntl(fds[1], F_GETFL);
    const int writeFlagsError = writeFlags < 0 ? errno : 0;
    char flagMessage[160];
    std::snprintf(flagMessage,sizeof(flagMessage),"UDP F_GETFL read=%d errno=%d write=%d errno=%d O_NONBLOCK=0x%x",readFlags,readFlagsError,writeFlags,writeFlagsError,unsigned(O_NONBLOCK));
    report(flagMessage);
    RtcWakeupOps ops{nullptr,wakeupSend,wakeupRecv,wakeupPoll,wakeupNow};
    RtcWakeupResult details;
    int result = rtcCheckWakeup(fds[0],fds[1],100,ops,details);
    // All failing errno values were captured by the helper before diagnostics.
    char message[320];
    std::snprintf(message,sizeof(message),
        "UDP details code=%d stage=%s cycle=%d count=%d send=%d send_errno=%d recv=%d recv_errno=%d poll=%d poll_errno=%d flags=0x%x EAGAIN=%d EWOULDBLOCK=%d",
        result,details.stage,details.cycle,details.count,details.sendResult,details.sendError,details.recvResult,details.recvError,
        details.pollResult,details.pollError,unsigned(details.flags),EAGAIN,EWOULDBLOCK);
    if (result) report(message);
    std::snprintf(message,sizeof(message),"UDP aggregate completed=%u received=%u wait_polls=%u extra_waits=%u elapsed_ms=%lld deadline_ms=1000/cycle",
        details.completed,details.totalReceived,details.polls,details.extraWaits,(long long)details.elapsedMillis);
    report(message);
    close(fds[0]); close(fds[1]);
    return result;
}
static int checkSctpAddressLayout() {
    static_assert(offsetof(sockaddr_conn, sconn_family) == offsetof(sockaddr, sa_family),
                  "SCTP family must use the native Switch sockaddr byte");
    static_assert(offsetof(sockaddr_conn, sconn_len) == offsetof(sockaddr, sa_len),
                  "SCTP length must use the native Switch sockaddr byte");
    sockaddr_conn address{};
    address.sconn_len = sizeof(address);
    address.sconn_family = AF_CONN;
    address.sconn_port = htons(5000);
    address.sconn_addr = &address;
    sa_family_t family{};
    std::memcpy(&family, reinterpret_cast<const char*>(&address) + offsetof(sockaddr, sa_family), sizeof(family));
    const bool ok = family == AF_CONN && address.sconn_len == sizeof(address) && ntohs(address.sconn_port) == 5000;
    char message[224];
    std::snprintf(message, sizeof(message),
        "SCTP native layout: %s size=%u family_offset=%u native_family_offset=%u length=%u family=%u AF_CONN=%u port=%u",
        ok ? "PASS" : "FAIL", unsigned(sizeof(address)), unsigned(offsetof(sockaddr_conn, sconn_family)),
        unsigned(offsetof(sockaddr, sa_family)), unsigned(address.sconn_len), unsigned(family), unsigned(AF_CONN),
        unsigned(ntohs(address.sconn_port)));
    report(message);
    return ok ? 0 : 34;
}

// Two ICE sockets request 1 MiB in each direction. Keep six additional
// small UDP sockets open to exercise wakeup-socket headroom before RTC starts.
// This bounded preflight is diagnostic, not a proof of all runtime allocations.
static int checkSocketBudget() {
    int sockets[8];
    for (int &fd : sockets) fd = -1;
    int result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const int savedError = sockets[i] < 0 ? errno : 0;
        const Result bsdResult = socketGetLastResult();
        char message[192];
        std::snprintf(message, sizeof(message), "Socket preflight open=%u fd=%d errno=%d bsd_result=0x%x", i, sockets[i], savedError, bsdResult);
        report(message);
        if (sockets[i] < 0) { result = 31; break; }
    }
    for (unsigned i = 0; !result && i < 2; ++i) {
        for (int option : {SO_RCVBUF, SO_SNDBUF}) {
            int requested = 1024 * 1024, accepted = 0;
            const int rc = setsockopt(sockets[i], SOL_SOCKET, option, &requested, sizeof(requested));
            const int savedError = rc < 0 ? errno : 0;
            const Result bsdResult = socketGetLastResult();
            socklen_t size = sizeof(accepted);
            const int getResult = getsockopt(sockets[i], SOL_SOCKET, option, &accepted, &size);
            const int getError = getResult < 0 ? errno : 0;
            char message[256];
            std::snprintf(message, sizeof(message),
                "Socket preflight buffer=%u %s requested=%d set=%d errno=%d bsd_result=0x%x get=%d get_errno=%d accepted=%d",
                i, option == SO_RCVBUF ? "receive" : "send", requested, rc, savedError, bsdResult, getResult, getError, accepted);
            report(message);
            if (rc < 0 || getResult < 0 || accepted < requested) { result = 32; break; }
        }
    }
    for (int fd : sockets) if (fd >= 0) {
        if (close(fd) < 0) {
            const int savedError = errno;
            char message[96];
            std::snprintf(message, sizeof(message), "Socket preflight close failed fd=%d errno=%d", fd, savedError);
            report(message); result = 33;
        }
    }
    report(result ? "Socket preflight FAIL; RTC skipped" : "Socket preflight PASS; all test sockets closed");
    return result;
}
static bool cleanupAfterFailure() {
    try {
        auto cleanup = rtc::Cleanup();
        if (cleanup.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            probeRtcLog(rtc::LogLevel::Error, "RTC failure cleanup timed out");
            return false;
        }
        rtc::JoinCleanupThreads();
        cleanup.get();
        return true;
    } catch (const std::exception &error) {
        probeRtcLog(rtc::LogLevel::Error, std::string("RTC failure cleanup exception: ") + error.what());
        return false;
    } catch (...) {
        probeRtcLog(rtc::LogLevel::Error, "RTC failure cleanup unknown exception");
        return false;
    }
}

/* Diagnostic failure is recorded independently of the homebrew exit status. */
static int finishProbe(int diagnosticResult, bool socketReady, bool nifmReady, bool rtcMayOwnSockets) {
    char message[128];
    std::snprintf(message, sizeof(message), "RESULT %s (diagnostic %d) - see switch/fogplay_nx/rtc-probe.log",
                  diagnosticResult ? "FAIL" : "PASS", diagnosticResult);
    report(message);
    if (!rtcMayOwnSockets) {
        report("Exit: initialized platform services cleanup begin");
        if (nifmReady) { report("Exit: nifmExit begin"); nifmExit(); report("Exit: nifmExit complete"); }
        if (socketReady) { report("Exit: socketExit begin"); socketExit(); report("Exit: socketExit complete"); }
        report("Exit: initialized platform services cleanup complete");
    } else {
        report("Exit: waiting for RTC cleanup before allowing return to hbmenu");
        bool cleanupSucceeded = false;
        try {
            auto cleanup = rtc::Cleanup();
            while (cleanup.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
                if (probeConsoleActive && !appletMainLoop()) probeConsoleActive = false;
            }
            rtc::JoinCleanupThreads();
        cleanup.get();
            cleanupSucceeded = true;
        } catch (...) {
            report("RTC cleanup failed. Close this application from HOME; return to hbmenu is blocked.");
        }
        if (!cleanupSucceeded) {
            // Do not return into the loader or destroy static state while RTC
            // may still own threads. Full application mode permits OS closure.
            for (;;) {
                if (probeConsoleActive && !appletMainLoop()) probeConsoleActive = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (nifmReady) { nifmExit(); report("Exit: nifmExit complete after RTC cleanup"); }
        if (socketReady) { socketExit(); report("Exit: socketExit complete after RTC cleanup"); }
    }
    report("Press + to exit");
    PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    bool plusPressed = false;
    while (probeConsoleActive && appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) { plusPressed = true; break; }
        if (appletGetFocusState() == AppletFocusState_InFocus) consoleUpdate(NULL);
    }
    report(plusPressed ? "Exit: + pressed" : "Exit: applet loop ended");
    report("Exit: consoleExit begin");
    consoleExit(NULL);
    // The console is gone; write directly to the still-open durable log.
    {
        std::lock_guard<std::mutex> lock(probeLogMutex);
        if (probeLog) {
            std::fprintf(probeLog, "Exit: consoleExit complete; return 0; diagnostic %d\n", diagnosticResult);
            std::fflush(probeLog);
            fsync(fileno(probeLog));
            std::fclose(probeLog);
            probeLog = nullptr;
        }
    }
    fsdevCommitDevice("sdmc");
    return 0;
}
int main() {
    consoleInit(NULL);
    fsdevMountSdmc();
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/fogplay_nx", 0777);
    probeLog = std::fopen("sdmc:/switch/fogplay_nx/rtc-probe.log", "w");
    report("RTC local probe 0.3.10: no account, rental, STUN/TURN or remote service");
    const AppletType type = appletGetAppletType();
    char configMessage[320];
    std::snprintf(configMessage, sizeof(configMessage), "Platform applet_type=%u full_application=%d",
        unsigned(type), type == AppletType_Application || type == AppletType_SystemApplication);
    report(configMessage);
    if (type != AppletType_Application && type != AppletType_SystemApplication) {
        report("Full application required: hold R while launching an installed game, then open this probe in hbmenu.");
        report("Album mode skipped: RTC resource and graphics tests will not run.");
        return finishProbe(12, false, false, false);
    }
    // Pinned Moonlight-Switch/Borealis full-application network profile.
    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.num_bsd_sessions = 12;
    config.sb_efficiency = 8;
    std::snprintf(configMessage, sizeof(configMessage),
        "BSD config sessions=%u efficiency=%u tcp_tx=%u tcp_rx=%u tcp_tx_max=%u tcp_rx_max=%u udp_tx=%u udp_rx=%u ENOBUFS=%d",
        config.num_bsd_sessions, config.sb_efficiency, config.tcp_tx_buf_size, config.tcp_rx_buf_size,
        config.tcp_tx_buf_max_size, config.tcp_rx_buf_max_size, config.udp_tx_buf_size, config.udp_rx_buf_size, ENOBUFS);
    report(configMessage);
    Result rc = socketInitialize(&config);
    if (R_FAILED(rc)) { report("FAIL socket initialization"); return finishProbe(10, false, false, false); }
    rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) { report("FAIL NIFM initialization"); return finishProbe(11, true, false, false); }
    int numericResult = rtc_numeric_contract();
    char numericMessage[96];
    std::snprintf(numericMessage, sizeof(numericMessage), "Numeric address IPv4/IPv6/ports/scope/bounds: %s (line %d)", numericResult ? "FAIL" : "PASS", numericResult);
    report(numericMessage);
    int result = numericResult ? 30 : checkWakeup();
    bool rtcMayOwnSockets = false;
    char message[96];
    std::snprintf(message, sizeof(message), "UDP wake/drain/re-poll x100: %s (code %d)", result ? "FAIL" : "PASS", result);
    report(numericResult ? "UDP wake/drain/re-poll: SKIPPED after numeric failure" : message);
    if (!result) result = checkSctpAddressLayout();
    if (!result) result = checkSocketBudget();
    if (!result) {
        report("Running real local WebRTC data + SRTP media + WebSocket, cleanup included...");
        struct RunState { std::atomic<bool> done{false}; std::atomic<int> result{20}; bool cleanupSafe = false; };
        auto state = std::make_shared<RunState>();
        rtcMayOwnSockets = true;
        std::thread worker;
        try { worker = std::thread([state] {
            try {
                state->result = rtc_probe_loopback();
                // 0/1 return only after the shared loopback cleanup completed;
                // 2 means its five-second cleanup deadline expired.
                state->cleanupSafe = state->result != 2;
            }
            catch (const std::exception &e) {
                probeRtcLog(rtc::LogLevel::Error, std::string("RTC probe exception: ") + e.what());
                state->result = 20;
                state->cleanupSafe = cleanupAfterFailure();
            }
            catch (...) {
                state->result = 21;
                state->cleanupSafe = cleanupAfterFailure();
            }
            state->done = true;
        }); } catch (const std::exception &error) {
            report("FAIL creating RTC worker; RTC was not started");
            probeRtcLog(rtc::LogLevel::Error, error.what());
            return finishProbe(23, true, true, false);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        bool timedOut = false;
        while (!state->done) {
            if (!timedOut && std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                report("FAIL RTC 45-second deadline. Waiting for worker shutdown; if stuck, close this application from HOME.");
            }
            // Service focus/exit messages. A static diagnostic screen needs no
            // repeated framebuffer dequeue while network workers are active.
            if (probeConsoleActive && !appletMainLoop()) probeConsoleActive = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        worker.join();
        result = timedOut ? 22 : int(state->result.load());
        rtcMayOwnSockets = !state->cleanupSafe;
        std::snprintf(message, sizeof(message), "Local counters: data=%d SRTP=%d WebSocket=%d (result %d)",
                      int(probeData.load()), int(probeMedia.load()), int(probeWebSocket.load()), result);
        report(message);
        report(result ? "FAIL local RTC loopback/cleanup" : "PASS data + SRTP + WebSocket + cleanup");
    }
    return finishProbe(result, true, true, rtcMayOwnSockets);
}
