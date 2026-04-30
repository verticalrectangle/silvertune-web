// Scale quantization — matches worklet.js SCALE_INTERVALS exactly

const SCALE_INTERVALS: &[&[i32]] = &[
    &[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11], // chromatic
    &[0, 2, 4, 5, 7, 9, 11],                   // major
    &[0, 2, 3, 5, 7, 8, 10],                   // minor
];

pub fn quantize(midi: f32, key: u8, scale: u8) -> i32 {
    let ivs = SCALE_INTERVALS
        .get(scale as usize)
        .copied()
        .unwrap_or(SCALE_INTERVALS[1]);

    let note = midi.round() as i32;
    let octave = note.div_euclid(12);
    let chroma = note.rem_euclid(12);

    // Find the closest interval in the scale
    let mut best = ivs[0];
    let mut best_dist = i32::MAX;
    for &iv in ivs {
        let candidate = (iv + key as i32).rem_euclid(12);
        let dist = (candidate - chroma).abs().min(12 - (candidate - chroma).abs());
        if dist < best_dist {
            best_dist = dist;
            best = iv;
        }
    }

    let target_chroma = (best + key as i32).rem_euclid(12);
    octave * 12 + target_chroma
}

pub fn midi_to_ratio(from_midi: f32, to_midi: i32) -> f64 {
    2.0f64.powf((to_midi as f64 - from_midi as f64) / 12.0)
}
