// Audio thread: grain shift only. YIN runs on main thread (same split as yin.cpp / audio_callback).

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
    this.shifter   = new GrainShifter();
    this.heldRatio = 1.0;

    // accumulate samples to send to main thread for YIN
    this.hopBuf = new Float32Array(512);
    this.hopPos = 0;

    this.port.onmessage = (e) => {
      if (e.data.ratio !== undefined) this.heldRatio = e.data.ratio;
    };
  }

  process(inputs, outputs) {
    const input = inputs[0]?.[0], output = outputs[0]?.[0];
    if (!input || !output) return true;

    for (let i = 0; i < input.length; i++) {
      // grain shift — only work on audio thread
      output[i] = this.shifter.process(input[i], this.heldRatio);

      // accumulate for YIN on main thread
      this.hopBuf[this.hopPos++] = input[i];
      if (this.hopPos >= 512) {
        const transfer = this.hopBuf.buffer.slice(0);
        this.port.postMessage({ samples: transfer }, [transfer]);
        this.hopPos = 0;
      }
    }

    return true;
  }
}

registerProcessor('silvertune-processor', SilvertuneProcessor);
