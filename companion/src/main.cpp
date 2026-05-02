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

#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  static std::string pid_path() { char t[MAX_PATH]; GetTempPathA(MAX_PATH,t); return std::string(t)+"silvertune-companion.pid"; }
  static int  get_pid()  { return (int)GetCurrentProcessId(); }
  static bool pid_alive(int pid) { HANDLE h=OpenProcess(SYNCHRONIZE,FALSE,(DWORD)pid); if(!h) return false; DWORD r=WaitForSingleObject(h,0); CloseHandle(h); return r==WAIT_TIMEOUT; }
  static void kill_pid(int pid)  { HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,(DWORD)pid); if(h){TerminateProcess(h,0);CloseHandle(h);} }
#else
  #include <signal.h>
  #include <unistd.h>
  static std::string pid_path() { return "/tmp/silvertune-companion.pid"; }
  static int  get_pid()  { return (int)getpid(); }
  static bool pid_alive(int pid) { return kill(pid, 0) == 0; }
  static void kill_pid(int pid)  { kill(pid, SIGKILL); }
#endif

static std::atomic<bool> g_running{true};

static void handle_pidfile() {
    std::string path = pid_path();
    // Kill any existing instance
    FILE* f = std::fopen(path.c_str(), "r");
    if (f) {
        int old_pid = 0;
        std::fscanf(f, "%d", &old_pid);
        std::fclose(f);
        if (old_pid > 0 && pid_alive(old_pid)) {
            std::printf("Killing previous instance (pid %d)\n", old_pid);
            kill_pid(old_pid);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    // Write own PID
    f = std::fopen(path.c_str(), "w");
    if (f) { std::fprintf(f, "%d\n", get_pid()); std::fclose(f); }
}

static void remove_pidfile() {
    std::remove(pid_path().c_str());
}

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD) { g_running.store(false); return TRUE; }
static void setup_signals() { SetConsoleCtrlHandler(ctrl_handler, TRUE); }
#else
static void sig_handler(int) { g_running.store(false); }
static void setup_signals() { signal(SIGTERM, sig_handler); signal(SIGINT, sig_handler); }
#endif

// ── Shared state ──────────────────────────────────────────────────────────────

static constexpr double DETUNE = 1.00463;
static constexpr uint16_t WS_PORT = 2747;

class FormantPreserver {
public:
    static constexpr int ORDER = 16;
    static constexpr int WIN   = 512;
    static constexpr int HOP   = 128;

    void reset() {
        memset(win_buf_, 0, sizeof(win_buf_));
        memset(a_,       0, sizeof(a_));
        memset(x_mem_,   0, sizeof(x_mem_));
        memset(y_mem_,   0, sizeof(y_mem_));
        win_pos_   = 0;
        hop_count_ = 0;
    }

    float analyze(float x) {
        win_buf_[win_pos_] = x;
        win_pos_ = (win_pos_ + 1) & (WIN - 1);
        if (++hop_count_ >= HOP) { hop_count_ = 0; update_lpc(); }
        float e = x;
        for (int k = 0; k < ORDER; ++k) e += a_[k] * x_mem_[k];
        for (int k = ORDER - 1; k > 0; --k) x_mem_[k] = x_mem_[k - 1];
        x_mem_[0] = x;
        return e;
    }

    float synthesize(float e) {
        float y = e;
        for (int k = 0; k < ORDER; ++k) y -= a_[k] * y_mem_[k];
        for (int k = ORDER - 1; k > 0; --k) y_mem_[k] = y_mem_[k - 1];
        y_mem_[0] = y;
        return y;
    }

private:
    float win_buf_[WIN] = {};
    int   win_pos_      = 0;
    int   hop_count_    = 0;
    float a_[ORDER]     = {};
    float x_mem_[ORDER] = {};
    float y_mem_[ORDER] = {};

    void update_lpc() {
        float r[ORDER + 1] = {};
        for (int lag = 0; lag <= ORDER; ++lag) {
            float sum = 0.0f;
            for (int i = lag; i < WIN; ++i) {
                int i0 = (win_pos_ + WIN - 1 - i) & (WIN - 1);
                int i1 = (win_pos_ + WIN - 1 - i - lag + WIN * 2) & (WIN - 1);
                sum += win_buf_[i0] * win_buf_[i1];
            }
            r[lag] = sum;
        }
        if (r[0] < 1e-10f) { memset(a_, 0, sizeof(a_)); return; }
        float tmp[ORDER] = {};
        float E = r[0];
        for (int m = 0; m < ORDER; ++m) {
            float km_num = -r[m + 1];
            for (int j = 0; j < m; ++j) km_num -= tmp[j] * r[m - j];
            float km = km_num / E;
            if (km >  0.9999f) km =  0.9999f;
            if (km < -0.9999f) km = -0.9999f;
            float new_tmp[ORDER];
            for (int j = 0; j < m; ++j) new_tmp[j] = tmp[j] + km * tmp[m - 1 - j];
            new_tmp[m] = km;
            for (int j = 0; j <= m; ++j) tmp[j] = new_tmp[j];
            E *= (1.0f - km * km);
            if (E < 1e-20f) break;
        }
        memcpy(a_, tmp, sizeof(a_));
    }
};

struct Params {
    int    key        = 0;
    int    scale      = 1;
    double tune       = 1.0;
    double wide       = 0.0;
    double gain       = 1.0;
    double volume     = 1.0;
    bool   formant_on = false;
    bool   vibrato_on = false;
    bool   chord_on   = false;
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
static GrainShifter g_chord1;
static GrainShifter g_chord1_dbl;
static GrainShifter g_chord2;
static GrainShifter g_chord2_dbl;
static FormantPreserver g_formant;

static double g_held_ratio    = 1.0;
static double g_current_ratio = 1.0;
static double g_locked_midi  = -1.0;
static int    g_low_conf     = 0;
static int    g_det_note     = -1;
static int    g_corr_note    = -1;

static double g_held_chord1    = 1.0;
static double g_current_chord1 = 1.0;
static double g_held_chord2    = 1.0;
static double g_current_chord2 = 1.0;
static float  g_vibrato_lp_hz  = 0.0f;

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

            if (hz > 20.0f) {
                float vib_ref = (g_vibrato_lp_hz > 0.0f) ? g_vibrato_lp_hz : hz;
                float vib_diff_cents = std::fabs(hz_to_midi(hz) - hz_to_midi(vib_ref)) * 100.0f;
                if (vib_diff_cents > 80.0f)
                    g_vibrato_lp_hz = hz;
                else
                    g_vibrato_lp_hz += 0.2f * (hz - g_vibrato_lp_hz);
            }
            float use_hz = (p.vibrato_on && g_vibrato_lp_hz > 0.0f) ? g_vibrato_lp_hz : hz;

            if (p.tune < 0.01) {
                g_held_ratio = 1.0;
            } else if (use_hz > 80.0f && use_hz < 2000.0f && conf > 0.5f) {
                double det_midi    = hz_to_midi(use_hz);
                if (g_locked_midi < 0.0) g_locked_midi = det_midi;
                g_low_conf = 0;
                int    det_round = (int)std::round(g_locked_midi);
                double corr      = quantize_to_scale(det_round, p.key, p.scale);
                int    corr_int  = (int)std::round(corr);
                double ref_hz    = midi_to_hz((float)g_locked_midi);
                double ratio     = midi_to_hz(corr_int) / ref_hz;
                ratio = 1.0 + (ratio - 1.0) * p.tune;
                g_held_ratio = std::max(0.5, std::min(2.0, ratio));
                g_det_note   = (int)std::round(det_midi);
                g_corr_note  = corr_int;
                if (p.chord_on) {
                    float c1_hz = midi_to_hz(corr_int + 7.0f);
                    float c2_hz = midi_to_hz(corr_int - 5.0f);
                    g_held_chord1 = std::max(0.5, std::min(2.0, (double)(c1_hz / ref_hz)));
                    g_held_chord2 = std::max(0.5, std::min(2.0, (double)(c2_hz / ref_hz)));
                }
            } else if (conf < 0.35f) {
                if (++g_low_conf >= 3) {
                    g_locked_midi    = -1.0;
                    g_low_conf       = 0;
                    g_held_ratio     = 1.0;
                    g_current_ratio  = 1.0;
                    g_held_chord1    = 1.0;
                    g_current_chord1 = 1.0;
                    g_held_chord2    = 1.0;
                    g_current_chord2 = 1.0;
                }
            }
        }

        // Noise gate: track signal energy, open/close smoothly
        g_gate_energy = 0.999f * g_gate_energy + 0.001f * s * s;
        float gate_open = g_gate_energy > GATE_THRESHOLD ? 1.0f : 0.0f;
        float coeff = gate_open > g_gate_gain ? (1.0f - GATE_ATTACK) : (1.0f - GATE_RELEASE);
        g_gate_gain += coeff * (gate_open - g_gate_gain);

        g_rms_acc   += s * s;
        g_rms_count++;

        float chase_coeff = ((float)p.tune >= 1.0f) ? 1.0f : (float)(p.tune * p.tune * 0.03);
        g_current_ratio  += (g_held_ratio  - g_current_ratio)  * chase_coeff;
        g_current_chord1 += (g_held_chord1 - g_current_chord1) * chase_coeff;
        g_current_chord2 += (g_held_chord2 - g_current_chord2) * chase_coeff;

        float src = p.formant_on ? g_formant.analyze(s) : s;
        float wet = g_wet.process(src, g_current_ratio);
        float dbl = g_dbl.process(src, g_current_ratio * (float)DETUNE);
        float out_s = wet + dbl * (float)p.wide;
        if (p.formant_on) out_s = g_formant.synthesize(out_s);
        if (p.chord_on) {
            float c1  = g_chord1.process(src, (float)g_current_chord1);
            float c1d = g_chord1_dbl.process(src, (float)g_current_chord1 * (float)DETUNE);
            float c2  = g_chord2.process(src, (float)g_current_chord2);
            float c2d = g_chord2_dbl.process(src, (float)g_current_chord2 * (float)DETUNE);
            float c1out = c1 + c1d * (float)p.wide;
            float c2out = c2 + c2d * (float)p.wide;
            if (p.formant_on) {
                c1out = g_formant.synthesize(c1out);
                c2out = g_formant.synthesize(c2out);
            }
            out_s = out_s * 0.6f + c1out * 0.25f + c2out * 0.25f;
        }
        float processed = out_s * (float)p.volume;
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
    setup_signals();
    handle_pidfile();

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

    while (g_running.load()) {
        std::printf("Waiting for browser connection...\n");
        std::fflush(stdout);

        if (!ws.accept()) {
            continue;
        }
        std::printf("Browser connected\n");
        std::fflush(stdout);
        ws.send(std::string("{\"type\":\"version\",\"v\":\"") + COMPANION_VERSION + "\"}");

        while (g_running.load() && ws.connected()) {
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
                {
                    double fn = json_num(msg, "formantOn", -1.0);
                    if (fn >= 0.0) g_params.formant_on = fn > 0.5;
                    double vn = json_num(msg, "vibratoOn", -1.0);
                    if (vn >= 0.0) g_params.vibrato_on = vn > 0.5;
                    double cn = json_num(msg, "chordOn", -1.0);
                    if (cn >= 0.0) g_params.chord_on = cn > 0.5;
                }
            }

            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        std::printf("Browser disconnected\n");
        std::fflush(stdout);
    }

    ma_device_uninit(&dev);
    remove_pidfile();
    return 0;
}
