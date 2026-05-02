const SCALE_INTERVALS = [
  [0,1,2,3,4,5,6,7,8,9,10,11],
  [0,2,4,5,7,9,11],
  [0,2,3,5,7,8,10],
];

const DETUNE = 1.00463;

function hzToMidi(hz) { return 69 + 12 * Math.log2(hz / 440); }
function midiToHz(m)  { return 440 * Math.pow(2, (m - 69) / 12); }

class FormantPreserver {
  constructor() {
    this.ORDER = 16;
    this.WIN   = 512;
    this.HOP   = 128;
    this.winBuf   = new Float32Array(512);
    this.winPos   = 0;
    this.hopCount = 0;
    this.a        = new Float32Array(16);
    this.xMem     = new Float32Array(16);
    this.yMem     = new Float32Array(16);
  }

  reset() {
    this.winBuf.fill(0); this.a.fill(0);
    this.xMem.fill(0); this.yMem.fill(0);
    this.winPos = 0; this.hopCount = 0;
  }

  _updateLpc() {
    const { ORDER, WIN } = this;
    const r = new Float32Array(ORDER + 1);
    for (let lag = 0; lag <= ORDER; lag++) {
      let sum = 0;
      for (let i = 0; i < WIN - lag; i++) {
        const i0 = (this.winPos - 1 - i + WIN * 2) % WIN;
        const i1 = (this.winPos - 1 - i - lag + WIN * 2) % WIN;
        sum += this.winBuf[i0] * this.winBuf[i1];
      }
      r[lag] = sum;
    }
    if (r[0] < 1e-10) { this.a.fill(0); return; }
    const tmp = new Float32Array(ORDER);
    let E = r[0];
    for (let m = 0; m < ORDER; m++) {
      let kmNum = -r[m + 1];
      for (let j = 0; j < m; j++) kmNum -= tmp[j] * r[m - j];
      let km = kmNum / E;
      if (km >  0.9999) km =  0.9999;
      if (km < -0.9999) km = -0.9999;
      const newTmp = tmp.slice();
      for (let j = 0; j < m; j++) newTmp[j] = tmp[j] + km * tmp[m - 1 - j];
      newTmp[m] = km;
      tmp.set(newTmp);
      E *= (1 - km * km);
      if (E < 1e-20) break;
    }
    this.a.set(tmp);
  }

  analyze(x) {
    this.winBuf[this.winPos] = x;
    this.winPos = (this.winPos + 1) % this.WIN;
    if (++this.hopCount >= this.HOP) { this.hopCount = 0; this._updateLpc(); }
    let e = x;
    for (let k = 0; k < this.ORDER; k++) e += this.a[k] * this.xMem[k];
    this.xMem.copyWithin(1, 0, this.ORDER - 1);
    this.xMem[0] = x;
    return e;
  }

  synthesize(e) {
    let y = e;
    for (let k = 0; k < this.ORDER; k++) y -= this.a[k] * this.yMem[k];
    this.yMem.copyWithin(1, 0, this.ORDER - 1);
    this.yMem[0] = y;
    return y;
  }
}

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

    this.formant   = new FormantPreserver();
    this.vibratoLpHz = 0.0;

    this.heldChord1    = 1.0;
    this.currentChord1 = 1.0;
    this.heldChord2    = 1.0;
    this.currentChord2 = 1.0;
    this.chord1    = new GrainShifter();
    this.chord1dbl = new GrainShifter();
    this.chord2    = new GrainShifter();
    this.chord2dbl = new GrainShifter();

    this.formantOn = false;
    this.vibratoOn = false;
    this.chordOn   = false;

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
      if (d.formantOn !== undefined) this.formantOn = d.formantOn;
      if (d.vibratoOn !== undefined) this.vibratoOn = d.vibratoOn;
      if (d.chordOn   !== undefined) this.chordOn   = d.chordOn;
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

    if (hz > 20) {
      if (this.vibratoLpHz === 0) this.vibratoLpHz = hz;
      const centsDiff = Math.abs(hzToMidi(hz) - hzToMidi(this.vibratoLpHz)) * 100;
      if (centsDiff > 80) this.vibratoLpHz = hz;
      else this.vibratoLpHz += 0.2 * (hz - this.vibratoLpHz);
    }
    const useHz = (this.vibratoOn && this.vibratoLpHz > 0) ? this.vibratoLpHz : hz;

    if (this.tune < 0.01) {
      this.heldRatio = 1.0;
    } else if (useHz > 80 && useHz < 2000 && conf > 0.5) {
      const detMidi = hzToMidi(useHz);
      const staysLocked = this.lockedMidi >= 0 && Math.abs(detMidi - this.lockedMidi) < 0.4;
      if (!staysLocked) this.lockedMidi = detMidi;
      this.lowConfCount = 0;
      const corrMidi = quantizeToScale(Math.round(this.lockedMidi), this.keyIdx, this.scaleIdx);
      let ratio = midiToHz(corrMidi) / useHz;
      ratio = 1.0 + (ratio - 1.0) * this.tune;
      this.heldRatio     = Math.max(0.5, Math.min(2.0, ratio));
      this.detectedNote  = Math.round(detMidi);
      this.correctedNote = corrMidi;
      if (this.chordOn) {
        const c1Hz = midiToHz(corrMidi + 7);
        const c2Hz = midiToHz(corrMidi - 5);
        this.heldChord1 = Math.max(0.5, Math.min(2.0, c1Hz / useHz));
        this.heldChord2 = Math.max(0.5, Math.min(2.0, c2Hz / useHz));
      }
    } else if (conf < 0.35) {
      if (++this.lowConfCount >= 3) {
        this.lockedMidi    = -1.0;
        this.lowConfCount  = 0;
        this.heldRatio     = 1.0;
        this.currentRatio  = 1.0;
        this.heldChord1    = 1.0;
        this.currentChord1 = 1.0;
        this.heldChord2    = 1.0;
        this.currentChord2 = 1.0;
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
        this.currentRatio  += (this.heldRatio  - this.currentRatio)  * coeff;
        this.currentChord1 += (this.heldChord1 - this.currentChord1) * coeff;
        this.currentChord2 += (this.heldChord2 - this.currentChord2) * coeff;
        const src = this.formantOn ? this.formant.analyze(s) : s;
        let wet = this.shifter.process(src, this.currentRatio);
        const dbl = this.doubler.process(src, this.currentRatio * DETUNE);
        let out = wet + dbl * this.wide;
        if (this.formantOn) out = this.formant.synthesize(out);
        if (this.chordOn) {
          const c1  = this.chord1.process(src, this.currentChord1);
          const c1d = this.chord1dbl.process(src, this.currentChord1 * DETUNE);
          const c2  = this.chord2.process(src, this.currentChord2);
          const c2d = this.chord2dbl.process(src, this.currentChord2 * DETUNE);
          let c1out = c1 + c1d * this.wide;
          let c2out = c2 + c2d * this.wide;
          if (this.formantOn) {
            c1out = this.formant.synthesize(c1out);
            c2out = this.formant.synthesize(c2out);
          }
          out = out * 0.6 + c1out * 0.25 + c2out * 0.25;
        }
        const processed = out * this.volume;
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
