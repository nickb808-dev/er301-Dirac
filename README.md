# Dirac

A **granular synthesizer** for the ER-301 named after [Paul Dirac](https://en.wikipedia.org/wiki/Paul_Dirac). Dirac chops a sound into short overlapping grains, each read straight from
the audio, windowed, pitched by playback speed, and layered into clouds.
It runs on a **live input** by default and switches to granulating a **loaded
sample** the moment you select one from the card.

Unit title: **Dirac** · mnemonic **Di** · stereo out · current version **0.1.17**

---

## How it works

Each grain reads a short window of the source with linear interpolation, multiplies
it by an envelope, and plays it back — at a different speed to transpose. Many grains
overlap into a continuous texture. Because it's time-domain, pitch shifting moves
formants (the characteristic granular shimmer), transients stay sharp, and grains
cost almost nothing — long grains and dense clouds are cheap, no per-grain buffers.

**Two source modes (automatic):**

- **Live** (no sample loaded) — Dirac captures the input into a ~2.7 s rolling ring
  buffer and granulates that. This is where **Feedback**, **Mix**, and the buffer
  scan live.
- **Sample** (select one from the menu) — Dirac granulates the loaded sample at a
  movable **Playhead**, drawn on the waveform display.

---

## Quick start

1. Add **Dirac**, patch in audio. You'll hear a cloud immediately — **Density**
   defaults to 3 and free-runs, so grains spawn on their own.
2. **gLen** sets grain size and **grains** sets how many overlap: short + sparse is
   pointillistic; long + dense is a smooth wash.
3. **Texture** shapes each grain: turn it **down** for crisp percussive "bits," leave
   it centered for a rounded Hann, turn it **up** for a fuller flat-top grain.
4. In live mode, **fdbk** (Feedback) is the magic knob — turn it up and the cloud
   regenerates and sustains after the input stops (drones, smears, infinite textures).
   **playh** sets the feedback delay / how far back in the buffer grains read.
5. Load a sample from the menu to granulate a recording; sweep **playh** through it.
6. Patch a gate into **trig** for rhythmic grains, or set **Density** to 0 for
   trigger-only.

---

## Controls

Spawning is **density-driven** (free-run) plus **trigger** edges, decoupled — both
can run at once.

| Btn | Inlet | Range / default | What it does |
|---|---|---|---|
| trig | Fire | gate | Spawns one grain per rising edge (manual button or patched gate). |
| hold | Hold | toggle | Freezes the current grains into a looping, frozen cloud. |
| playh | Playhead | 0–1 / 0 | **Sample:** read position. **Live:** how far back in the buffer grains read — also the **feedback delay** (0 = most recent / tight, 1 = oldest / long echo). |
| dens | Rate | 0–16 / 3 | **Density** — target grain overlap (how many deep). 0 = trigger-only (free-run off). |
| gLen | GrainLen | 1 ms – 1 s / 50 ms | Grain length. |
| grains | Grains | 1–16 / 12 | Polyphony cap — how many grains may overlap at once. |
| semi | SemiShift | −24…+24 st / 0 | Coarse transpose knob (integer semitones, playback-speed pitch). |
| V/oct | V/Oct | 1 V/oct / 0 | Continuous pitch CV — patch a 1 V/oct source here; summed with SemiShift, clamped ±4 octaves. |
| psprd | Psprd | 0–12 st / 0 | Per-grain pitch scatter. |
| scale | Scale | 0–6 / 0 | Quantize the Psprd scatter to a scale: 0 off · 1 chromatic · 2 major · 3 minor · 4 penta-maj · 5 penta-min · 6 whole-tone. |
| detun | Detune | 0–0.5 st / 0 | Stereo detune — R read pitched vs L for width. |
| sprd | Spread | 0–1 / 0.5 | Stereo position scatter across grains (also the binaural **width**). |
| posJtr | PosJtr | 0–1 / 0 | Random spray of each grain's read position (±100 ms). |
| rev | RevProb | 0–1 / 0 | Probability a grain plays in reverse. |
| textr | Texture | 0–1 / 0.5 | Grain envelope shape: **Percussive (0) → Hann (0.5) → Tukey (1)**. |
| comp | Compress | 0–1 / 0 | **Per-grain leveling** — boosts quiet source bits / tames loud ones (±12 dB) so sparse clouds stay even. |
| bin | Binaural | 0–1 / 0 | **3D depth** — adds interaural time difference (ITD) + head-shadow filtering on top of the pan, scaled by Spread. 0 = plain stereo. |
| level | Level | 0–1 / 0.7 | Output level. |
| mix | Mix | 0–1 / 1 | Wet/dry — **live only**. 1 = grains, 0 = dry input through. Inert with a sample loaded. |
| **fdbk** | **Feedback** | **0–1 / 0** | **Feedback — reinjects output into the buffer so the cloud regenerates and sustains. Live-mode; tanh soft-clipped so it saturates into a stable drone instead of running away. ~0.8 = clear tail, ~1 = held drone.** |

### Sample menu

Press the unit's sub-display button for: **Select from Card**, **Select from Pool**,
**Detach Buffer** (return to live input), **Edit Buffer**.

---

## Feature notes

**Feedback (live).** Grains read just *behind* the write head, so they pick up the
freshly fed-back output and regenerate. Playhead is the delay time. A makeup stage
inside the soft-clipper lets Feedback approach unity loop gain near 1.0 while `tanh`
keeps it bounded — push it for evolving drones and infinite smears.

**Binaural.** Each grain's pan position becomes an azimuth: the far ear's read is
delayed by up to ±31 samples (ITD) and rolled off by a one-pole head-shadow lowpass
(20 kHz → 4 kHz toward the side). **Spread** sets the width, **Binaural** the depth;
at 0 it's bit-identical plain stereo.

**Compression.** At spawn, Dirac peeks the source where each grain will read and
applies a makeup gain toward a target — so quiet passages still speak in sparse
clouds without making loud ones clip.

**Visualizer.** The grain-field display draws each grain as an upward **triangle** —
a Dirac-comb impulse — at its stereo position (horizontal) and pitch (vertical); the
triangle pulses with the grain's envelope and leaves a phosphor afterglow as grains
decay. (Impulse triangles are Dirac's signature.)

---

## Build

```bash
cd ~/Documents/Claude/dirac
make dist ER301_SDK=~/er-301      # → build/am335x/dirac-<version>.pkg
```

Requires Docker (cross-compiles for the AM3358 / Cortex-A8). Bump `VERSION` in the
`Makefile` **and** `assets/toc.lua` on every flash — the firmware caches the `.so`
per version.

### Host test harness

`test/host/` holds stub `od::` headers and a harness that runs the DSP on your
computer (no hardware needed) — used to verify that optimizations are bit-identical,
to fuzz long-grain / extreme-pitch edge cases under ASan/UBSan, and to measure
seam-fade click levels:

```bash
g++ -std=c++11 -O2 -ffast-math -Itest/host -Isrc src/Dirac.cpp test/host/main.cpp -o t
./t ident out.raw   # deterministic renders (bit-exactness comparisons)
./t asan            # torture configs (build with -std=c++17 -fsanitize=address,undefined)
./t seam <label>    # R-channel click metric at a sample loop boundary
```

---

## Changelog (recent)

- **0.1.17** — Code-review pass. Two safety fixes: guarded a rare out-of-bounds
  envelope-table read on very long grains (float accumulator drift), and the seam
  edge-fade now tracks the **right-channel read independently** — with Detune the R
  read drifts far from L and could cross a loop boundary or the live write head
  unfaded (right-channel click, now gone). CPU: source-read state hoisted out of the
  per-sample loop, feedback path fully gated off in sample mode, power-of-two mask on
  the ring write, envelope blend skips a table lerp at Texture nodes (incl. the 0.5
  default). All CPU changes verified **bit-identical**; ASan/UBSan clean. Added the
  host test harness (`test/host/`).
- **0.1.16** — Micro-opts: binaural head-shadow filter skipped when Binaural is 0;
  faster pitch-increment math (`expf` instead of `powf`).
- **0.1.15** — **V/Oct** input: continuous 1 V/oct pitch CV, summed with SemiShift,
  clamped ±4 octaves.
- **0.1.14** — **Scale**: quantizes the Psprd pitch scatter to a musical scale
  (chromatic / major / minor / penta / whole-tone) so clouds scatter musically.
- **0.1.13** — Click-free Texture modulation: three static envelope tables, each
  grain captures its shape at spawn — moving Texture never disturbs a playing grain.

---

## Design notes

- **Time-domain, incremental render.** Grains are rendered a few samples per block as
  they play (never pre-rendered all at once), so there are no spawn CPU spikes and
  long grains cost no extra memory.
- **No voice stealing.** At the Grains cap, new spawns are skipped rather than
  hard-cutting a playing grain (which would click); density self-limits.
- **Click-safe.** Envelopes are zero-ended at both ends; an edge-fade guards the
  live read/write collision and sample-loop boundary; all output is finiteness-
  scrubbed (NaN/Inf-proof) and soft-limited so dense clouds never hard-clip the DAC.
