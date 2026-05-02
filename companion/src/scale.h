#pragma once
#include <cmath>
#include <cstdint>
#include <array>

// Mirrors worklet.js SCALE_INTERVALS / quantizeToScale
static constexpr int SCALE_INTERVALS[3][12] = {
    {0,1,2,3,4,5,6,7,8,9,10,11},  // chromatic (12 notes)
    {0,2,4,5,7,9,11,-1,-1,-1,-1,-1}, // major (7 notes)
    {0,2,3,5,7,8,10,-1,-1,-1,-1,-1}, // natural minor (7 notes)
};
static constexpr int SCALE_SIZES[3] = {12, 7, 7};

inline double hz_to_midi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
inline double midi_to_hz(double m)  { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

inline double quantize_to_scale(double midi_note, int root, int scale_idx) {
    const int* ivs   = SCALE_INTERVALS[scale_idx];
    const int  count = SCALE_SIZES[scale_idx];
    double best      = midi_note;
    double best_dist = 999.0;
    int base = (int)std::floor(midi_note / 12.0);
    for (int oct = base - 1; oct <= base + 1; ++oct) {
        for (int k = 0; k < count; ++k) {
            double c = oct * 12.0 + root + ivs[k];
            double d = std::fabs(c - midi_note);
            if (d < best_dist) { best_dist = d; best = c; }
        }
    }
    return best;
}
