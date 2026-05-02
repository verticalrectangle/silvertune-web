# silvertune-web

Source for [silvertune.live](https://silvertune.live) — browser-based real-time pitch correction. No install, no backend, one HTML file and one AudioWorklet.

---

## How it works

Microphone input is processed entirely on the audio thread inside an `AudioWorkletProcessor`. The worklet runs YIN pitch detection, quantizes the detected note to the chosen scale, and pitch-shifts the audio through a two-tap grain shifter. Output goes directly to your speakers.

```
Microphone
    │
    ▼
AudioWorkletNode  (worklet.js)
    ├─ YIN pitch detection        BUF=1024, HALF=512, HOP=32
    ├─ Scale quantization
    ├─ Exponential correction curve  coeff = tune² × 0.03
    └─ Two-tap grain shifter      Hann window, phases 0 / 0.5
         │
         ├──▶ actx.destination        (monitor)
         └──▶ MediaStreamDestination  (recording)

Main thread  (index.html)
    ├─ Canvas render loop
    │   ├─ Background animation
    │   ├─ Sonar rings + note flash events
    │   └─ Piano visualization
    └─ XMB-style navigation  (FX · Sound · Rec · Settings)
```

---

## Controls

| Control | Description |
|---------|-------------|
| **Key** | Root key for the scale |
| **Scale** | Major / Minor / Chromatic |
| **Tune** | Correction strength — 0% dry, 100% fully snapped |
| **Wide** | Stereo doubler blend (+8 cents detune) |
| **Input / Output** | Audio device selection (Settings tab) |
| **▣** | Minimal mode — hides the canvas |

---

## Companion app

For lower latency than the browser allows, run the native companion binary before opening the site. It connects via WebSocket on port 2747 and the page detects it automatically.

Download from [Releases](../../releases) or build from source:

```bash
cmake -S companion -B companion/build -DCMAKE_BUILD_TYPE=Release
cmake --build companion/build
./companion/build/silvertune-companion
```

Linux requires `libasound2-dev`.

---

## Running locally

```bash
python3 -m http.server 8080
# open http://localhost:8080
```

AudioWorklets require a secure context (HTTPS or localhost).

---

## Deployment

The [Deploy workflow](.github/workflows/deploy.yml) publishes to GitHub Pages automatically on every push to `master`. Custom domain set via `CNAME`.

### Companion release builds

The [Release Companion workflow](.github/workflows/release-companion.yml) builds native binaries for Linux, macOS (universal), and Windows.

Trigger manually from the Actions tab, or with the GitHub CLI:

```bash
gh workflow run release-companion.yml --field tag=companion-v1.0.0
```

Binaries are published as a GitHub Release at the specified tag.

---

## License

MIT
