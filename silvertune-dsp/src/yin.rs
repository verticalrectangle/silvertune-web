// YIN pitch detector — matches worklet.js exactly: BUF=512, HALF=256, HOP=32

pub const BUF_SIZE: usize = 512;
const HALF: usize = BUF_SIZE / 2;
const HOP: usize = 32;
const THRESHOLD: f32 = 0.15;
const MIN_HZ: f32 = 80.0;
const MAX_HZ: f32 = 2000.0;

pub struct Yin {
    buf: [f32; BUF_SIZE],
    diff: [f32; HALF],
    pos: usize,
    pub pitch_hz: f32,
    pub confidence: f32,
    sample_rate: f32,
}

impl Yin {
    pub fn sample_rate(&self) -> f32 { self.sample_rate }

    pub fn new(sample_rate: f32) -> Self {
        Self {
            buf: [0.0; BUF_SIZE],
            diff: [0.0; HALF],
            pos: 0,
            pitch_hz: -1.0,
            confidence: 0.0,
            sample_rate,
        }
    }

    // Returns true when a new pitch estimate is ready.
    #[inline]
    pub fn push(&mut self, sample: f32) -> bool {
        self.buf[self.pos] = sample;
        self.pos += 1;
        if self.pos >= BUF_SIZE {
            self.detect();
            // Shift buffer by HOP (matches JS yinBuf.copyWithin(0, YIN_HOP))
            self.buf.copy_within(HOP.., 0);
            self.pos = BUF_SIZE - HOP;
            true
        } else {
            false
        }
    }

    fn detect(&mut self) {
        let buf = &self.buf;

        // Step 1: difference function
        for tau in 0..HALF {
            let mut s = 0.0f32;
            for j in 0..HALF {
                let d = buf[j] - buf[j + tau];
                s += d * d;
            }
            self.diff[tau] = s;
        }

        // Step 2: cumulative mean normalized difference
        self.diff[0] = 1.0;
        let mut running_sum = 0.0f32;
        for tau in 1..HALF {
            running_sum += self.diff[tau];
            self.diff[tau] = if running_sum > 0.0 {
                self.diff[tau] * tau as f32 / running_sum
            } else {
                1.0
            };
        }

        // Step 3: absolute threshold — find first tau below threshold
        let min_tau = (self.sample_rate / MAX_HZ) as usize + 1;
        let max_tau = ((self.sample_rate / MIN_HZ) as usize).min(HALF - 2);

        let mut best_tau = None;
        let mut tau = min_tau;
        while tau <= max_tau {
            if self.diff[tau] < THRESHOLD {
                // Local minimum search
                while tau + 1 <= max_tau && self.diff[tau + 1] < self.diff[tau] {
                    tau += 1;
                }
                best_tau = Some(tau);
                break;
            }
            tau += 1;
        }

        match best_tau {
            None => {
                self.pitch_hz = -1.0;
                self.confidence = 0.0;
            }
            Some(tau) => {
                // Step 4: parabolic interpolation
                let refined = if tau > 0 && tau < HALF - 1 {
                    let x0 = self.diff[tau - 1];
                    let x1 = self.diff[tau];
                    let x2 = self.diff[tau + 1];
                    let denom = 2.0 * x1 - x2 - x0;
                    if denom.abs() > 1e-6 {
                        tau as f32 + 0.5 * (x2 - x0) / denom
                    } else {
                        tau as f32
                    }
                } else {
                    tau as f32
                };

                self.pitch_hz = self.sample_rate / refined;
                self.confidence = 1.0 - self.diff[tau];
            }
        }
    }
}

pub fn hz_to_midi(hz: f32) -> f32 {
    69.0 + 12.0 * (hz / 440.0).log2()
}
