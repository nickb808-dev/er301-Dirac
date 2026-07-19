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

- **v0.1.18 — Speed: playhead scan (pitch/time decoupling, sample mode).** New **Speed** inlet
  [−4, 4], default 0, appended LAST (preserves port indices; SWIG regen not strictly needed —
  ports are runtime-registered — but `make dist` regenerates anyway). Grain pitch is unchanged
  (per-grain read speed); Speed moves WHERE grains spawn: `mScanPos` (double, block-rate — float
  lacks sample resolution on long samples) advances `speed·blockSize/sampleCount` per block,
  wraps [0,1). Speed 0 → parked, scan follows the knob = legacy behavior (host-verified
  BIT-IDENTICAL to 0.1.17); Speed 1 = original tempo; negative = reverse. Touching the Playhead
  knob (change > 0.0005) re-seats the scan there — so Playhead-CV patches should keep Speed at 0
  (documented; CV wobble would re-seat constantly). Live mode ignores Speed (Playhead stays the
  fb delay-time control). Time-stretch artifacts are the classic granular kind: comb/phasiness
  at pitch≈0 + regular spawns (PosJtr/Psprd decorrelate), transient stutter at slow speeds
  (GrainLen scales it). CPU ≈ 0 (block-rate accumulator only). Host: ramp-sample scan verified
  at speed ±1/0.5/±4 (correct traverse rates + wrap), knob re-seat verified, ASan/UBSan clean
  with Speed=4 in the torture config. Lua: "speed" dial (−4..4) after playh.
- **v0.1.17 — Code-review pass: 2 safety fixes + CPU micro-opts.** SAFETY: (1) **Env table OOB guard** —
  `envPh` is a float accumulator; on very long grains (2 s ≈ 96k adds, margin only ~0.0107 ≈ 175 ulps at 1024)
  rounding drift could push `ei` to kEnvTableSize → `eA[ei+1]` read past the 1025-entry table. One compare/sample
  clamps it. (2) **R-channel seam fade** — the edge fade tracked only `readL`; with Detune the R read runs at a
  different speed and drifts »kSeamGuard from L over a long grain, so R crossed the loop boundary / write head
  UNFADED → right-channel click (ITD offset same story smaller). Now `relSeamR` is tracked per grain (st only),
  each channel fades on its own distance. Host: detune-2 grain parked at a non-seamless boundary, R max delta
  0.200 → 0.105 = the detune-0 legit-material floor (0.120). CPU (all host-verified BIT-IDENTICAL on sample-mode
  busy config + live+feedback + texture nodes): (3) feedback path (capture mix + tanh loop) gated on `liveMode` —
  sample mode paid 128 tanhf/block for an inert feature; `mFeedPrimed` clears mFeedBuf once on leaving so
  re-entering doesn't inject a stale block; (4) **readHoisted()** — source read takes cap/sd/nc as ARGS, hoisted
  once per grain-block (float stores to out[] can alias same-typed member loads → compiler couldn't hoist
  mpSample->mpData/mChannelCount itself); readSrc kept as wrapper for the spawn peek; also hoisted `g.envIncr`;
  (5) ring write uses power-of-two mask not compare-branch; (6) env blend skips the 2nd table lerp when ew==0
  (exact: texture at 0 or the 0.5 default). Also `<cstdint>` in Dirac.h, deleted stale fork .o/.so cruft +
  .DS_Store. ASan/UBSan clean (2 s held grains, ±4 oct, 5-sample sample, fb 0.95, detach transitions), 0
  non-finite. Host harness (stubbed od:: headers + ident/asan/seam modes) now lives in **test/host/** —
  `g++ -std=c++11 -O2 -ffast-math -Itest/host -Isrc src/Dirac.cpp test/host/main.cpp -o t && ./t ident out.raw`
  (use -std=c++17 for the -fsanitize=address,undefined build; not part of the device .pkg).
- **v0.1.16 — Code-review micro-opts (behavior-preserving).** (1) Hot render loop: gated the binaural
  head-shadow one-pole LP behind `doShadow` (lpAlpha<1) — it was running 4 ops/sample even when Binaural=0
  (transparent). Common (non-binaural) case now skips it. (2) `incL = powf(2,semis/12)` → `expf(kLog2Over12·semis)`
  (exact, consistent with detuneRatio, faster than powf). Host: binaural still ~1.6× decorrelation, V/oct 1 V=12 st,
  0 non-finite, ASan clean. Review verdict: engine already well-optimized (incremental render, gated seam-fade,
  static env tables, mask/compare-subtract reads, sanitize+softLimit); no dead code, ports consistent. Minor
  noted-not-changed: compress peek uses forward span for reverse grains (tiny estimate error); env blend does 2
  lerps even at exact node textures.
- **v0.1.15 — V/Oct pitch input.** New **V/Oct** inlet, continuous, summed with SemiShift (which stays the
  coarse integer-semitone knob). ER-301 convention (from SDK TestOscillator + Conversions.h): normalized CV
  1.0 = FULLSCALE_IN_VOLTS(10) octaves, so a patched 1 V = 0.1 → ×`kVoctToSemis`(120) = 12 st = 1 octave.
  `semiShift = clamp(round(knob) + voct·120, ±kMaxTranspose=48)` (±4 oct ceiling, aliasing guard); this combined
  transpose is what's passed to spawnGrain (now continuous, so V/oct tracks smoothly). Host: 1 V→12 st, 2 V→24,
  −1 V→−12, knob+7 & 1 V→19, huge→clamp 48. ASan clean, ports consistent. Dial label "1V/oct pitch (patch here)"
  — patchable input; SemiShift remains the manual transpose knob. **All three user TODOs now done** (Texture
  clicks v0.1.13, Psprd scale v0.1.14, V/oct v0.1.15).
- **v0.1.14 — Psprd scale quantization.** New **Scale** inlet/dial (integer 0–6): snaps each grain's Psprd
  pitch scatter to a musical scale so clouds scatter *musically*. 0 off · 1 chromatic · 2 major · 3 minor ·
  4 penta-maj · 5 penta-min · 6 whole-tone. `snapToScale(semis, scale)` reduces to pitch-class, finds nearest
  degree (incl. octave wrap), re-adds octaves; applied to `psprdSemi` at spawn (before semiShift), so scale
  degrees transpose with SemiShift. Implemented as a CV-modulatable dial (like the old Chord) not a menu
  Option — od::Option needs menu wiring that wasn't worth the token cost; dial is proven + modulatable.
  NOTE: param renamed `scale`→`scaleSel` in spawnGrain (local gain `scale` shadowed it). Host: OFF = all 12
  PCs, MAJOR = {0,2,4,5,7,9,11}, pentaMaj = {0,2,4,7,9} exactly. ASan clean, ports consistent.
  ⚠️ TODO still open: **V/oct input** (continuous, summed with SemiShift; ±~4 oct clamp; verify ER-301 volt
  scaling) — discussed, not built.
- **v0.1.13 — Fixed Texture-modulation CLICKS (real cause, not the earlier CPU one).** After v0.1.10 fixed the
  CPU, the user still heard clicks when *modulating* Texture. Cause: ALL grains shared one `mEnvTable` read by
  phase; rebuilding it (even throttled) made every IN-FLIGHT grain's amplitude step → a click per rebuild.
  Fix: build **three STATIC shape tables once** (mPercTable/mHannTable/mTukeyTable, never rebuilt); each grain
  captures Texture at spawn (`g.envTex`) and blends two tables at render. Modulating Texture now affects only
  NEW grains — playing grains are untouched. Removed buildEnvTable(texture)/mEnvCacheTex/the throttle. Render
  cost: 2 table lerps + 1 blend per sample (no rebuild ever). Host: Texture swept every block → maxDelta 0.0057
  (< sine's own slope), click-free, 0 non-finite, ASan clean.
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

## M4L port (2026-07)

`m4l/` contains a Max for Live port: gen~ engine (dirac.genexpr embedded in
dirac.gendsp), Dirac.maxpat wrapper, dirac-wave.js (WaveHeadView equivalent)
and dirac-field.js (triangle field). Plan + rationale: PORT-M4L.md. Status:
generated, NOT yet compiled in Max — first-open syntax nits expected in the
codebox; acceptance checklist in m4l/BUILD.md. Chord voices deferred to v1.1.
SUPERSEDED for devices by ../m4l/dirac/ (the confirmed build_amxd pipeline);
this folder's m4l/ is kept for the design docs + js sources, per SUPERSEDED.txt.

## Eurorack / Tap-O-Matic firmware (2026-07)

Three hardware concepts sketched (SVGs in this folder): a 22HP dedicated
panel (`eurorack-panel-mockup.svg`), a DPT mapping (`DPT-PORT.md`), and —
CANONICAL, since Nick owns the hardware — the **Tap-O-Matic DDV param-fader
firmware** (`TAPOMATIC-PORT.md`, `tapomatic-panel-params.svg`; the grain-
histogram variant `tapomatic-panel-mockup.svg` is parked).

**`tapomatic/` holds the WRITTEN firmware** (July 2026, host-verified, not
yet flashed): `dirac_engine.c` = the audited M4L codebox regenerated via
`m4l/tools/gen2c.py --float` (new float mode: f-suffixed libm, GEN2C_BIG_ARRAY
SDRAM hook, requires -fsingle-precision-constant — M7 FPU is single-precision,
double math there is soft-float). Engine UNMODIFIED — the six port deltas
(dry/wet split via mix=1 + wrapper dry sum, run=1, v/oct summing, reverse CV,
defaults, speed = wrapper-integrated playhead drift) live in `tap_mapping.h`
(pure, host-tested) + `TapDirac.cpp` (patterns from the DDV's TimeMachine.cpp:
block 5, LEDs from main loop — FPU lazy-stacking, GPIO PULLUP switches).
Host results: float engine ALL GREEN on the ported audit harness (0 clicks
matrix-wide, HFres at floor); mapping suite ALL GREEN (v/oct +1V=+12semi
clamp ±24, drift wrap, 20k-state registry fuzz, end-to-end granulation RMS
0.25 + trigger-only mode). Test gotcha worth remembering: at playhead ≥ ~0.5
the module is CORRECTLY silent for up to 2.7 s after reset (frontier gate —
that ring history doesn't exist yet).

time_machine_hardware.{h,cpp} are VENDORED into tapomatic/ (from the DDV
repo, attribution headers; the .cpp omits only the disabled-by-default
NOISE_DIAGNOSTICS impl block — re-copy upstream if enabling that flag, or
if upstream fixes hardware bugs). Remaining for Nick: `make
LIBDAISY_DIR=...`, `make program-dfu`, then the README first-power-on
checklist (switch orientation, v/oct offset, CPU headroom).

## v0.2.0 — multiple playheads + Texture glyph + Playheads option (2026-07)

- **Multi-playhead**: `Heads` [1,4] / `HeadSpread` [0,1] / `HeadTune` [-12,+12] st.
  At spawn a grain is dealt round-robin to head h; position offset
  `(h·spread)/heads` of the buffer span (spread=1 → heads evenly distributed),
  pitch offset `h·HeadTune`. **Spawn-time only — no per-sample cost, no extra
  grains** (Grains cap + density unchanged). `heads=1` short-circuits =
  bit-identical to v0.1.18. `g.pitchSemi` now carries the head transpose so heads
  stack visibly in the grain-field viz. Verified: 3 heads @+7 → 14 st span,
  4 @−5 → 15 st, 1 → 0 st; finite/bounded.
- **Playheads menu option** (`od::Option`, 1=single default / 2=multi). Off hides
  the 3 controls (they're NOT in `controlOrder`; `showHeads()` places/removes them
  mirroring `showWaveform()`, with `onLoadFinished()` syncing a restored preset)
  AND hard-bypasses the DSP (`heads` forced to 1). Verified: option=1 with
  Heads=4/Tune=7 cranked → 0.0 st span; option=2 → 21 st.
- **Texture now readable in the grain field**: `triSplat` takes `tex` and morphs
  the glyph — leaning wedge (Percussive 0) → symmetric arrowhead (Hann 0.5) →
  flat-topped trapezoid (Tukey 1). New `getVizTexture()`. No new UI element.
- Host stub gained `od::Option` (Dirac's harness lacked it). `asan`/`seam`/`ident`
  all still pass. Needs docker-build + hardware.

### v0.2.0 (cont.) — Snap + Orbit

- **Snap** `[0,1]`, default 0 — onset-locked spawning. At spawn, before the
  Compress peek, a **coarse→fine** search runs over the source: 3 stages × 12
  candidates, window starts at ±3600 samples (±75 ms) and shrinks by `2/(kCand-1)`
  each stage (~30-sample final resolution for 36 probes — a flat grid that fine
  would need 240). Each candidate scores `Σ(after²) − Σ(before²)` over 5 taps per
  side at 40-sample spacing; positive = rising edge. Only pulls if
  `bestScore > 0`, and lerps: `startPos += snap * (bestPos - startPos)`.
  **Spawn-time only.** First attempt (16 flat candidates, 4 taps @32) gave only
  1.5× improvement — the grid was coarser than the onset spacing. Coarse→fine
  gives **3.2×** (`snapt` test: mean |start − onset| 1184 → 371 samples).
- **Orbit** `[0,1]`, default 0 — grains travel the field over their life. At spawn:
  `panEnd = pan + orbit*2*randBipolar()` (clamped ±1), then
  `gainLIncr/gainRIncr = (endGain − startGain)/duration` and the same for the
  binaural `lpAlphaL/RIncr`. **ITD is deliberately NOT swept** — modulating the
  read offset would doppler-shift the grain. Render loop: `gL/gR/lpAL/lpAR` went
  from `const` to running values incremented per sample under a `moving` flag
  (all four increments zero → flag false → old code path exactly), written back
  to the Grain each block along with `panPos += panIncr*toRender` so the viz
  follows. `orbitt` test: mean |ΔgainL| over a grain's life 0.0000 (off) → 0.3746
  (on), 0 non-finite, gains bounded (peak 1.41491 vs √2 = 1.41421 — 0.05%
  incremental float drift, ≈0.004 dB, accepted rather than adding a per-sample
  clamp).
- Both are in `controlOrder` (visible by default) and both hard-bypass at 0.
- **`ident` is NOT bit-identical to HEAD (v0.1.18)**: max abs deviation 3.5e-5
  (≈ −89 dBFS) starting frame 1031. Traced to FMA reassociation from the
  multi-playhead `semis + float(head)*headTune` term under `-ffast-math`, NOT a
  behaviour change (reverting the basePos block alone did not restore the hash).
  README claim softened from "bit-identical" to "numerically equivalent".
- Host harness: new `snapt` and `orbitt` modes. **Gotcha:** the mode dispatch has
  an `#ifdef HAS_SPEED` block — new modes must go BEFORE it or they silently
  report "unknown mode" unless `-DHAS_SPEED` is passed.

### Snap × multi-playhead interaction (v0.2.0)

Snap runs AFTER the head offset in `spawnGrain`, so each head snaps independently.
Measured (`headsnap` test, onsets every 4800, 4 heads): unclamped, HeadSpread=0.5
dropped 4 distinct onsets → 3, and HeadSpread=0.10's even 1200-sample ladder
collapsed into two clusters — the ±3600 search window is wider than the head
spacing. **Fix: clamp the window to half the head gap** (`headGap` computed in both
live and sample branches), with a `kMinLane = 400` floor below which heads are
treated as stacked and allowed to converge (that case is reachable deliberately via
HeadSpread ≈ 0). After: HeadSpread=0.5 holds 4 distinct onsets.

**Decided AGAINST a user control for this.** It would only matter when Snap > 0 AND
Playheads = multi AND HeadSpread sits in a middle range — three conditions deep, and
the user is explicitly trying to limit control count. Convergence is already
reachable by lowering HeadSpread; ladder-preservation was NOT reachable any other
way, so the clamp adds capability rather than trading one for another. Honest caveat
kept in the README: at moderate spacing the two features are geometrically
incompatible (no position is both on-transient and correctly spaced), so Snap goes
inert for heads that can't reach an onset within their lane. HeadTune is never
touched by Snap — the chord always holds.

### Shift-tap axis toggle on the grain field (v0.2.0)

**The idea:** Snap and multi-playhead both move grains to specific buffer positions,
and NO display showed where. The waveform shows one playhead marker, not 4 heads and
not Snap's pull. Shift-tap on `GrainFieldView` now swaps the field's X axis from
stereo pan to normalised read position, and swaps the sub display from the Dirac
epigraph to an axis legend.

**Why the field control specifically:** shift is heavily booked by the firmware on
GainBias controls (`shift+sub2/3` = numeric keyboard entry, `shift+encoder` = fine
adjust, plus shift+spot/dial). `GrainFieldView` is display-only — no readouts — so
shift is free there. Pattern borrowed from habitat's `AnamSubControl`:
`grabFocus("shiftPressed","shiftReleased")` on cursor enter, `shiftUsed` flag to
tell a TAP from shift-as-modifier.

**API traps hit (both would have crashed on hardware):**
- `ViewControl` defines **no** `encoder` and **no** `subReleased`. My first draft
  overrode `encoder` and called `ViewControl.encoder(...)` → nil call. The only
  shift-taking methods the base actually defines are `dialPressed(shifted)` and
  `spotReleased(spot, shifted)`; the shiftUsed guard hangs off those.
- `ViewControl:onCursorEnter/Leave` already add/remove `self.subGraphic`, so
  `setAxisMode` must remove the OLD graphic before swapping the field and adding the
  new one, or the panel leaks/blanks.

**DSP side:** new `Dirac::getGrainReadPos01(i)` — live mode returns position relative
to the ring write head (1 = newest), sample mode returns readPos/mEndIndex. Uses the
LIVE read position, not the spawn position, so grains drift as they play.
`DiracFieldGraphic::setAxisMode()` clears the phosphor on change so the two mappings
don't smear together; POSITION mode draws a dashed baseline so an empty field still
shows which mode you're in, and skips the L/R detune split (a stereo concept that's
meaningless on this axis).

**SECTION_PLY = 42**, so the panel is 42 px for the whole buffer — coarse but the
right scope (whole-buffer overview, not a zoom). Verified with `fieldpos` test which
histograms into 42 columns: hsprd 0.0/0.3/0.9 → 1 column / clustered pairs / 4 even
columns; snap 0→1 turns a broad smear into two clumps. Axis mode is NOT serialised
(view preference, like focus).

### Mode-aware control strip (v0.2.0)

`mix` + `feedback` hidden in SAMPLE mode, `speed` hidden in LIVE mode. All three are
already hard-gated in Dirac.cpp (`if (liveMode && mix < 0.99995f)`,
`if (liveMode && feedback > 0.0001f)`, speed inside `if (!liveMode)`), so this only
removes knobs that visibly move and do nothing — no behaviour change.

**Hook:** `Dirac:setSample()` is the single funnel (card, pool, detach, and
deserialize all route through it) and already called `showWaveform`; added
`showModeControls(self.sample == nil)` beside it. `onLoadFinished()` also calls it,
because `deserialize` only invokes setSample when the preset HAS a sample — a
sample-less preset would otherwise never reconcile.

**The subtle part — insertion position.** `placeControl(id, view, pos)` takes a
USER-facing position and internally does `pos + 2` to skip the insert+header
entries. Appending (pos=nil) is what showHeads does, fine for controls that live at
the end — but `speed` sits between `playhead` and `grainlen`, so re-showing it must
compute a position or the strip order scrambles. `orderedPosition()` counts how many
of a control's canonical predecessors are currently placed. Canonical order =
`{"wave","field"} .. controlOrder`; head controls are appended after everything so
they're deliberately absent from it. Verified by simulating the Lua table arithmetic
in Python across boot → attach → detach → re-attach → detach-with-heads: all orders
correct.

**Lua scoping trap hit:** `kExpandedOrder` is built next to `controlOrder`, far BELOW
`showModeControls`. A `local` declared after a function is not an upvalue of it — it
resolves as a nil global at call time. Fixed with a forward `local kExpandedOrder`
above the function and a plain assignment (not re-declaration) below. Same trap would
bite any future helper that references the view tables.

`speed` is also removed from the default `views.expanded` (built live-mode-first) but
KEPT in `controlOrder` so its focused view `{"wave","speed"}` is still created.
Note: head controls are NOT in controlOrder, so they have no focused view and fall
back to the framework default `{"scope", <control>}` — pre-existing, not fixed here.

### Orbit audibility — CORRECTION to earlier advice (v0.2.0)

I originally documented Orbit as "best with long grains and Binaural up". Hardware
feedback ("orbit was not prominent") prompted an actual measurement (`orbitaud` test:
RMS of block-rate stereo balance (|L|-|R|)/(|L|+|R|)), and the advice was wrong on
both counts:

```
 gLen   dens2/grains3   dens12/grains16
  50ms      0.427            0.175
 200ms      0.340            0.176
 500ms      0.370            0.153
1000ms      0.310            0.166
```

- **DENSITY is the dominant lever**, not grain length — swing roughly HALVES at high
  density. Each grain picks a random direction, so a dense cloud averages them into a
  static blur. Dirac's defaults (`dens 3 / grains 12`) sit in the washed-out regime,
  which is why it read as weak on hardware.
- **Grain length does not change travel distance**, only the RATE: 50 ms ≈ 20 Hz zip
  (perceived as width/timbre), 500 ms ≈ 2 Hz sweep (perceived as motion). The metric
  measures magnitude, not rate, hence the flat column.
- **Orbit does NOT require Binaural or Spread.** `pan = randBipolar() * sprd` but
  `panEnd = pan + orbit*2*randBipolar()` runs whenever orbit > 0, so the pan lerp is
  independent of both; Binaural only adds the head-shadow sweep on top. Swing at
  `bin = 0` is 0.15–0.43, i.e. plainly audible.

## v0.2.3 — removed Glide (2026-07)

Nick tested 0.2.2 on hardware, kept Color/Diffuse/Freeze/sample-mode-feedback, cut
**Glide** (control-count discipline; the feature worked — `glidet` passed — it just
didn't earn its dial). Fully backed out: inlet, Grain fields
(glideRatio/pitchSemiIncr), spawn block, render-loop ratio path (incL/incR back to
const — hot loop restored exactly), seam travScale, hold park lines, Lua
param/dial/controlOrder, harness `glidet` + setInputs + torture ctl. Ports after
Color re-registered one earlier — safe, everything is name-bound. If glisson ever
comes back, the full implementation is in git history at the 0.2.2 tree.

Also in 0.2.3: **Heads dial now integer-steps** (LinearDialMap(1,4) +
setCoarseRadix(3), the Grains/Semitone pattern) — it deals grains to discrete
heads and the DSP rounds anyway, so a "2.7 heads" readout was noise. Note the
integer maps at the bottom of onLoadViews are declared AFTER the heads gb call,
so headsMap is declared locally beside it.

## v0.2.2 — four features + sample-mode feedback (2026-07)

Nick approved the commercial-gap proposals; everything hard-bypassed at defaults —
**`ident` is BIT-IDENTICAL to 0.2.1** with all new features off (host-verified cmp).

- **Color** (dial): per-grain LP cutoff scatter, log-uniform 20 kHz→~600 Hz at 1
  (`fc = 20000·e^(−3.5·color·u)`). REUSES the binaural head-shadow one-pole:
  min-combined per ear INCLUDING the Orbit alpha-sweep endpoints (recomputes the
  increments), so a sweeping grain never rises above its colour ceiling. Render
  cost = the already-gated LP path. `colort`: HF ratio 1.10→0.46 (2.36× darker).
- **Glide** (dial): glisson — per-grain destination ±12 st·glide (clamped
  ±kMaxTranspose), per-sample RATIO on both pitch increments (exponential in Hz =
  linear in st; detune interval preserved). Persisted like Orbit's gains
  (magnitude stored, reverse sign stripped on write-back); seam-gate travel
  estimate scaled by glr^toRender on the rising side. **Hold PARKS it**
  (glideRatio=1, pitchSemiIncr=0) — same lesson as Orbit×Hold, or pitch compounds
  every loop. `glidet`: ZC dev 0.00 off / 0.11 on; held max |pitchSemi| = 12 st.
  Viz: pitchSemi drifts so the field shows the bend.
- **Diffuse** (menu option, off default): 4 cascaded Schroeder allpasses per
  channel, mutually-prime delays L={113,337,719,1201} R={127,347,743,1213},
  g=0.6, ~49 ms path. Placed AFTER the grain sum, BEFORE the feedback tap →
  regeneration is diffused (tanh still bounds the loop). Inputs sanitized so NaN
  can't lodge in the delay lines; lines cleared on Off (mDiffusePrimed). C++11
  gotcha: static constexpr arrays need out-of-line defs in Dirac.cpp. `difft`:
  temporal spread ×36.6, energy ×1.00 (exactly conserving). CPU: +14% on a busy
  host config (~0.03% realtime absolute). **Test-methodology trap worth
  remembering: the first difft missed the FIRE block in the dry pass — the
  diffuser delayed that unmeasured energy into the window and read as ×9.7
  "gain". Chased it through isolated-allpass, ASan, -O0, stub-aliasing before
  finding the test bug. Measure ALL blocks from the trigger.**
- **Freeze** (menu option, "Capture: run/freeze"): gates every ring write (input
  mix AND feedback reinjection); mCapWrite parks, all controls stay live over the
  locked 2.7 s. Hold freezes the CLOUD, Freeze freezes the SOURCE. `freezet`:
  late RMS 0.17 frozen vs 0.0000 running. Test gotcha: playh must sit inside the
  tone-filled span of the ring or frozen grains read pre-fill zeros.
- **Sample-mode Feedback** (the old "possible v2" note, done): ring now captures
  `in + fdbk·tanh(wet)` in BOTH modes; in sample mode a FEEDBACK-SIZED SHARE of
  spawns becomes regen grains (`randUnipolar() < feedback` → g.live=true) reading
  a tight anchor `mCapWrite − grainDuration − 4800` (margin covers posJtr's
  bipolar spray). Per-grain source choice = hot loop untouched. Head position
  offsets skipped for regen grains, head PITCH kept (they join the chord);
  snap/compress peeks follow srcLive so they probe the right source. Lua:
  feedback REMOVED from kLiveOnlyControls (visible in both modes now), labels
  updated. `sfbt`: silent sample, late RMS 0.338 regen vs 0.000 at fdbk 0,
  bounded, 0 non-finite.
- Harness: setInputs gained Color/Glide (forgot at first — colort silently
  no-opped; add new inlets to the ins[] list EVERY time), asan gained a v0.2.2
  torture config (everything at once + freeze toggled mid-flight + detach with
  diffuser running). All modes PASS under ASan+UBSan.

## v0.2.1 — code-review hardening (2026-07)

Released as its OWN version (Makefile + toc.lua 0.2.1) so Nick can A/B against the
0.2.0 pkg on device. Three changes, all host-verified:

- **Live-mode head offsets scale into remaining room.** Was `basePos -= headOff ·
  (maxDelay − minDelay)` — at playh→1 with HeadSpread up, total delay reached
  ~1.75× the ring and the mask wrapped "older" heads onto the freshest material
  (write-head collision zone, scrambled head order). Now `room = maxDelay − delay`
  and both the offset AND `headGap` (the Snap lane width) scale by it: heads always
  fit, order monotonic, spacing shrinks to stacked at playh = 1. Host (`livehead`
  throwaway, playh 0.95 / hsprd 1 / 4 heads / HeadTune 1): pre-fix h3 delay
  exceeded maxDelay (wrapped); post-fix all delays in [minDelay, maxDelay], strictly
  increasing with head index. BEHAVIOR NOTE vs 0.2.0: live-mode head spacing now
  narrows as Playhead rises (it no longer aliases instead).
- **Snap per-block search budget** (`kSnapPerBlock = 2`, `mSnapBudget` reset each
  process()). The 360-read coarse→fine search ran per spawn; worst case (64-sample
  grains, overlap 16 ≈ 12k spawns/s) ≈ 4.3M reads/s. Budget caps it at 2
  searches/block (~16× worst-case reduction); grains past the budget spawn
  unsnapped that block. Deliberately NOT a per-block cached result — posJtr+Snap
  landing each grain on a DIFFERENT hit (README quick start #7) requires per-grain
  searches. Musical densities (~1 spawn/block in the snapt/headsnap configs) never
  hit the cap: snapt still 3.2×, headsnap still 4 distinct onsets at hsprd 0.5.
- Version bumped in Makefile + toc.lua + README (changelog entry added).

### Code-review fixes (v0.2.0, pre-release)

- **FIXED Orbit × Hold unbounded gain drift.** The hold-loop branch reset
  age/envPhase/readPos but NOT the orbit travel state, so a held orbiting grain kept
  integrating its increments every loop: host-measured gains grew ~4.8/s linearly,
  crossed zero (phase flip), pinned the soft limiter into a distorted drone; with
  Binaural up the LP alphas left [0,1] and killed the channel. Fix: the grain sits
  exactly at its travel destination at loop time, so the hold branch now zeros all
  five increments (gainL/R, lpAlphaL/R, pan) — the grain parks there. Regression
  case added to `orbitt` (orbit 1 + binaural 1 + ~4 s of hold: peakGain 1.4152,
  bounded; 0 bad).
- **FIXED harness compile break.** The new test modes reached into private state
  (`Dirac::kMaxGrains`, `mGrains[i].readPos0L/gainL/gainR/pitchSemi`) — the README
  build command failed with 7 errors. `kMaxGrains` is now a public constant and the
  harness uses new public accessors `getGrainSpawnPos/getGrainGainL/getGrainGainR`
  (+ existing `getGrainPitch`), all inside `#ifndef SWIGLUA` so the SWIG surface is
  untouched. Also: `fieldpos` printf extra-arg wart fixed (prints top3 concentration
  now), and `ident`/`speed` guard argc/fopen instead of segfaulting on bad usage.
  All modes re-verified after the fixes: snapt 3.2×, orbitt/fieldpos PASS, seam
  0.10473, asan clean (ASan+UBSan), ident runs.

Recipe now documented: `dens 2 / grains 3 / gLen 500 ms / orbit 1`, then add `bin`.
Corrected in README feature note, control table, quick start, and changelog.
**Lesson: don't write performance advice from the design intent — measure it.**

### Orbit REMOVED in v0.2.4 — and why

Shipped in 0.2.0, cut in 0.2.4 after hardware listening ("doesn't seem to make much
of a sonic difference"). **It was not broken** — `orbitab` A/B measured swing +152%
and image-movement rate +254% at 500 ms / dens 2. It was just perceptually weak, for
three stacking reasons:

1. Equal-power pan is a ~5 dB cue for a half-field trip.
2. The `sin(πt)` grain envelope silences departure AND arrival — amplitude-weighted
   excursion is only 75% of a flat window, and just 50% of the trip is above −3 dB.
3. Random per-grain directions partially cancel in a cloud.

Tested **coherent direction** (all grains drift the same way) as a fix: better at
dense/short (+229% vs +109% swing) but WORSE at the sparse/long showcase setting
(+109% vs +152% swing, +127% vs +254% rate). Not the lever. Cut instead.

**Process note worth keeping:** the first `orbitaud` test had NO orbit=0 control
condition — it measured absolute swing and I read density/grain-length trends off it,
which produced advice I later had to retract. Always include the off condition.

**Entanglements unpicked on removal** (both introduced after 0.2.0, easy to miss):
- **Color** clamped the Orbit alpha-sweep ENDPOINTS (`endL/endR` from
  `lpAlphaL + lpAlphaLIncr*dur`) so a sweeping grain couldn't rise above its colour
  ceiling. With Orbit gone this collapses to a plain min-combine of `lpAlphaL/R`
  against `aC`.
- **Hold** zeroed the five Orbit increments at loop time to stop gains running off
  unbounded. Whole block deleted with the increments.
- Render loop: `gL/gR/lpAL/lpAR` returned to `const`; `moving` flag, per-sample
  increment step, and cross-block write-back all removed. `doShadow` dropped its
  `lpA*Inc != 0` terms (Color still sets `lpAlphaL/R` directly, so it stays covered).

**Port-index caveat:** `mOrbitIn` was registered between `mSnapIn` and `mColorIn`, so
removing it shifts Color's port index down by one. Lua connects by NAME
(`connect(p,"Out",head,port)`) and branches serialise by name, so this should be
safe — but it's the reason the ctor comments stress append-only ordering. Worth
confirming an old preset with Color set still reloads correctly.

If spatial motion is ever revisited: **doppler**, not panning. Pitch shift is the cue
the ear reads as movement. It was deliberately excluded (ITD held at spawn azimuth to
avoid doppler-shifting the read) — but that artifact IS the effect.
