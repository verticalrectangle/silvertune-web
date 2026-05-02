#define NOMINMAX
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
    int    scale     = 1;
    double tune      = 1.0;
    double wide      = 0.0;
    double gain      = 1.0;
    double volume    = 1.0;
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

static double g_held_ratio = 1.0;
static int    g_det_note   = -1;
static int    g_corr_note  = -1;

static float   g_rms_acc    = 0.0f;
static uint32_t g_rms_count = 0;
static uint32_t g_post_counter = 0;
static constexpr uint32_t POST_EVERY = 512;

// Pre-emphasis + RMS normalisation for YIN only
static float g_pre_prev   = 0.0f;
static float g_yin_energy = 1e-6f;
static constexpr float YIN_TARGET   = 0.08f;
static constexpr float YIN_MAX_GAIN = 80.0f;

// Noise gate
static float g_gate_energy = 1e-6f;         // leaky RMS² of gained signal
static constexpr float GATE_THRESHOLD = 2e-5f; // ~-47 dBFS RMS
static constexpr float GATE_ATTACK  = 0.9995f; // ~1000-sample (~21ms) attack
static constexpr float GATE_RELEASE = 0.9999f; // ~5000-sample (~104ms) release
static float g_gate_gain = 0.0f;             // smoothed 0→1 gate state

// ── Audio callback ────────────────────────────────────────────────────────────

static void audio_callback(ma_device* /*dev*/, void* out_buf, const void* in_buf, ma_uint32 n)
{
    const float* in  = (const float*)in_buf;
    float*       out = (float*)out_buf;

    Params p;
    { std::lock_guard<std::mutex> lk(g_params_mtx); p = g_params; }

    for (ma_uint32 i = 0; i < n; ++i) {
        float s = in[i] * (float)p.gain;

        // Pre-emphasis (one-pole high-pass) + RMS normalisation for YIN
        float s_pre = s - 0.95f * g_pre_prev;
        g_pre_prev  = s;
        g_yin_energy = 0.999f * g_yin_energy + 0.001f * s_pre * s_pre;
        float norm_gain = YIN_TARGET / std::sqrt(g_yin_energy);
        if (norm_gain > YIN_MAX_GAIN) norm_gain = YIN_MAX_GAIN;
        float s_yin = s_pre * norm_gain;

        g_yin.push_sample(s_yin);
        if (g_yin.pending) {
            g_yin.run_detect();
            float hz   = g_yin.pitch_hz;
            float conf = g_yin.confidence;
            if (hz > 80.0f && hz < 2000.0f && conf > 0.5f) {
                double det_midi_f = hz_to_midi(hz);
                int    det_round  = (int)std::round(det_midi_f);
                double corr       = quantize_to_scale(det_round, p.key, p.scale);
                int    corr_int   = (int)std::round(corr);
                double cents_off  = std::fabs(det_midi_f - corr_int) * 100.0;
                g_det_note  = det_round;
                g_corr_note = corr_int;
                if (cents_off < 40.0) {
                    // Within dead zone — no correction needed
                    g_held_ratio = 1.0;
                } else {
                    double ratio = midi_to_hz(corr_int) / (double)hz;
                    ratio = 1.0 + (ratio - 1.0) * p.tune;
                    double new_ratio = std::max(0.5, std::min(2.0, ratio));
                    if (std::fabs(new_ratio - g_held_ratio) > 0.06) {
                        g_wet.reset_phases();
                        g_dbl.reset_phases();
                    }
                    g_held_ratio = new_ratio;
                }
            } else {
                g_held_ratio = 1.0;
            }
        }

        // Noise gate: track signal energy, open/close smoothly
        g_gate_energy = 0.999f * g_gate_energy + 0.001f * s * s;
        float gate_open = g_gate_energy > GATE_THRESHOLD ? 1.0f : 0.0f;
        float coeff = gate_open > g_gate_gain ? (1.0f - GATE_ATTACK) : (1.0f - GATE_RELEASE);
        g_gate_gain += coeff * (gate_open - g_gate_gain);

        g_rms_acc   += s * s;
        g_rms_count++;

        float wet = g_wet.process(s, g_held_ratio);
        float dbl = g_dbl.process(s, g_held_ratio * (float)DETUNE);
        float processed = (wet + dbl * (float)p.wide) * (float)p.volume;
        out[i] = (processed * g_gate_gain) + (s * (float)p.volume * (1.0f - g_gate_gain));

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
    std::printf("Silvertune Companion " COMPANION_VERSION "\n");
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
        ws.send(std::string("{\"type\":\"version\",\"v\":\"") + COMPANION_VERSION + "\"}");

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
                std::lock_guard<std::mutex> lk(g_params_mtx);
                g_params.key    = (int)json_num(msg, "key",    g_params.key);
                g_params.scale  = (int)json_num(msg, "scale",  g_params.scale);
                g_params.tune   = json_num(msg, "tune",   g_params.tune);
                g_params.wide   = json_num(msg, "wide",   g_params.wide);
                g_params.gain   = json_num(msg, "gain",   g_params.gain);
                g_params.volume = json_num(msg, "volume", g_params.volume);
            }

            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        std::printf("Browser disconnected\n");
        std::fflush(stdout);
    }

    ma_device_uninit(&dev);
    return 0;
}
