pub const BUF_SIZE: usize = 512;
const HALF: usize = BUF_SIZE / 2;
const HOP: usize = 128;
const THRESHOLD: f32 = 0.15;

pub struct Yin {
    buf: [f32; BUF_SIZE],
    diff: [f32; HALF],
    pos: usize,
    pub pitch_hz: f32,
    pub confidence: f32,
    sample_rate: f32,
}

impl Yin {
    pub fn new(sample_rate: f32) -> Self {
        Self {
            buf: [0.0; BUF_SIZE],
            diff: [0.0; HALF],
            pos: 0,
            pitch_hz: 0.0,
            confidence: 0.0,
            sample_rate,
        }
    }

    #[inline]
    pub fn push(&mut self, sample: f32) -> bool {
        self.buf[self.pos] = sample;
        self.pos += 1;
        if self.pos >= BUF_SIZE {
            self.detect();
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
            let mut sum = 0.0f32;
            for j in 0..HALF {
                let d = buf[j] - buf[j + tau];
                sum += d * d;
            }
            self.diff[tau] = sum;
        }

        // Step 2: cumulative mean normalized difference
        self.diff[0] = 1.0;
        let mut running_sum = 0.0f32;
        for tau in 1..HALF {
            running_sum += self.diff[tau];
            self.diff[tau] = self.diff[tau] * tau as f32 / running_sum;
        }

        // Step 3: absolute threshold — search from tau=2, no Hz clamp
        let mut tau_est = 0usize;
        let mut tau = 2usize;
        while tau < HALF {
            if self.diff[tau] < THRESHOLD {
                while tau + 1 < HALF && self.diff[tau + 1] < self.diff[tau] {
                    tau += 1;
                }
                tau_est = tau;
                break;
            }
            tau += 1;
        }

        if tau_est == 0 {
            self.pitch_hz = 0.0;
            self.confidence = 0.0;
            return;
        }

        // Step 4: parabolic interpolation (matches C++ plugin exactly)
        let s0 = self.diff[tau_est - 1];
        let s1 = self.diff[tau_est];
        let s2 = self.diff[if tau_est + 1 < HALF { tau_est + 1 } else { tau_est }];
        let denom = 2.0 * (2.0 * s1 - s2 - s0);
        let shift = if denom.abs() > 1e-12 { (s0 - s2) / denom } else { 0.0 };
        let refined = tau_est as f32 + shift;

        self.pitch_hz = self.sample_rate / refined;
        self.confidence = (1.0 - s1).clamp(0.0, 1.0);
    }
}

pub fn hz_to_midi(hz: f32) -> f32 {
    69.0 + 12.0 * (hz / 440.0).log2()
}
