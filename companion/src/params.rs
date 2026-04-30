use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;

#[derive(Default)]
pub struct AtomicParams {
    pub key:   AtomicU32,  // 0–11
    pub scale: AtomicU32,  // 0–2
    tune:  AtomicU32,      // f32 bits, 0.0–1.0
    wide:  AtomicU32,      // f32 bits, 0.0–1.0
}

impl AtomicParams {
    pub fn tune(&self) -> f32 {
        f32::from_bits(self.tune.load(Ordering::Relaxed))
    }
    pub fn wide(&self) -> f32 {
        f32::from_bits(self.wide.load(Ordering::Relaxed))
    }
    pub fn set_tune(&self, v: f32) {
        self.tune.store(v.to_bits(), Ordering::Relaxed);
    }
    pub fn set_wide(&self, v: f32) {
        self.wide.store(v.to_bits(), Ordering::Relaxed);
    }
}

pub type SharedParams = Arc<AtomicParams>;
