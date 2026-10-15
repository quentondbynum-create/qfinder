# QFinder

Real-time key + BPM analyzer by **Headroom Audio**.

Insert QFinder on any track: audio passes through untouched while the
background analyzer reports the track's tempo and musical key.

## How it works

- **BPM:** STFT (2048 FFT / 512 hop) → spectral-flux onset envelope →
  autocorrelation → peak-pick in the 60–200 BPM range. The readout only moves
  when the estimate is stable for a smoothing-dependent hold time (1–4 s).
- **Key:** 12-bin chromagram (~4 s decay) → Pearson correlation against
  Krumhansl-Schmuckler major/minor profiles in all 12 rotations, re-evaluated
  ~1/s. Camelot wheel notation shown alongside (e.g. `8A`).

Analysis runs on a background thread fed by a lock-free FIFO — the audio
thread is never blocked.

## UI

- Big BPM readout with confidence bar
- Big key readout with Camelot badge + confidence
- Onset-activity meter, ANALYZE reset button, AUTO toggle, Stability slider
- Preset menu (factory + user + ♥ favorites), Headroom Audio logo mark

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

VST3 lands in `build/QFinder_artefacts/Release/VST3/`.
GitHub Actions builds Mac (AU/VST3, installer DMG) and Windows on every push.
