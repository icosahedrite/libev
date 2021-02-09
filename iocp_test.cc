#include <stdio.h>
#include <time.h>
#include <unistd.h>
//#include <io.h>
#include <winsock2.h>
#include <windows.h>

#define N_SOCKPAIRS 8192
#define BUFFER_SIZE 10240
#define MIN_PENDING (BUFFER_SIZE*1)
#define MAX_IO_THREADS 0 // 0=default

#define DIE_ERROR \
    do { \
      fprintf(stderr, "Error at line %d (errno = %d)\n", \
      __LINE__, GetLastError()); \
      exit(1); \
    } while(0)

typedef struct {
    SOCKET in;
    SOCKET out;
    int pending;
} Pair;

typedef void (OverlappedCallback)(DWORD, DWORD, struct PO*);

typedef struct PO {
    OVERLAPPED overlapped;
    OverlappedCallback* callback;
    Pair* pair;
} PairOverlapped;

HANDLE iocp;
Pair pairs[N_SOCKPAIRS] = { 0 };
char buffer[BUFFER_SIZE] = { 1 };
double sent = 0, received = 0;

int socketpair(SOCKET& sock1, SOCKET& sock2) {
    SOCKET listen_sock;
    SOCKADDR_IN addr1;
    SOCKADDR_IN addr2;
    int addr1_len = sizeof(addr1);
    int addr2_len = sizeof(addr2);

    sock1 = INVALID_SOCKET;
    sock2 = INVALID_SOCKET;

    if ((listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) == INVALID_SOCKET)
        goto error;

    memset((void*)&addr1, 0, sizeof(addr1));
    addr1.sin_family = AF_INET;
    addr1.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr1.sin_port = 0;

    if (bind(listen_sock, (SOCKADDR*)&addr1, addr1_len) == SOCKET_ERROR)
        goto error;

    if (getsockname(listen_sock, (SOCKADDR*)&addr1, &addr1_len) == SOCKET_ERROR)
        goto error;

    if (listen(listen_sock, 1))
        goto error;

    if ((sock1 = socket(AF_INET, SOCK_STREAM, IPPROTO_IP)) == INVALID_SOCKET)
        goto error;

    if (connect(sock1, (SOCKADDR*)&addr1, addr1_len))
        goto error;

    if ((sock2 = accept(listen_sock, 0, 0)) == INVALID_SOCKET)
        goto error;

    if (getpeername(sock1, (SOCKADDR*)&addr1, &addr1_len) == INVALID_SOCKET)
        goto error;

    if (getsockname(sock2, (SOCKADDR*)&addr2, &addr2_len) == INVALID_SOCKET)
        goto error;

    if (addr1_len != addr2_len
        || addr1.sin_addr.s_addr != addr2.sin_addr.s_addr
        || addr1.sin_port != addr2.sin_port)
        goto error;

    closesocket(listen_sock);

    return 0;

error:
    int error = WSAGetLastError();

    if (listen_sock != INVALID_SOCKET)
        closesocket(listen_sock);

    if (sock1 != INVALID_SOCKET)
        closesocket(sock1);

    if (sock2 != INVALID_SOCKET)
        closesocket(sock2);

    WSASetLastError(error);

    return SOCKET_ERROR;
}

void s_sendto(Pair* pair);
void s_recvfrom(Pair* pair);

void after_send(DWORD error, DWORD bytes, PairOverlapped* o) {
    if (error) DIE_ERROR;
    o->pair->pending -= bytes;
    sent += bytes;
    free(o);

    s_sendto(o->pair);
}

void s_sendto(Pair* pair) {
    PairOverlapped* o;
    WSABUF buf;

    buf.len = sizeof(buffer);
    buf.buf = buffer;

    while (pair->pending < MIN_PENDING) {
        o = (PairOverlapped*)malloc(sizeof(*o));
        memset(o, 0, sizeof(o->overlapped));
        o->pair = pair;
        o->callback = after_send;

        if (WSASend(pair->out, &buf, 1, NULL, 0, (OVERLAPPED*)o, NULL)) {
            if (WSAGetLastError() != WSA_IO_PENDING) DIE_ERROR;
        }

        pair->pending += sizeof(buffer);
    }
}

void after_recv(DWORD error, DWORD bytes, PairOverlapped* o) {
    if (error) DIE_ERROR;
    received += bytes;
    free(o);

    s_recvfrom(o->pair);
}

void s_recvfrom(Pair* pair) {
    WSABUF buf;
    PairOverlapped* o;
    DWORD flags = 0;

    buf.len = sizeof(buffer);
    buf.buf = buffer;

    o = (PairOverlapped*)malloc(sizeof(*o));
    memset(o, 0, sizeof(o->overlapped));
    o->callback = after_recv;
    o->pair = pair;

    if (WSARecv(pair->in, &buf, 1, NULL, &flags, (OVERLAPPED*)o, NULL)) {
        if (WSAGetLastError() != WSA_IO_PENDING) DIE_ERROR;
    }
}

int main() {
    int i;
    Pair* pair;
    double start, now, last = 0, delta;
    OverlappedCallback callback;
    BOOL result;
    ULONG64 context, context_in = 0;
    DWORD bytes;
    PairOverlapped* o;
    DWORD error;

    // Initialize winsock - https://docs.microsoft.com/en-us/windows/win32/winsock/initializing-winsock
    WSAData ws_info;
    WORD version = MAKEWORD(2, 2);
    if (WSAStartup(version, &ws_info)) {
        DIE_ERROR;
    }

    // Create input/output (I/O) completion port 
    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)&context_in, MAX_IO_THREADS);
    if (!iocp) DIE_ERROR;

    // Create n pair of connected sockets and loop
    for (i = 0; i < N_SOCKPAIRS; i++) {
        pair = &pairs[i];
        if (socketpair(pair->in, pair->out) == SOCKET_ERROR)
            DIE_ERROR;

        // Associates one or more file handles with the port
        if (!CreateIoCompletionPort((HANDLE)pair->in, iocp, (ULONG_PTR)&context_in, 0))
            DIE_ERROR;
        if (!CreateIoCompletionPort((HANDLE)pair->out, iocp, (ULONG_PTR)&context_in, 0))
            DIE_ERROR;
    }

    // Get the number of seconds used by the CPU
    start = clock() / CLOCKS_PER_SEC;

    // Start loop and proccess send/recv
    for (i = 0; i < N_SOCKPAIRS; i++) {
        pair = &pairs[i];
        s_sendto(pair);
        s_recvfrom(pair);
    }

    // Main event loop
    while (1) {
        BOOL result = GetQueuedCompletionStatus(iocp, &bytes, &context, (OVERLAPPED**)&o, INFINITE);
        if (!o) DIE_ERROR;
        error = result ? 0 : GetLastError();
        o->callback(error, bytes, o);

        now = clock() / CLOCKS_PER_SEC;
        delta = now - start;
        if (delta - last >= 1.0) {
            fprintf(stdout, "Inbound: %f Mbit/s, Outbound: %f Mbit/s\n", received / delta * 8 / 1000000, sent / delta * 8 / 1000000);
            last = delta;
        }
    }
}
