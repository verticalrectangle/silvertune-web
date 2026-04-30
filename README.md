# Silvertune

Browser-based autotune with a live piano visualization. No backend, no dependencies — one HTML file and one AudioWorklet.

**Live:** [silvertune.live](https://silvertune.live)

---

## What it does

Silvertune takes your microphone input, detects the pitch in real time using a hand-rolled YIN algorithm, snaps it to the nearest note in the selected scale, and pitch-shifts the audio using a 3-tap grain shifter. Output goes directly to your speakers with near-zero latency.

A canvas piano in the center of the screen shows which note is being detected (blue) and which note it's being corrected to (white, pressed). A dashed arc animates between the two when correction is active. Keys outside the current scale are overlaid with a dark mask.

---

## Controls

| Control | What it does |
|---|---|
| **Key** | Root key for the scale |
| **Scale** | Chromatic (no correction) / Major / Minor |
| **Tune** | Correction strength — 0% is dry pitch, 100% is fully snapped |
| **Wide** | Stereo doubler blend |
| **Input / Output** | Audio device selection (Settings tab) |
| **▣** (bottom right) | Minimal mode — hides the canvas visualizer |

---

## Architecture

```
Microphone
    │
    ▼
AudioWorkletNode  (worklet.js — runs on dedicated audio thread)
    │  ├─ YIN pitch detection  (BUF=512, HALF=256, HOP=32)
    │  ├─ Scale quantization
    │  └─ 3-tap grain shifter  (COLA, phases 0 / ⅓ / ⅔, Hann window)
    │
    ├──▶ actx.destination  (live monitor)
    └──▶ MediaStreamDestination  (recording)

Main thread  (index.html)
    ├─ Canvas render loop  (requestAnimationFrame)
    │   ├─ Animated silver/white blob background
    │   ├─ Sonar rings + orb flashes on note events
    │   └─ Piano visualization  (OffscreenCanvas cache for inactive keys)
    └─ XMB-style navigation  (FX · Sound · Rec · Settings)
```

### Pitch detection

YIN runs inline per sample. Every `HOP` (32) samples the circular buffer is shifted and a new CMND difference function is computed. Pitch is estimated via parabolic interpolation on the normalized difference function, then converted to the nearest MIDI note.

### Grain shifter

Three overlapping grains with phases `[0, 1/3, 2/3]` of the grain size (128 samples). Each grain reads from the pitch-shifted position using a Hann window and COLA normalization (×2/3). The pitch ratio is computed from the interval between detected and target note, blended by the Tune parameter.

---

## Recording

**Video + Audio** — captures the canvas + processed audio as WebM using `MediaRecorder`.

**Audio Only** — captures processed audio only via `MediaStreamDestination`.

Files download automatically when you stop recording.

---

## Running locally

```bash
python3 -m http.server 8080
# open http://localhost:8080
```

AudioWorklets require a secure context (HTTPS or localhost).

---

## Deployment

GitHub Actions deploys to GitHub Pages on every push to `master`. Custom domain configured via `CNAME`.
