#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 3-tap grain shifter — matches worklet.js exactly
class GrainShifter {
public:
    static constexpr uint32_t BUF  = 4096;
    static constexpr uint32_t MASK = BUF - 1;
    static constexpr double   GRAIN = 128.0;

    GrainShifter() : write_(0) {
        for (auto& s : buf_) s = 0.0f;
        phases_[0] = 0.0; phases_[1] = 1.0/3.0; phases_[2] = 2.0/3.0;
    }

    void reset_phases() {
        phases_[0] = 0.0; phases_[1] = 1.0/3.0; phases_[2] = 2.0/3.0;
    }

    float process(float in, double ratio) {
        buf_[write_ & MASK] = in;
        ++write_;
        const double inc = (1.0 - ratio) / GRAIN;
        float out = 0.0f;
        for (int i = 0; i < 3; ++i) {
            phases_[i] += inc;
            phases_[i] -= std::floor(phases_[i]);
            out += lerp(phases_[i] * GRAIN + 2.0) * hann(phases_[i]);
        }
        return out * (2.0f / 3.0f);
    }

private:
    float    buf_[BUF];
    uint32_t write_;
    double   phases_[3];

    float lerp(double delay) const {
        double rp = (double)write_ - delay;
        rp = std::fmod(rp, (double)BUF);
        if (rp < 0.0) rp += BUF;
        uint32_t i0 = (uint32_t)rp & MASK;
        uint32_t i1 = (i0 + 1) & MASK;
        float frac  = (float)(rp - std::floor(rp));
        return buf_[i0] * (1.0f - frac) + buf_[i1] * frac;
    }

    static float hann(double p) {
        return 0.5f * (1.0f - (float)std::cos(2.0 * M_PI * p));
    }
};
