#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "ws.h"
#include "yin.h"
#include "grain.h"
#include "scale.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

// ── Shared state ──────────────────────────────────────────────────────────────

static constexpr double DETUNE = 1.00463;
static constexpr uint16_t WS_PORT = 2747;

struct Params {
    int    key       = 0;
    int    scale     = 1;   // major by default
    double tune      = 1.0;
    double wide      = 0.0;
};

struct PitchReport {
    int   detected  = -1;
    int   corrected = -1;
    float rms       = 0.0f;
};

static std::mutex      g_params_mtx;
static Params          g_params;

static std::mutex      g_report_mtx;
static PitchReport     g_report;
static std::atomic<bool> g_report_ready{false};

// ── DSP state (audio thread only) ─────────────────────────────────────────────

static YinDetector  g_yin;
static GrainShifter g_wet;
static GrainShifter g_dbl;

static double g_held_ratio  = 1.0;
static int    g_det_note    = -1;
static int    g_corr_note   = -1;

static float   g_rms_acc    = 0.0f;
static uint32_t g_rms_count = 0;
static uint32_t g_post_counter = 0;
static constexpr uint32_t POST_EVERY = 512;

// ── Audio callback ────────────────────────────────────────────────────────────

static void audio_callback(ma_device* /*dev*/, void* out_buf, const void* in_buf, ma_uint32 n)
{
    const float* in  = (const float*)in_buf;
    float*       out = (float*)out_buf;

    Params p;
    { std::lock_guard<std::mutex> lk(g_params_mtx); p = g_params; }

    for (ma_uint32 i = 0; i < n; ++i) {
        float s = in[i];

        g_yin.push_sample(s);
        if (g_yin.pending) {
            g_yin.run_detect();
            float hz   = g_yin.pitch_hz;
            float conf = g_yin.confidence;
            if (hz > 80.0f && hz < 2000.0f && conf > 0.5f) {
                double det_midi  = hz_to_midi(hz);
                int    det_round = (int)std::round(det_midi);
                double corr      = quantize_to_scale(det_round, p.key, p.scale);
                int    corr_int  = (int)std::round(corr);
                double ratio     = midi_to_hz(corr_int) / (double)hz;
                ratio = 1.0 + (ratio - 1.0) * p.tune;
                g_held_ratio = std::max(0.5, std::min(2.0, ratio));
                g_det_note   = det_round;
                g_corr_note  = corr_int;
            }
        }

        g_rms_acc   += s * s;
        g_rms_count++;

        float wet = g_wet.process(s, g_held_ratio);
        float dbl = g_dbl.process(s, g_held_ratio * (float)DETUNE);
        out[i] = wet + dbl * (float)p.wide;

        ++g_post_counter;
        if (g_post_counter >= POST_EVERY) {
            g_post_counter = 0;
            float rms = std::sqrt(g_rms_acc / (float)std::max(1u, g_rms_count));
            g_rms_acc   = 0.0f;
            g_rms_count = 0;

            std::lock_guard<std::mutex> lk(g_report_mtx);
            g_report = { g_det_note, g_corr_note, rms };
            g_report_ready.store(true, std::memory_order_release);
        }
    }
}

// ── Tiny JSON helpers ─────────────────────────────────────────────────────────

static double json_num(const std::string& s, const char* key, double def) {
    std::string k = std::string("\"") + key + "\":";
    auto pos = s.find(k);
    if (pos == std::string::npos) return def;
    pos += k.size();
    while (pos < s.size() && s[pos] == ' ') ++pos;
    return std::stod(s.c_str() + pos);
}

static std::string json_pitch(int det, int corr, float rms) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
        "{\"type\":\"pitch\",\"detectedNote\":%d,\"correctedNote\":%d,\"rms\":%.4f}",
        det, corr, rms);
    return buf;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main()
{
    std::printf("Silvertune Companion v0.2.0\n");
    std::printf("Listening on ws://127.0.0.1:%u\n", (unsigned)WS_PORT);
    std::fflush(stdout);

    // Init YIN
    g_yin.init(48000.0f);

    // Init audio device
    ma_device_config cfg = ma_device_config_init(ma_device_type_duplex);
    cfg.capture.format   = ma_format_f32;
    cfg.capture.channels = 1;
    cfg.playback.format  = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.sampleRate       = 48000;
    cfg.dataCallback     = audio_callback;
    cfg.periodSizeInFrames = 128;

    ma_device dev;
    if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) {
        std::fprintf(stderr, "Failed to open audio device\n");
        return 1;
    }
    if (ma_device_start(&dev) != MA_SUCCESS) {
        std::fprintf(stderr, "Failed to start audio device\n");
        ma_device_uninit(&dev);
        return 1;
    }
    std::printf("Audio started: %s → %s @ %uHz\n",
        dev.capture.name, dev.playback.name, dev.sampleRate);
    std::fflush(stdout);

    WsServer ws(WS_PORT);

    while (true) {
        std::printf("Waiting for browser connection...\n");
        std::fflush(stdout);

        if (!ws.accept()) {
            std::fprintf(stderr, "accept() failed, retrying in 1s\n");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::printf("Browser connected\n");
        std::fflush(stdout);
        ws.send("{\"type\":\"version\",\"v\":\"0.2.0\"}");

        while (ws.connected()) {
            // Send pitch report if ready
            if (g_report_ready.load(std::memory_order_acquire)) {
                PitchReport r;
                {
                    std::lock_guard<std::mutex> lk(g_report_mtx);
                    r = g_report;
                    g_report_ready.store(false, std::memory_order_release);
                }
                ws.send(json_pitch(r.detected, r.corrected, r.rms));
            }

            // Receive params (non-blocking)
            std::string msg = ws.recv();
            if (!msg.empty()) {
                // Expect {"type":"params","key":K,"scale":S,"tune":T,"wide":W}
                std::lock_guard<std::mutex> lk(g_params_mtx);
                g_params.key   = (int)json_num(msg, "key",   g_params.key);
                g_params.scale = (int)json_num(msg, "scale", g_params.scale);
                g_params.tune  = json_num(msg, "tune",  g_params.tune);
                g_params.wide  = json_num(msg, "wide",  g_params.wide);
            }

            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        std::printf("Browser disconnected\n");
        std::fflush(stdout);
    }

    ma_device_uninit(&dev);
    return 0;
}
