// 3-tap grain shifter — matches worklet.js exactly:
// grainSize=128, phases [0, 1/3, 2/3], COLA normalize ×2/3, BUF=4096

const BUF_SIZE: usize = 4096;
const MASK: usize = BUF_SIZE - 1;
const GRAIN_SIZE: f64 = 128.0;
const NUM_TAPS: usize = 3;
const NORMALIZE: f32 = 2.0 / 3.0;

pub struct GrainShifter {
    buf: Box<[f32; BUF_SIZE]>,
    write_pos: usize,
    phases: [f64; NUM_TAPS],
}

impl GrainShifter {
    pub fn new() -> Self {
        let mut phases = [0.0f64; NUM_TAPS];
        for (i, p) in phases.iter_mut().enumerate() {
            *p = i as f64 / NUM_TAPS as f64;
        }
        Self {
            buf: Box::new([0.0f32; BUF_SIZE]),
            write_pos: 0,
            phases,
        }
    }

    #[inline]
    pub fn process(&mut self, input: f32, pitch_ratio: f64) -> f32 {
        self.buf[self.write_pos & MASK] = input;
        self.write_pos = self.write_pos.wrapping_add(1);

        let inc = (1.0 - pitch_ratio) / GRAIN_SIZE;
        let mut out = 0.0f32;

        for i in 0..NUM_TAPS {
            self.phases[i] += inc;
            // Wrap to [0,1) — must use floor subtraction, not fract()
            // because fract() is negative for negative numbers.
            self.phases[i] -= self.phases[i].floor();

            let delay = self.phases[i] * GRAIN_SIZE + 2.0;
            out += self.lerp_read(delay) * self.hann(self.phases[i]);
        }

        out * NORMALIZE
    }

    #[inline]
    fn lerp_read(&self, delay: f64) -> f32 {
        let rp = self.write_pos as f64 - delay;
        let n = BUF_SIZE as f64;
        let wrapped = ((rp % n) + n) % n;
        let i0 = (wrapped as usize) & MASK;
        let i1 = (i0 + 1) & MASK;
        let frac = (wrapped - wrapped.floor()) as f32;
        self.buf[i0] * (1.0 - frac) + self.buf[i1] * frac
    }

    #[inline]
    fn hann(&self, phase: f64) -> f32 {
        (0.5 * (1.0 - (2.0 * std::f64::consts::PI * phase).cos())) as f32
    }
}
