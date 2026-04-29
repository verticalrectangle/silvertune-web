const SCALE_INTERVALS = [
  [0,1,2,3,4,5,6,7,8,9,10,11],
  [0,2,4,5,7,9,11],
  [0,2,3,5,7,8,10],
];

function hzToMidi(hz) { return 69 + 12 * Math.log2(hz / 440); }
function midiToHz(m)  { return 440 * Math.pow(2, (m - 69) / 12); }

function quantizeToScale(midiNote, root, scaleIdx) {
  const ivs = SCALE_INTERVALS[scaleIdx];
  let best = midiNote, bestDist = 999;
  const base = Math.floor(midiNote / 12);
  for (let oct = base - 1; oct <= base + 1; oct++) {
    for (const iv of ivs) {
      const c = oct * 12 + root + iv;
      const d = Math.abs(c - midiNote);
      if (d < bestDist) { bestDist = d; best = c; }
    }
  }
  return best;
}

class GrainShifter {
  constructor() {
    this.BUF  = 4096;
    this.MASK = 4095;
    this.buf       = new Float32Array(4096);
    this.writePos  = 0;
    this.phaseA    = 0.0;
    this.phaseB    = 0.5;
    this.grainSize = 1024;
  }

  process(input, pitchRatio) {
    this.buf[this.writePos & this.MASK] = input;
    this.writePos++;

    const inc = (1.0 - pitchRatio) / this.grainSize;
    this.phaseA += inc; this.phaseA -= Math.floor(this.phaseA);
    this.phaseB += inc; this.phaseB -= Math.floor(this.phaseB);

    const sA = this._read(this.phaseA * this.grainSize + 2.0);
    const sB = this._read(this.phaseB * this.grainSize + 2.0);
    return sA * this._hann(this.phaseA) + sB * this._hann(this.phaseB);
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

    this.yinBuf  = new Float32Array(1024);
    this.yinPos  = 0;
    this.diff    = new Float32Array(512);

    this.heldRatio     = 1.0;
    this.detectedNote  = -1;
    this.correctedNote = -1;

    this.tune     = 1.0;
    this.keyIdx   = 0;
    this.scaleIdx = 1;

    this.postCounter = 0;

    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.tune     !== undefined) this.tune     = d.tune;
      if (d.keyIdx   !== undefined) this.keyIdx   = d.keyIdx;
      if (d.scaleIdx !== undefined) this.scaleIdx = d.scaleIdx;
    };
  }

  _yin() {
    const buf = this.yinBuf, diff = this.diff, HALF = 512;
    for (let tau = 0; tau < HALF; tau++) {
      let sum = 0;
      for (let j = 0; j < HALF; j++) { const d = buf[j] - buf[j+tau]; sum += d*d; }
      diff[tau] = sum;
    }
    diff[0] = 1.0;
    let run = 0;
    for (let tau = 1; tau < HALF; tau++) {
      run += diff[tau];
      diff[tau] = diff[tau] * tau / run;
    }
    let tau = 2;
    for (; tau < HALF; tau++) {
      if (diff[tau] < 0.15) {
        while (tau + 1 < HALF && diff[tau+1] < diff[tau]) tau++;
        break;
      }
    }
    if (tau >= HALF) return;

    const s0 = diff[tau-1], s1 = diff[tau], s2 = diff[Math.min(tau+1, HALF-1)];
    const denom = 2*(2*s1 - s2 - s0);
    const shift = Math.abs(denom) > 1e-12 ? (s0-s2)/denom : 0;
    const hz = sampleRate / (tau + shift);
    const conf = Math.max(0, Math.min(1, 1 - s1));

    if (hz > 50 && hz < 2000 && conf > 0.5) {
      const detMidi  = Math.round(hzToMidi(hz));
      const corrMidi = quantizeToScale(detMidi, this.keyIdx, this.scaleIdx);
      const target   = midiToHz(corrMidi);
      let ratio = target / hz;
      ratio = 1.0 + (ratio - 1.0) * this.tune;
      this.heldRatio     = Math.max(0.5, Math.min(2.0, ratio));
      this.detectedNote  = detMidi;
      this.correctedNote = corrMidi;
    }
  }

  process(inputs, outputs) {
    const input = inputs[0]?.[0], output = outputs[0]?.[0];
    if (!input || !output) return true;

    for (let i = 0; i < input.length; i++) {
      this.yinBuf[this.yinPos++] = input[i];
      if (this.yinPos >= 1024) {
        this._yin();
        this.yinBuf.copyWithin(0, 512);
        this.yinPos = 512;
      }
    }

    for (let i = 0; i < input.length; i++)
      output[i] = this.shifter.process(input[i], this.heldRatio);

    this.postCounter += input.length;
    if (this.postCounter >= 2400) {
      this.postCounter = 0;
      let rms = 0;
      for (let i = 0; i < input.length; i++) rms += input[i]*input[i];
      this.port.postMessage({
        detectedNote:  this.detectedNote,
        correctedNote: this.correctedNote,
        correctionRatio: this.heldRatio,
        amplitude: Math.sqrt(rms / input.length),
      });
    }
    return true;
  }
}

registerProcessor('silvertune-processor', SilvertuneProcessor);
