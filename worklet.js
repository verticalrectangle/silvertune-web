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
    this.phaseA    = 0.0;
    this.phaseB    = 0.5;  // 2-tap: sum of Hann at 0.5 offset = 1.0, no normalisation needed
    this.grainSize = 256;
  }

  resetPhases() { this.phaseA = 0.0; this.phaseB = 0.5; }

  process(input, pitchRatio) {
    this.buf[this.writePos & this.MASK] = input;
    this.writePos++;
    const inc = (1.0 - pitchRatio) / this.grainSize;
    this.phaseA += inc; this.phaseA -= Math.floor(this.phaseA);
    this.phaseB += inc; this.phaseB -= Math.floor(this.phaseB);
    return this._read(this.phaseA * this.grainSize + 2.0) * this._hann(this.phaseA)
         + this._read(this.phaseB * this.grainSize + 2.0) * this._hann(this.phaseB);
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

class SilvertuneProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.shifter = new GrainShifter();
    this.doubler = new GrainShifter();

    this.YIN_BUF  = 1024;
    this.YIN_HALF = 512;
    this.YIN_HOP  = 32;
    this.yinBuf   = new Float32Array(1024);
    this.diff     = new Float32Array(512);
    this.yinPos   = 0;

    this.heldRatio    = 1.0;
    this.currentRatio = 1.0;
    this.lockedMidi    = -1.0;
    this.lowConfCount  = 0;
    this.detectedNote  = -1;
    this.correctedNote = -1;

    this.tune     = 1.0;
    this.wide     = 0.0;
    this.volume   = 1.0;
    this.gain     = 1.0;
    this.keyIdx   = 0;
    this.scaleIdx = 1;
    this.bypass   = false;

    this.postCounter = 0;
    this.POST_EVERY  = 600;
    this.rmsAcc      = 0;
    this.rmsCount    = 0;

    this.prePrev    = 0.0;
    this.yinEnergy  = 1e-6;
    this.YIN_TARGET = 0.08;
    this.YIN_MAX_G  = 80.0;

    this.gateEnergy = 1e-6;
    this.GATE_THRESH = 2e-5;
    this.gateGain   = 0.0;

    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.tune     !== undefined) this.tune     = d.tune;
      if (d.wide     !== undefined) this.wide     = d.wide;
      if (d.volume   !== undefined) this.volume   = d.volume;
      if (d.gain     !== undefined) this.gain     = d.gain;
      if (d.keyIdx   !== undefined) this.keyIdx   = d.keyIdx;
      if (d.scaleIdx !== undefined) this.scaleIdx = d.scaleIdx;
      if (d.bypass   !== undefined) this.bypass   = d.bypass;
    };
  }

  _yin() {
    const buf = this.yinBuf, diff = this.diff, HALF = this.YIN_HALF;
    for (let tau = 0; tau < HALF; tau++) {
      let sum = 0;
      for (let j = 0; j < HALF; j++) { const d = buf[j]-buf[j+tau]; sum += d*d; }
      diff[tau] = sum;
    }
    diff[0] = 1.0;
    let run = 0;
    for (let tau = 1; tau < HALF; tau++) { run += diff[tau]; diff[tau] = diff[tau]*tau/run; }
    let tau = 2;
    for (; tau < HALF; tau++) {
      if (diff[tau] < 0.15) { while (tau+1 < HALF && diff[tau+1] < diff[tau]) tau++; break; }
    }
    if (tau >= HALF) return;
    const s0=diff[tau-1], s1=diff[tau], s2=diff[Math.min(tau+1,HALF-1)];
    const denom = 2*(2*s1-s2-s0);
    const shift = Math.abs(denom) > 1e-12 ? (s0-s2)/denom : 0;
    const hz = sampleRate / (tau + shift);
    const conf = Math.max(0, Math.min(1, 1-s1));
    if (this.tune < 0.01) {
      this.heldRatio = 1.0;
    } else if (hz > 80 && hz < 2000 && conf > 0.5) {
      const detMidi = hzToMidi(hz);
      const staysLocked = this.lockedMidi >= 0 && Math.abs(detMidi - this.lockedMidi) < 0.4;
      if (!staysLocked) this.lockedMidi = detMidi;
      this.lowConfCount = 0;
      const corrMidi = quantizeToScale(Math.round(this.lockedMidi), this.keyIdx, this.scaleIdx);
      let ratio = midiToHz(corrMidi) / hz;
      ratio = 1.0 + (ratio - 1.0) * this.tune;
      this.heldRatio     = Math.max(0.5, Math.min(2.0, ratio));
      this.detectedNote  = Math.round(detMidi);
      this.correctedNote = corrMidi;
    } else if (conf < 0.35) {
      if (++this.lowConfCount >= 3) {
        this.lockedMidi   = -1.0;
        this.lowConfCount = 0;
        this.heldRatio    = 1.0;
        this.currentRatio = 1.0;
      }
    }
  }

  process(inputs, outputs) {
    const input = inputs[0]?.[0], output = outputs[0]?.[0];
    if (!input || !output) return true;

    for (let i = 0; i < input.length; i++) {
      const s = input[i] * this.gain;
      this.gateEnergy = 0.999 * this.gateEnergy + 0.001 * s * s;
      const gateOpen = this.gateEnergy > this.GATE_THRESH ? 1.0 : 0.0;
      this.gateGain += (gateOpen > this.gateGain ? 0.0005 : 0.0001) * (gateOpen - this.gateGain);

      const sPre = s - 0.95 * this.prePrev;
      this.prePrev = s;
      this.yinEnergy = 0.999 * this.yinEnergy + 0.001 * sPre * sPre;
      const normG = Math.min(this.YIN_MAX_G, this.YIN_TARGET / Math.sqrt(this.yinEnergy));
      this.yinBuf[this.yinPos++] = sPre * normG;
      if (this.yinPos >= this.YIN_BUF) {
        this._yin();
        this.yinBuf.copyWithin(0, this.YIN_HOP);
        this.yinPos = this.YIN_BUF - this.YIN_HOP;
      }
      this.rmsAcc += s * s;
      this.rmsCount++;

      if (this.bypass) {
        output[i] = s * this.volume;
      } else {
        const coeff = this.tune >= 1.0 ? 1.0 : this.tune * this.tune * 0.03;
        this.currentRatio += (this.heldRatio - this.currentRatio) * coeff;
        const wet = this.shifter.process(s, this.currentRatio);
        const dbl = this.doubler.process(s, this.currentRatio * DETUNE);
        const processed = (wet + dbl * this.wide) * this.volume;
        output[i] = processed * this.gateGain + s * this.volume * (1.0 - this.gateGain);
      }
    }

    this.postCounter += input.length;
    if (this.postCounter >= this.POST_EVERY) {
      this.postCounter = 0;
      const rms = Math.sqrt(this.rmsAcc / Math.max(1, this.rmsCount));
      this.rmsAcc = 0; this.rmsCount = 0;
      this.port.postMessage({
        detectedNote:  this.detectedNote,
        correctedNote: this.correctedNote,
        rms,
      });
    }

    return true;
  }
}

registerProcessor('silvertune-processor', SilvertuneProcessor);
