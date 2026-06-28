# memory.md — dirac

> **For Claude: read this, then `src/Dirac.h` and `assets/Dirac.lua` before working.**

## What this is

`dirac` is a **classic time-domain granular synth** — the Beads/Clouds approach.
It's a sibling of **Planck** (spectral) and **Bohr** (spectral + binaural), forked
from Planck's shell but with a **completely different engine**: no FFT. Grains are
read straight from the source with linear interpolation, windowed, pitched by
playback speed, and overlap-added.

Named for the **Dirac comb** (Ш) — a train of impulses = a grain stream. Unit
title **Dirac**, mnemonic **Di**. Class names (Dirac / DiracHeadDisplay /
DiracFieldGraphic) are unique vs Planck/Bohr, so all three install together.

## Engine (time-domain, ported in spirit from Whirlpool's render)

- **Source (dual, automatic, same as Planck):** static `od::Sample` at the
  Playhead, OR the live capture ring when no sample is attached.
- **Grain render** (`process()` accumulation loop): per grain, per sample —
  `env = mEnvTable[envPhase]`; `s = readSrc(readPos)` (linear interp); `out +=
  gain·env·s`; advance `readPos += pitchIncr` (negative if reverse), `envPhase
  += envIncr`. No per-grain buffers, no FFT.
- **Pitch = playback speed**: `pitchIncr = 2^(semis/12)` (chord voice + SemiShift
  + Psprd + HoldDetune). Detune gives R a separate increment. Classic granular
  formant-shifting pitch. Host-verified exact: semi 0→1000 Hz, +12→2000, −12→480.
- **Envelope**: `mEnvTable[kEnvTableSize=1024]` read by phase (`envIncr =
  1024/duration`), so any grain length works with no storage. Morphs Hann →
  Tukey (Texture) → FOF (Fog); rebuilt only when Texture/Fog change (cached).
- **Pan**: equal-power, centre = unity (`gainL=√(1−pan)`, `gainR=√(1+pan)`),
  pan = per-grain random × Spread. Separate R read only when Detune > 0.
- **Density/level**: `scale = level · densComp / √voices`, `densComp =
  1/√overlap`.
- **Chord**: voices = separate grains at the chord intervals + the v0.7.9
  decorrelation lessons (onset stagger 0..10 ms when nVoices>1; time-domain
  voices are inherently decorrelated by differing playback speeds).
- **Spawn**: decoupled free-run at Rate + trig edges (same model as Planck v0.7.4).

## Feedback (the headline addition vs Planck)

`Feedback` [0,1] reinjects output into the capture ring: `ring[w] = input +
feedback · tanh(wetMono_prev)`. Grains then re-granulate previous output →
regenerating clouds / smears / drones (classic Beads). The **tanh soft-clip on
the feedback path keeps it stable** (no runaway) and adds gentle saturation.
Host-verified: tail energy grows monotonically with feedback (0→silent,
0.95→long decay), stable. **Live-mode feature** (grains read the ring); inert in
sample mode (grains read the static sample) — same scoping as Mix. Sample-mode
feedback is a possible v2 (blend sample + feedback ring).

## Controls vs Planck

- **Added**: `feedback` ("fdbk").
- **Dropped (spectral-only)**: Shift, Smear, Lag. Also dropped the **MaxGrainLen
  menu option** — time-domain grains have no per-grain buffers, so grain length
  is free (GrainLen 1 ms–1 s; engine ceiling kMaxGrainSamp = 2 s). No
  `allocGrainBuffers`, no `od::Option`.
- **Kept**: Playhead, Rate, Grains, GrainLen, PosJtr, Spread, Detune, Chord,
  Psprd, Semitone, HoldDetune, Texture, Fog, Reverse, Hold, Trig, Level, Mix.
- Viz (v0.1.1): **DiracFieldGraphic now draws upward triangles** (Dirac-comb
  impulses) instead of Planck's dots/capsules — `triSplat()` filled triangle, size
  from envelope (+ a little grain length, cap kMaxTri=5), X = pan, Y = pitch, phosphor
  afterglow kept. Also fixed a pan double-apply (panPos is already ×Spread; the graphic
  no longer multiplies by Spread again). This is Dirac's signature look vs Planck/Bohr.

## Changes log

- **v0.1.12 — Binaural (ITD + head-shadow), ported from Whirlpool.** New **Binaural** inlet [0,1] = 3D depth.
  Per grain: azimuth = pan·90° (pan = Spread-scattered position). ITD = ±31 samples (kMaxITDSamples) × sin(az)
  × binaural, applied as a read-position offset on the FAR ear (delayL/delayR off startPos). Head shadow =
  one-pole LP on the far ear, fc 20 kHz→4 kHz with |sin(az)|·binaural (lpAlpha = 1−exp(−2π fc/SR), set on the
  contralateral ear; near ear alpha=1 = transparent). LP runs every grain but is a no-op at alpha=1, so
  binaural=0 is bit-identical plain stereo. Grain gained 4 fields (lpStateL/R, lpAlphaL/R); engages the
  separate R read (g.stereo=true). Host: L/R decorrelation ×1.64 at binaural 1, level preserved, 0 non-finite,
  ASan/UBSan clean, Lua/inlet ports consistent. Spread = width, Binaural = depth.
- **v0.1.11 — Merged DiracFB's feedback redesign (user A/B'd it, preferred).** Two changes from the diracfb
  fork: (1) **Live read anchor moved BEHIND the write head** — `basePos = mCapWrite − (grainDuration +
  playhead·(kCapBufSize − 2·grainDuration))`. Grains now read the freshly fed-back output (written just behind
  mCapWrite) instead of the oldest ring data, so Feedback actually regenerates. **Playhead is now a delay-time
  control in live mode** (0 = tight/freshest, 1 = long echo); Lua label updated. (2) **Makeup drive 2.5 inside
  the feedback tanh** — `mFeedBuf = tanh(2.5·0.5·(L+R))` recovers the ~8 dB round-trip loss so feedback reaches
  ~unity loop gain near 1.0; tanh still bounds it → stable drone, no runaway. Host (live, sine burst→silence):
  late-tail 0.00 (fb 0) → 0.51 (fb 0.8) → 0.64 (fb 0.99), bounded, 0 non-finite, ASan clean. The `diracfb`
  fork is now redundant and can be retired.
- **v0.1.10 — Fixed Texture-sweep CPU spike (regression from v0.1.9).** User: CPU high when moving Texture.
  Cause: v0.1.9's percussive envelope called `expf()` PER table entry (1025×), and `buildEnvTable` rebuilds
  every block while the encoder moves → ~1025 expf/block during a sweep (same trap that got Fog removed in
  v0.1.6). Two fixes: (1) **Incremental decay** — one expf for the per-step geometric ratio, then a multiply
  per entry (exp(-4·(t−atk)/(1−atk)) at uniform steps IS geometric, so it's exact — host maxDeltas unchanged:
  0.032/0.010/0.019). (2) **Throttled rebuild** — quantize Texture to a 1% grid, rebuild only on change
  (≤101 rebuilds across the full range vs every block). Host: sweeping Texture every block for 53 s of audio
  = 0.12% of realtime budget. Envelope shape & click-free behavior preserved.
- **v0.1.9 — Sharper grains, per-grain leveling, shorter labels.** (1) **Percussive envelope.** `buildEnvTable`
  now morphs THREE shapes via Texture: **Percussive(0) → Hann(0.5) → Tukey(1)**. Percussive = fast linear
  attack + expo decay + short cosine release-to-zero (click-free), for crisp transient "bits" in sparse
  short-grain clouds. Default Texture moved 0.0→**0.5** so the out-of-box sound stays Hann (param + gb bias
  both 0.5). Host: click-free at tex 0/0.5/1 (perc maxDelta 0.032 < 0.05 thresh), 0 non-finite. (2) **Per-grain
  leveling ("Compress", new inlet [0,1], default 0).** At spawn, peek the source (6 reads) where the grain will
  read, estimate localRMS, apply makeup = clamp(0.3/localRMS, ±12 dB), blend by `compress`. Quiet source bits
  speak in clouds. Host: quiet/loud output ratio 0.10→0.45 at compress 1; ASan/UBSan clean, 0 non-finite;
  Lua/inlet ports still fully consistent (Compress wired). (3) **Shortened all 2nd-screen gb labels** (sub-
  display truncates): gLen→"Grain length (s)", Mix→"Wet/dry (live)", Texture→"Shape: perc–round–flat", etc.
  ⚠️ DEFERRED (user, lowest priority): gLen-scaled phosphor trails on the viz. Also offered but NOT chosen this
  round: binaural (ITD/ILD/head-shadow port from Whirlpool — confirmed same time-domain model, clean to add).
- **v0.1.8 — Code-review fixes (ASan-driven).** (1) **FIXED a heap OOB read in `readSrc` (sample mode).**
  ASan-confirmed: the render loop's single compare-subtract wrap assumes the pitch increment < sample
  length; a tiny sample (≤ a few samples) + extreme SemiShift/Psprd left `i0 ≥ sampleCount` → read past
  `mpSample->mpData`. Added a cheap defensive clamp (`i0` to `[0, sampleCount)`, no divide). Re-ran ASan:
  the case that crashed now passes, 0 non-finite. (2) Removed a dead branch (`if (outStart >= blockSize)` —
  `spawnAt`/`startDelay` are always < blockSize). (3) Split a two-statements-on-one-line spawnAt clamp.
  ⚠️ Stale fork wrappers `src/libmorphgrainsample_wrap.cpp` + `src/libplanck_wrap.cpp` should be deleted by
  hand (perms blocked it here; not built, but cruft + SWIG-collision risk). Review confirmed the strengths:
  incremental per-sample render (no spawn CPU spike), grains store positions not buffers (long grains cost
  no memory), sanitize+softLimit everywhere, Lua/inlet ports fully consistent. Feedback regeneration is weak
  (deferred): grains read FORWARD from the write head into oldest ring data, but fed-back output sits just
  BEHIND mCapWrite → only catches feedback near playhead≈1. Redesign should anchor reads behind mCapWrite.
- **v0.1.7 — Removed Hold Detune (hDtn).** Effectively dead in practice: it only applied per-grain AT
  SPAWN and only when held, but Hold freezes the cloud at the grain cap (no new spawns), so turning hDtn
  after engaging Hold did nothing — and it overlapped Psprd (which scatters all grains always). Dropped
  the inlet/param/control + `holdDetune`/`holdSemi`. `spawnGrain` no longer takes holdDetune; `held` is
  still passed (for the Hold loop flag). User confirmed it did nothing on device.
- **v0.1.6 — Testing-feedback pass: removed Fog + Chord, Rate → Density.** (1) **Removed Fog** — it rebuilt
  the 1025-entry envelope table with `expf` on every change → CPU spike when swept. `buildEnvTable` now
  Hann→Tukey only (cosf), rebuilt on Texture change. (2) **Removed Chord** — `spawnChord`→`spawnGrain`
  (single grain per spawn; dropped kChord* tables, mRatioTable, mChordIn, voice loop, onset stagger).
  (3) **Rate → Density** — `mRateIn` is now a target grain OVERLAP, `hop = grainLen/overlap`, floored at
  `kDensFloor=0.25` so the low end is pointillistic (~5 grains/s) instead of "almost stops audio" (the old
  `SR/rate` made low Hz spawn grains seconds apart = silent/gappy/clicky). 0 = trigger-only; default 3
  (smooth); dial LinearDialMap(0,16), button relabelled "dens". Inlet kept named "Rate" (no SWIG churn).
  Host-verified: density 0 = trigger-only, 0.25–1 pointillistic, ≥2 smooth; single-grain spawn works.
  ⚠️ STILL TODO (user wants it): **Feedback rework to work in BOTH live+sample modes.** It's currently
  capture-ring-based (live only) AND regressed to near-zero sustain even in live mode (tail ~0.002 vs
  ~0.137 originally) — fragile. Needs a proper from-scratch redesign (deferred so it's done right, not
  rushed). Likely approach: a dedicated output→source feedback path that both modes read, with a clean
  delay/loop-gain structure + tanh stability.
- **v0.1.5 — Clicks when changing gLen / Grains → CPU-headroom + no-steal.** User: "a lot of clicks when
  changing gLen, even slow, also grain count." Host tests (SINE) show the engine produces NO sample-level
  clicks on gLen sweeps, hard jumps, voice-steals, or grain-count changes (worst Δ 0.009). The "any control
  change, even slow → clicks" signature is **audio underruns** — Dirac was CPU-heavy and UI redraw on a
  control move tipped it over budget. Cut CPU, biggest first: (1) **killed the per-sample software MODULO in
  sample-mode reads** — `readSrc` did `int(fp) % sampleCount` EVERY sample (A8 has no HW divide); now the
  render loop keeps `readL/readR` wrapped to `[0,sampleCount)` with a cheap compare/subtract and `readSrc`
  indexes directly. Behaviour-preserving (host pitch −12/0/+12 → 480/1000/2000 exact). (2) **gated the edge
  fade** — only runs per-sample when the grain can reach within kSeamGuard of the seam this block (most
  grains skip it entirely); seam protection still engages (boundary test Δ 0.005). (3) **removed the
  per-sample source `sanitize`** — output sanitize still contains NaN/Inf (host-verified 0 non-finite with
  corrupt sample + Hold). Also **removed voice stealing**: `findSlot` returns −1 at capacity instead of
  hard-cutting an active grain (a real click vector on complex audio); excess grains finish naturally,
  density self-limits to Grains. NOTE: couldn't reproduce the clicks on host (engine is clean), so this is
  a CPU-headroom fix targeting underruns; if clicks persist it's elsewhere (need device CPU-meter / does
  changing OTHER controls also click?). The per-sample-modulo removal likely worth porting to any sibling
  that reads a sample per-sample (Dirac-only here; Planck/Bohr already use single-modulo or mask reads).
- **v0.1.3 — CRITICAL NaN/Inf guard (ported from planck v0.8.2).** A non-finite value (corrupt sample
  region, glitchy input) poisons Dirac's capture ring + feedback loop (`tanh(NaN)=NaN`) and held grains
  re-reading a bad region → non-finite output that escapes downstream and persists after delete. Added
  `sanitize()` (bit test on exponent — `-ffast-math`-proof) at: capture write (incl feedback term),
  per-sample source reads (`sL`/`sR` — catches corrupt sample reads + held grains), feedback store
  (`tanh`), final output. Host-verified 0 non-finite for NaN+Inf with a corrupt sample + Hold on.
- **v0.1.4 — Output soft limiter (clipping protection).** Dirac's Level already attenuates per-grain (no
  ceiling-clamp bug like the spectral units), so this just adds the final `softLimit()` (transparent <0.9,
  asymptotes to ±1.0) to stop dense-overlap spikes hard-clipping the DAC. Host-verified Level tracks
  (1.0→peak 0.45 on a dense hot sample), peaks ≤1.0, NaN guard intact.
- **v0.1.2 — Edge fade: boundary crossfade (sample) + read/write collision guard (live).** Dirac is
  time-domain → grains read the buffer CONTINUOUSLY, so two click sources unique to it: (a) a grain
  straddling a non-seamless sample loop boundary (mid-grain step the envelope doesn't mask), (b) the live
  write head lapping a grain's read position (read/write collision). Unified fix in the render loop: fade
  a grain by `dist·kInvSeamGuard` when its read is within `kSeamGuard=256` samples (~5.3 ms) of the
  nearest SEAM — the write head `mCapWrite` in live mode, the loop boundary `0/sampleCount` in sample
  mode. Seam distance tracked incrementally (`relSeam += incL`; one `floorf` per grain per block at setup).
  Dense cloud → overlapping grains cover the dip → reads as a crossfade (the "delta-controls-fade" pattern
  an experienced 301 builder suggested; chosen over bounce-reverse for transparency). Skipped for samples
  shorter than `4·kSeamGuard`; normal playback (read far from any seam) untouched (fade=1). Host-verified:
  non-seamless boundary, playhead parked at seam → worst Δ 0.005 (was several tenths); live pitched-up long
  grains at newest → worst Δ 0.009. Spectral siblings (Planck/Bohr) DON'T need this — pre-rendered snapshot
  grains, no continuous re-read, immune to both.
- **v0.1.1 — triangle viz** (see Viz note above) + chord voice decorrelation (ported from planck v0.7.9).

## Status — v0.1.0 (host-verified DSP; device test needed)

Host-verified (stubbed od:: + interp source): sample granulation, exact pitch
(±/0 semitones), live source, feedback sustain sweep (stable), graphic draws.
NOT device-verified: actual sound/feel, the feedback texture on hardware, view
mutation, both-installed-with-Planck/Bohr (unique names → expected OK).

## Build

```bash
cd ~/Documents/Claude/dirac
make dist ER301_SDK=~/er-301      # → build/am335x/dirac-<version>.pkg
```
Bump VERSION (Makefile + assets/toc.lua) every flash. Stale read-only
`src/libplanck_wrap.cpp` / `libmorphgrainsample_wrap.cpp` copied from the fork
aren't built (swig regenerates `libdirac_wrap.cpp`); delete by hand.

## Lineage / sync note

Shared scaffolding with Planck/Bohr (source switch, capture ring, Mix, spawn
model, viz, build). But the ENGINE is independent — a Planck spectral fix does
NOT auto-apply here. The chord-decorrelation idea (planck v0.7.9) IS carried in
(onset stagger). Whirlpool is the time-domain reference (`../whirlpool`).
