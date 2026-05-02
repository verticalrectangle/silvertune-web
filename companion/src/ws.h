#pragma once
#include <string>
#include <cstdint>

// Minimal single-client WebSocket server for localhost
class WsServer {
public:
    explicit WsServer(uint16_t port);
    ~WsServer();

    bool     accept();                   // blocking until client connects
    std::string recv();                  // non-blocking; returns "" if no data
    bool     send(const std::string& s); // returns false on error/disconnect
    bool     connected() const;
    void     disconnect();

private:
    int server_ = -1;
    int client_ = -1;

    bool doHandshake();
    bool recvExact(void* buf, int len);
};
