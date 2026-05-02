#include "yin.h"
#include <cmath>

static constexpr float YIN_THRESHOLD = 0.15f;

void YinDetector::init(float sample_rate) {
    sr_  = sample_rate;
    pos_ = 0;
    pitch_hz = confidence = 0.0f;
    pending = false;
    for (auto& x : buf_)  x = 0.0f;
    for (auto& x : diff_) x = 0.0f;
}

void YinDetector::push_sample(float sample) {
    if (pending) return;
    buf_[pos_++] = sample;
    if (pos_ >= BUF_SIZE) pending = true;
}

void YinDetector::run_detect() {
    detect();
    for (uint32_t i = 0; i < BUF_SIZE - HOP_SIZE; ++i)
        buf_[i] = buf_[i + HOP_SIZE];
    pos_    = BUF_SIZE - HOP_SIZE;
    pending = false;
}

void YinDetector::detect() {
    // Step 1: difference function
    for (uint32_t tau = 0; tau < HALF; ++tau) {
        float sum = 0.0f;
        for (uint32_t j = 0; j < HALF; ++j) {
            float d = buf_[j] - buf_[j + tau];
            sum += d * d;
        }
        diff_[tau] = sum;
    }

    // Step 2: cumulative mean normalised difference
    diff_[0] = 1.0f;
    float running = 0.0f;
    for (uint32_t tau = 1; tau < HALF; ++tau) {
        running += diff_[tau];
        diff_[tau] = diff_[tau] * (float)tau / running;
    }

    // Step 3: absolute threshold — search from tau=2 (no Hz clamp)
    uint32_t tau_est = 0;
    for (uint32_t tau = 2; tau < HALF; ++tau) {
        if (diff_[tau] < YIN_THRESHOLD) {
            while (tau + 1 < HALF && diff_[tau+1] < diff_[tau]) ++tau;
            tau_est = tau;
            break;
        }
    }

    if (tau_est == 0) { pitch_hz = confidence = 0.0f; return; }

    // Step 4: parabolic interpolation
    float s0 = diff_[tau_est - 1];
    float s1 = diff_[tau_est];
    float s2 = diff_[tau_est+1 < HALF ? tau_est+1 : tau_est];
    float denom = 2.0f * (2.0f * s1 - s2 - s0);
    float shift = std::fabs(denom) > 1e-12f ? (s0 - s2) / denom : 0.0f;

    pitch_hz   = sr_ / ((float)tau_est + shift);
    confidence = std::max(0.0f, std::min(1.0f, 1.0f - s1));
}
