#pragma once
#include <cstdint>
#include <string>
#include <cstring>
#include <algorithm>

// Minimal SHA-1 + Base64 for WebSocket handshake

static std::string sha1_b64(const std::string& msg) {
    uint32_t h0=0x67452301u, h1=0xEFCDAB89u,
             h2=0x98BADCFEu, h3=0x10325476u, h4=0xC3D2E1F0u;

    // Pad message
    std::string p = msg;
    p += (char)0x80;
    while (p.size() % 64 != 56) p += (char)0x00;
    uint64_t ml = (uint64_t)msg.size() * 8;
    for (int i = 7; i >= 0; --i) p += (char)((ml >> (i*8)) & 0xff);

    auto rol32 = [](uint32_t v, int n) { return (v << n) | (v >> (32-n)); };

    for (size_t off = 0; off < p.size(); off += 64) {
        uint32_t w[80] = {};
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint8_t)p[off+i*4]<<24)|((uint8_t)p[off+i*4+1]<<16)|
                   ((uint8_t)p[off+i*4+2]<<8) |(uint8_t)p[off+i*4+3];
        for (int i = 16; i < 80; ++i) {
            uint32_t v = w[i-3]^w[i-8]^w[i-14]^w[i-16];
            w[i] = rol32(v, 1);
        }
        uint32_t a=h0, b=h1, c=h2, d=h3, e=h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i<20) { f=(b&c)|((~b)&d); k=0x5A827999u; }
            else if (i<40) { f=b^c^d;           k=0x6ED9EBA1u; }
            else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDCu; }
            else           { f=b^c^d;           k=0xCA62C1D6u; }
            uint32_t t = rol32(a,5)+f+e+k+w[i];
            e=d; d=c; c=rol32(b,30); b=a; a=t;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
    }

    uint8_t digest[20];
    auto store = [&](int idx, uint32_t v) {
        for (int i = 0; i < 4; ++i) digest[idx*4+i] = (v >> (24-i*8)) & 0xff;
    };
    store(0,h0); store(1,h1); store(2,h2); store(3,h3); store(4,h4);

    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (int i = 0; i < 20; i += 3) {
        int rem = std::min(3, 20-i);
        uint32_t v = (uint32_t)digest[i]<<16;
        if (rem>1) v |= (uint32_t)digest[i+1]<<8;
        if (rem>2) v |= (uint32_t)digest[i+2];
        out += b64[(v>>18)&63];
        out += b64[(v>>12)&63];
        out += rem>1 ? b64[(v>>6)&63] : '=';
        out += rem>2 ? b64[v&63]      : '=';
    }
    return out;
}
