#include "ws.h"
#include "sha1.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define INVALID_SOCK INVALID_SOCKET
  #define sock_close   closesocket
  static void sock_init() { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
  static int  sock_errno() { return WSAGetLastError(); }
  static bool would_block(int e) { return e == WSAEWOULDBLOCK; }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int sock_t;
  #define INVALID_SOCK (-1)
  #define sock_close   ::close
  static void sock_init() {}
  static int  sock_errno() { return errno; }
  static bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
#endif

#include <cstring>
#include <cstdio>
#include <string>

// Cast int↔SOCKET safely
static inline sock_t to_sock(int fd) { return (sock_t)(intptr_t)fd; }
static inline int    to_fd (sock_t s) { return (int)(intptr_t)s; }

WsServer::WsServer(uint16_t port) {
    sock_init();

    sock_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCK) { perror("socket"); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

#ifdef _WIN32
    DWORD tv_ms = 1000;
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
#else
    struct timeval tv{ 1, 0 };
    setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);

    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); sock_close(srv); return;
    }
    ::listen(srv, 1);
    server_ = to_fd(srv);
}

WsServer::~WsServer() {
    disconnect();
    if (server_ >= 0) { sock_close(to_sock(server_)); server_ = -1; }
}

bool WsServer::accept() {
    if (server_ < 0) return false;
    disconnect();
    sock_t c = ::accept(to_sock(server_), nullptr, nullptr);
    if (c == INVALID_SOCK) return false;
    client_ = to_fd(c);
    if (!doHandshake()) { disconnect(); return false; }
    return true;
}

bool WsServer::recvExact(void* buf, int len) {
    char* p = (char*)buf;
    while (len > 0) {
        int r = (int)::recv(to_sock(client_), p, len, 0);
        if (r <= 0) { disconnect(); return false; }
        p += r; len -= r;
    }
    return true;
}

bool WsServer::doHandshake() {
    // Read HTTP request line by line
    std::string req;
    char c = 0;
    while (true) {
        if (::recv(to_sock(client_), &c, 1, 0) <= 0) return false;
        req += c;
        if (req.size() >= 4 && req.substr(req.size()-4) == "\r\n\r\n") break;
        if (req.size() > 4096) return false;
    }

    // Extract Sec-WebSocket-Key
    const char* marker = "Sec-WebSocket-Key: ";
    auto pos = req.find(marker);
    if (pos == std::string::npos) return false;
    pos += strlen(marker);
    auto end = req.find("\r\n", pos);
    if (end == std::string::npos) return false;
    std::string key = req.substr(pos, end - pos);

    // Compute accept key
    std::string accept = sha1_b64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

    int sent = (int)::send(to_sock(client_), response.c_str(), (int)response.size(), 0);
    return sent == (int)response.size();
}

std::string WsServer::recv() {
    if (client_ < 0) return "";

    // Non-blocking check
    fd_set fds; FD_ZERO(&fds); FD_SET(to_sock(client_), &fds);
    struct timeval tv{0, 0};
    if (::select(client_+1, &fds, nullptr, nullptr, &tv) <= 0) return "";

    // Read frame header (2 bytes minimum)
    uint8_t hdr[2];
    if (!recvExact(hdr, 2)) return "";

    bool fin    = (hdr[0] & 0x80) != 0;
    uint8_t op  =  hdr[0] & 0x0f;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7f;

    if (op == 0x8) { disconnect(); return ""; } // close frame
    if (op == 0x9) {                             // ping → pong
        uint8_t pong[2] = {0x8a, 0x00};
        ::send(to_sock(client_), (char*)pong, 2, 0);
        return "";
    }

    if (len == 126) {
        uint8_t ext[2]; if (!recvExact(ext, 2)) return "";
        len = ((uint64_t)ext[0]<<8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8]; if (!recvExact(ext, 8)) return "";
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len<<8) | ext[i];
    }

    uint8_t mask[4] = {};
    if (masked && !recvExact(mask, 4)) return "";

    if (len > 65536) return ""; // sanity limit
    std::string payload(len, '\0');
    if (len > 0 && !recvExact(&payload[0], (int)len)) return "";

    if (masked)
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] ^= mask[i % 4];

    (void)fin;
    return payload;
}

bool WsServer::send(const std::string& s) {
    if (client_ < 0) return false;

    std::string frame;
    frame += (char)0x81; // FIN + text opcode
    size_t len = s.size();
    if (len <= 125) {
        frame += (char)len;
    } else if (len <= 65535) {
        frame += (char)126;
        frame += (char)(len >> 8);
        frame += (char)(len & 0xff);
    } else {
        frame += (char)127;
        for (int i = 7; i >= 0; --i) frame += (char)((len >> (i*8)) & 0xff);
    }
    frame += s;

    int r = (int)::send(to_sock(client_), frame.c_str(), (int)frame.size(), 0);
    if (r != (int)frame.size()) { disconnect(); return false; }
    return true;
}

bool WsServer::connected() const { return client_ >= 0; }

void WsServer::disconnect() {
    if (client_ >= 0) { sock_close(to_sock(client_)); client_ = -1; }
}
