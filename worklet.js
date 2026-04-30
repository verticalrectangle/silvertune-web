const SCALE_INTERVALS = [
  [0,1,2,3,4,5,6,7,8,9,10,11],
  [0,2,4,5,7,9,11],
  [0,2,3,5,7,8,10],
];

const DETUNE = 1.00463;

function hzToMidi(hz) { return 69 + 12 * Math.log2(hz / 440); }
function midiToHz(m)  { return 440 * Math.pow(2, (m - 69) / 12); }

function quantizeToScale(midiNote, root, si) {
  const ivs = SCALE_INTERVALS[si];
  let best = midiNote, bestDist = 999;
  const base = Math.floor(midiNote / 12);
  for (let oct = base - 1; oct <= base + 1; oct++)
    for (const iv of ivs) { const c = oct*12+root+iv, d = Math.abs(c-midiNote); if (d < bestDist) { bestDist=d; best=c; } }
  return best;
}

class GrainShifter {
  constructor() {
    this.BUF  = 4096;
    this.MASK = 4095;
    this.buf       = new Float32Array(4096);
    this.writePos  = 0;
    this.phases    = [0.0, 1/3, 2/3]; // 3-tap COLA: sum of Hann at 1/3 offset = 1.5
    this.grainSize = 128;
  }

  process(input, pitchRatio) {
    this.buf[this.writePos & this.MASK] = input;
    this.writePos++;
    const inc = (1.0 - pitchRatio) / this.grainSize;
    let out = 0;
    for (let i = 0; i < 3; i++) {
      this.phases[i] += inc;
      this.phases[i] -= Math.floor(this.phases[i]);
      out += this._read(this.phases[i] * this.grainSize + 2.0) * this._hann(this.phases[i]);
    }
    return out * (2 / 3);
  }

  _read(delay) {
    let rp = ((this.writePos - delay) % this.BUF + this.BUF) % this.BUF;
    const i0 = Math.floor(rp) & this.MASK;
    const i1 = (i0 + 1) & this.MASK;
    const f  = rp - Math.floor(rp);
    return this.buf[i0] * (1 - f) + this.buf[i1] * f;
  }

  _hann(p) { return 0.5 * (1 - Math.cos(2 * Math.PI * p)); }
}

// Shared YIN runner — works on any buf/diff pair
function runYin(buf, diff, HALF, sr) {
  for (let tau = 0; tau < HALF; tau++) {
    let sum = 0;
    for (let j = 0; j < HALF; j++) { const d = buf[j] - buf[j+tau]; sum += d*d; }
    diff[tau] = sum;
  }
  diff[0] = 1.0;
  let run = 0;
  for (let tau = 1; tau < HALF; tau++) { run += diff[tau]; diff[tau] = diff[tau]*tau/run; }
  let tau = 2;
  for (; tau < HALF; tau++) {
    if (diff[tau] < 0.15) { while (tau+1 < HALF && diff[tau+1] < diff[tau]) tau++; break; }
  }
  if (tau >= HALF) return null;
  const s0=diff[tau-1], s1=diff[tau], s2=diff[Math.min(tau+1,HALF-1)];
  const denom = 2*(2*s1-s2-s0);
  const refined = tau + (Math.abs(denom) > 1e-12 ? (s0-s2)/denom : 0);
  return { hz: sr / refined, conf: Math.max(0, Math.min(1, 1 - s1)) };
}

class SilvertuneProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.shifter = new GrainShifter();
    this.doubler = new GrainShifter();

    // Full YIN — 512 samples, detects down to ~187Hz (covers all voices)
    this.YIN_BUF  = 512; this.YIN_HALF = 256; this.YIN_HOP = 32;
    this.yinBuf   = new Float32Array(512);
    this.diff     = new Float32Array(256);
    this.yinPos   = 0;

    // Fast YIN — 256 samples, detects down to ~375Hz (soprano/tenor/alto)
    // Half the window = half the onset detection latency for high voices
    this.FAST_BUF  = 256; this.FAST_HALF = 128; this.FAST_HOP = 32;
    this.fastBuf   = new Float32Array(256);
    this.fastDiff  = new Float32Array(128);
    this.fastPos   = 0;

    this.heldRatio     = 1.0;
    this.targetRatio   = 1.0;
    this.detectedNote  = -1;
    this.correctedNote = -1;

    this.tune = 1.0; this.wide = 0.0; this.keyIdx = 0; this.scaleIdx = 1; this.bypass = false;

    this.postCounter = 0; this.POST_EVERY = 600;
    this.rmsAcc = 0; this.rmsCount = 0;

    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.tune     !== undefined) this.tune     = d.tune;
      if (d.wide     !== undefined) this.wide     = d.wide;
      if (d.keyIdx   !== undefined) this.keyIdx   = d.keyIdx;
      if (d.scaleIdx !== undefined) this.scaleIdx = d.scaleIdx;
      if (d.bypass   !== undefined) this.bypass   = d.bypass;
    };
  }

  _applyDetection(hz, conf, minHz, minConf) {
    if (hz < minHz || hz > 2000 || conf < minConf) return false;
    const detMidi  = Math.round(hzToMidi(hz));
    const corrMidi = quantizeToScale(detMidi, this.keyIdx, this.scaleIdx);
    let ratio = midiToHz(corrMidi) / hz;
    ratio = 1.0 + (ratio - 1.0) * this.tune;
    this.targetRatio   = Math.max(0.5, Math.min(2.0, ratio));
    this.detectedNote  = detMidi;
    this.correctedNote = corrMidi;
    return true;
  }

  process(inputs, outputs) {
    const input = inputs[0]?.[0], output = outputs[0]?.[0];
    if (!input || !output) return true;

    for (let i = 0; i < input.length; i++) {
      const s = input[i];

      // Full YIN — low-priority, handles bass/baritone/low tenor (>187Hz)
      this.yinBuf[this.yinPos++] = s;
      if (this.yinPos >= this.YIN_BUF) {
        const r = runYin(this.yinBuf, this.diff, this.YIN_HALF, sampleRate);
        if (r) this._applyDetection(r.hz, r.conf, 80, 0.35);
        this.yinBuf.copyWithin(0, this.YIN_HOP);
        this.yinPos = this.YIN_BUF - this.YIN_HOP;
      }

      // Fast YIN — high-priority, overwrites full YIN for voices >375Hz
      // Half the window (5.3ms vs 10.7ms) = locks on to new notes twice as fast
      this.fastBuf[this.fastPos++] = s;
      if (this.fastPos >= this.FAST_BUF) {
        const r = runYin(this.fastBuf, this.fastDiff, this.FAST_HALF, sampleRate);
        if (r) this._applyDetection(r.hz, r.conf, 375, 0.5);
        this.fastBuf.copyWithin(0, this.FAST_HOP);
        this.fastPos = this.FAST_BUF - this.FAST_HOP;
      }

      this.rmsAcc += s * s;
      this.rmsCount++;

      if (this.bypass) {
        output[i] = s;
      } else {
        // Snap toward target — fast glide on large interval jumps, instant on small
        const delta = this.targetRatio - this.heldRatio;
        this.heldRatio += delta * (Math.abs(delta) > 0.03 ? 0.25 : 1.0);

        const wet = this.shifter.process(s, this.heldRatio);
        const dbl = this.doubler.process(s, this.heldRatio * DETUNE);
        output[i] = wet * 0.95 + dbl * this.wide + s * 0.05;
      }
    }

    this.postCounter += input.length;
    if (this.postCounter >= this.POST_EVERY) {
      this.postCounter = 0;
      const rms = Math.sqrt(this.rmsAcc / Math.max(1, this.rmsCount));
      this.rmsAcc = 0; this.rmsCount = 0;
      this.port.postMessage({ detectedNote: this.detectedNote, correctedNote: this.correctedNote, rms });
    }

    return true;
  }
}

registerProcessor('silvertune-processor', SilvertuneProcessor);
