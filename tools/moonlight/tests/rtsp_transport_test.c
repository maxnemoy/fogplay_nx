#include "Limelight-internal.h"
#include <assert.h>
#include <pthread.h>
#include <time.h>
#include "../../../third_party/moonlight/prepared/core/RtspConnection.c"

volatile bool ConnectionInterrupted;
struct sockaddr_storage RemoteAddr;
SOCKADDR_LEN AddrLen = sizeof(struct sockaddr_in);
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
uint16_t RtspPortNumber;

static void logMessage(const char* format, ...) { (void)format; }
uint64_t PltGetMillis(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
void PltSleepMs(int ms) {
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&delay, NULL);
}
static void* interruptSoon(void* unused) {
    (void)unused;
    PltSleepMs(50);
    ConnectionInterrupted = true;
    return NULL;
}
static int listenLocal(void) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    assert(server >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(server, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(server, 0) == 0);
    socklen_t length = sizeof(addr);
    assert(getsockname(server, (struct sockaddr*)&addr, &length) == 0);
    memcpy(&RemoteAddr, &addr, sizeof(addr));
    RtspPortNumber = ntohs(addr.sin_port);
    return server;
}
static void* respondRtsp(void* listener) {
    const int connection = accept(*(int*)listener, NULL, NULL);
    assert(connection >= 0);
    char request[1024];
    assert(recv(connection, request, sizeof(request), 0) > 0);
    const char response[] = "RTSP/1.0 200 OK\r\nCSeq: 0\r\n\r\n";
    assert(send(connection, response, sizeof(response) - 1, 0) == sizeof(response) - 1);
    closeSocket(connection);
    return NULL;
}
int main(void) {
    ListenerCallbacks.logMessage = logMessage;
    int server = listenLocal();
    // The first unaccepted connection fills this socket's zero-sized backlog.
    int occupied = connectTcpSocket(&RemoteAddr, AddrLen, RtspPortNumber, 1);
    assert(occupied >= 0);
    pthread_t interrupter;
    assert(pthread_create(&interrupter, NULL, interruptSoon, NULL) == 0);
    uint64_t began = PltGetMillis();
    assert(FogMoonlightConnectTcpSocket(&RemoteAddr, AddrLen, RtspPortNumber, 10) == INVALID_SOCKET);
    assert(errno == EINTR && PltGetMillis() - began < 250);
    pthread_join(interrupter, NULL);
    closeSocket(occupied);
    closeSocket(server);

    // A host accepts the RTSP request but never sends its response. Exercise
    // the entire prepared TCP transaction, not a replacement polling loop.
    ConnectionInterrupted = false;
    server = listenLocal();
    RTSP_MESSAGE request, response;
    createRtspRequest(&request, NULL, 0, "OPTIONS", "rtsp://127.0.0.1", "RTSP/1.0", 0, NULL, NULL, 0);
    assert(pthread_create(&interrupter, NULL, interruptSoon, NULL) == 0);
    int error = 0;
    began = PltGetMillis();
    assert(!transactRtspMessageTcp(&request, &response, &error));
    assert(error == EINTR && PltGetMillis() - began < 250 && sock == INVALID_SOCKET);
    pthread_join(interrupter, NULL);
    closeSocket(server);

    // Cancellation also slices the 500ms ECONNREFUSED retry sleep.
    ConnectionInterrupted = false;
    assert(pthread_create(&interrupter, NULL, interruptSoon, NULL) == 0);
    began = PltGetMillis();
    assert(!transactRtspMessageTcp(&request, &response, &error));
    assert(PltGetMillis() - began < 250 && sock == INVALID_SOCKET);
    pthread_join(interrupter, NULL);

    // The sum of 100ms slices still honors the original total wait budget.
    ConnectionInterrupted = false;
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    struct pollfd waiting = {.fd = sockets[0], .events = POLLIN};
    began = PltGetMillis();
    assert(FogMoonlightPollSockets(&waiting, 1, 230) == 0);
    assert(PltGetMillis() - began >= 230 && PltGetMillis() - began < 500);
    assert(write(sockets[1], "x", 1) == 1);
    assert(FogMoonlightPollSockets(&waiting, 1, 15000) == 1);
    closeSocket(sockets[0]); closeSocket(sockets[1]);

    server = listenLocal();
    pthread_t responder;
    assert(pthread_create(&responder, NULL, respondRtsp, &server) == 0);
    assert(transactRtspMessageTcp(&request, &response, &error));
    assert(response.message.response.statusCode == 200 && sock == INVALID_SOCKET);
    freeMessage(&response);
    pthread_join(responder, NULL);
    closeSocket(server);

    // Read-only connectivity probing after a stopped stream must remain usable.
    server = listenLocal();
    ConnectionInterrupted = true;
    int probe = connectTcpSocket(&RemoteAddr, AddrLen, RtspPortNumber, 1);
    assert(probe >= 0);
    closeSocket(probe); closeSocket(server);
    puts("Prepared RTSP connect, response and retry cancellation passed (<250ms including scheduler allowance)");
    return 0;
}
