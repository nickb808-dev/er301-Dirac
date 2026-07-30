# Dirac

A granular synthesizer for the ER-301 named after [Paul Dirac](https://en.wikipedia.org/wiki/Paul_Dirac). Dirac chops a sound into short overlapping grains, each read straight from
the audio, windowed, pitched by playback speed, and layered.
It runs on a live input by default and switches to granulating a loaded
sample as soon as you select one from the menu.

Unit title: **Dirac** · mnemonic **Di** · stereo out · current version **0.2.8**

---

## How it works

Each grain reads a short window of the source with linear interpolation, multiplies
it by an envelope, and plays it back. Many grains
overlap into a continuous texture. Because it's time-domain, pitch shifting moves
formants (the characteristic granular shimmer), transients stay sharp, and grains
cost almost nothing. Long grains and dense clouds are cheap, no per-grain buffers.

**Two source modes (automatic):**

- **Live** (no sample loaded) — Dirac captures the input into a ~2.7 s rolling ring
  buffer and granulates that. This is where Feedback, Mix, and the buffer
  scan live.
- **Sample** (select one from the menu) — Dirac granulates the loaded sample at a
  movable Playhead, drawn on the waveform display.

---

## Quick start

1. Add Dirac, patch in audio. You'll hear a cloud immediately. Density
   defaults to 3 and free-runs, so grains spawn on their own.
2. gLen sets grain size and grains sets how many overlap: short + sparse is
   pointillistic; long + dense is a smooth wash.
3. Texture shapes each grain: turn it down for crisp percussive "bits," leave
   it centered for a rounded Hann, turn it **up** for a fuller flat-top grain.
4. In live mode, fdbk regenerates and sustains grains after the input stops (drones, smears, infinite textures).
   playh sets the feedback delay / how far back in the buffer grains read.
5. Load a sample from the menu to granulate a recording; sweep playh through it.
6. Patch a gate into **trig** for rhythmic grains, or set Density to 0 for
   trigger-only.
7. Granulating a beat? Turn snap up and grains lock onto the transients instead of
   landing in the gaps. Try it alongside posJtr at 1: the jitter sprays the reads
   wide and Snap reels each one back onto a hit, so you get variety in *which* hit
   rather than a comb of identical reads.
8. For chords, switch Playheads → multi in the menu (sub-display button) to
   reveal **heads** / **hsprd** / **htune**. `htune = 7` stacks fifths, `4` major
   thirds, `−12` octaves down.
9. Put the cursor on the grain field visual and tap shift. The X axis flips from
    stereo position to *read position in the buffer*, so you can watch where grains
    are actually reading. Tap again to flip back. This is the view that makes Snap
    and multiple playheads legible.

---

## Controls

Spawning is density-driven (free-run) plus trigger edges, decoupled — both
can run at once.

| Btn | Inlet | Range / default | What it does |
|---|---|---|---|
| trig | Fire | gate | Spawns one grain per rising edge (manual button or patched gate). |
| hold | Hold | toggle | Freezes the current grains into a looping, frozen cloud. |
| playh | Playhead | 0–1 / 0 | **Sample:** read position. **Live:** how far back in the buffer grains read — also the **feedback delay** (0 = most recent / tight, 1 = oldest / long echo). |
| speed | Speed | −4…+4 / 0 | *(Sample mode only. Hidden in live mode.)* **Playhead scan rate** — decouples time from pitch. 0 = parked (playh is the position, as before); 1 = original tempo; negative = reverse; wraps at the ends. Touching playh re-seats the scan. |
| dens | Rate | 0–16 / 3 | Density is target grain overlap (how many deep). 0 = trigger-only (free-run off). |
| gLen | GrainLen | 1 ms – 1 s / 50 ms | Grain length. |
| grains | Grains | 1–16 / 12 | Polyphony limit / number of overlapping grains. |
| semi | SemiShift | −24…+24 st / 0 | Coarse transpose knob (integer semitones, playback-speed pitch). |
| V/oct | V/Oct | 1 V/oct / 0 | Continuous pitch CV. Patch a 1 V/oct source here. This is summed with SemiShift and limited to ±4 octaves. |
| psprd | Psprd | 0–12 st / 0 | Per-grain pitch scatter. |
| scale | Scale | 0–6 / 0 | Quantize the Psprd scatter to a scale: 0 off · 1 chromatic · 2 major · 3 minor · 4 penta-maj · 5 penta-min · 6 whole-tone. |
| detun | Detune | 0–0.5 st / 0 | Stereo detune. |
| sprd | Spread | 0–1 / 0.5 | Stereo position scatter across grains (also the binaural width). |
| posJtr | PosJtr | 0–1 / 0 | Random spray of each grain's read position (±100 ms). |
| rev | RevProb | 0–1 / 0 | Probability a grain plays in reverse. |
| textr | Texture | 0–1 / 0.5 | Grain envelope shape: **Percussive (0) → Hann (0.5) → Tukey (1)**. |
| snap | Snap | 0–1 / 0 | This pulls each grain's read position onto the nearest transient instead of wherever the metronome landed. 0 = free (classic), 1 = locked. Turns a smear of a drum loop back into hits. |
| color | Color | 0–1 / 0 | Each grain draws a random low-pass cutoff, darker with Color (20 kHz down to ~600 Hz at 1). The spectral version of psprd. |
| comp | Compress | 0–1 / 0 | Per-grain leveling that boosts quiet source bits and tames loud ones (±12 dB) so sparse clouds stay even. |
| bin | Binaural | 0–1 / 0 | Gives 3D depth by way of interaural time difference (ITD) + head-shadow filtering on top of the pan, scaled by Spread. 0 = plain stereo. |
| heads | Heads | 1–4 / 1 | The number of playheads sharing the buffer. Grains are dealt round-robin between them. 1 = classic single head. *(Hidden until **Playheads → multi** is set in the config menu.)* |
| hsprd | HeadSpread | 0–1 / 0 | Position offset between heads. 0 = all heads stacked on the playhead; 1 = heads spaced evenly across the whole buffer/sample. |
| htune | HeadTune | −12…+12 st / 0 | Pitch interval between heads that can form a chord. Chromatic ladder by default (`7` → 0-7-14-21). With `scale` set, the ladder turns diatonic (v0.2.7): `htune 4` + major voices a real triad 0-4-7-12; `7` gives in-scale fifths; the shape transposes with `semi`/V-Oct (the root is never snapped). |
| level | Level | 0–1 / 0.7 | Output level. |
| mix | Mix | 0–1 / 1 | *(Live mode only — hidden when a sample is attached.)* Wet/dry. 1 = grains, 0 = dry input through. |
| fdbk | Feedback | 0–1 / 0 | This reinjects output into the capture ring, tanh soft-clipped so it saturates into a stable drone. With live input the cloud re-granulates its own output (~0.8 = clear tail, ~1 = held drone). When working with a sample file a feedback-sized share of grains reads the regeneration ring instead of the sample, so the cloud granulates its own tail on top of the sample. |

### Sample menu

Press the unit's sub-display button for: **Select from Card**, **Select from Pool**,
**Detach Buffer** (return to live input), **Edit Buffer**.

### Menu options

Below the sample tasks in the same menu:

| Option | Choices / default | What it does |
|---|---|---|
| Playheads | single / **single** | 'multi' reveals the `heads` / `hsprd` / `htune` controls: grains are dealt round-robin to up to 4 playheads reading the same buffer at offset positions and stacked pitch intervals (`htune 7` = fifths, `−12` = octaves down). 'single' hides the controls and hard-bypasses the DSP path. |
| Diffuse | off / 'off' | 'on' runs the wet output through four cascaded allpasses per channel (~49 ms, different delays L/R) — smears grain edges into a lush halo, and feeds the regeneration loop so Feedback smears too. Off is a hard bypass. |
| Capture | run / 'run' | 'freeze' stops *writing* the ~2.7 s capture ring: the source material locks while every control stays live — keep scanning, pitching, and re-granulating the frozen moment. Contrast with Hold, which freezes the *cloud* (grains loop, no new spawns); Freeze freezes the *source* (spawning continues). Pair with `fdbk` for a self-sustaining instrument; in sample mode it stalls the regeneration ring. |

---

## Reading the grain field

The phosphor scope isn't decoration — every dimension of a grain's life is drawn.
One glyph = one grain; learn the mappings and the field becomes a diagnostic:

| You see | It's showing | Controls that move it |
|---|---|---|
| Number of glyphs | Cloud population | `dens`, `grains`, `trig` |
| Horizontal position | Stereo placement | `sprd` (scatter width) |
| One glyph splits into an L/R pair | Stereo detune | `detune` |
| Vertical position | Pitch | `semi`, **V/Oct**, `psprd`, `htune` |
| Vertical scatter collapses into discrete rows | Pitch quantization | `scale` |
| Glyph swells and fades | The grain's envelope over its life | (grain lifecycle; slow = long `gLen`) |
| Glyph shape: leaning wedge → arrowhead → flat-top | Envelope shape | `textr` (perc / Hann / Tukey) |
| **Stem height under the arrowhead** | Grain length increase produces a taller [Dirac comb](https://en.wikipedia.org/wiki/Dirac_comb) ↑ | `gLen` |
| Glyph points **down** (stem above) | Reverse grain | `rev` |
| **Hollow** outline instead of filled | Regeneration grain (reading the feedback ring, sample mode) | `fdbk` |
| Overall brightness | The grain's real loudness | `level`, `compress`, source dynamics |
| Some glyphs are more **dim** | Spectrally dark grains (low cutoff) — also the binaural far-ear shadow | `color`, `bin` |
| Trails linger ~2× longer | Diffusion | **Diffuse** (menu) |
| Whole constellation freezes | Cloud frozen (grains looping) | `hold` |
| **✳ snowflake**, top-right — Capture ring frozen | **Capture: freeze** (menu) |

Shift-tap the field to flip its X axis from stereo to *read position in the
buffer* (the sub display shows the legend). In this view:

| You see | It's showing | Controls that move it |
|---|---|---|
| Horizontal position | Where each grain reads in the buffer/sample | `playh`, `speed`, `posJtr` |
| Glyphs collect onto the same spots | Onset locking | `snap` |
| The cloud splits into evenly spaced columns | Multiple playheads | `heads`, `hsprd` |
| Dim trail behind each glyph | The material the grain has covered so far — grows as it plays, leftward if reversed | `gLen`, pitch, `rev` |
| Dashed baseline crawls | Capture ring recording (parks when frozen) | Capture (menu) |

## Feature notes

**Feedback (live).** Grains read just *behind* the write head, so they pick up the
freshly fed-back output and regenerate. Playhead is the delay time. A makeup stage
inside the soft-clipper lets Feedback approach unity loop gain near 1.0 while `tanh`
keeps it bounded — push it for evolving drones and infinite smears.

**Binaural.** Each grain's pan position becomes an azimuth: the far ear's read is
delayed by up to ±31 samples (ITD) and rolled off by a one-pole head-shadow lowpass
(20 kHz → 4 kHz toward the side). Spread sets the width, Binaural the depth;
at 0 it's bit-identical plain stereo.

**Snap (onset lock).** Granulating rhythmic material normally lands in the gaps as
often as on the hits, which is why drum loops turn to mush. Snap runs a short
coarse→fine search around each grain's intended start — three stages of 12 probes,
each scoring *energy after* minus *energy before* — and pulls the read toward the
strongest rising edge within ±75 ms. At 1 the grains lock to transients; in between
they're statistically biased toward them, so the cloud gets a pulse without becoming
a stutter. Spawn-time only (~36 buffer peeks per grain), fully bypassed at 0.
Measured on a click track: mean distance to the nearest onset drops **3.2×**.

With **multiple playheads**, Snap runs per head, and the search window is
automatically clamped to half the head spacing so each head stays in its own lane —
otherwise the ±75 ms window is wider than the spacing at moderate HeadSpread and the
heads collapse onto shared transients. Note the two features are geometrically in
tension when the head spacing is much smaller than the gap between onsets: there is
no position that is both on a transient *and* correctly spaced, so Snap simply goes
quiet for the heads that can't reach one. Wide HeadSpread gets you both. HeadTune
(pitch) is never affected, so the chord holds at any Snap setting.

**Compression.** At spawn, Dirac peeks the source where each grain will read and
applies a makeup gain toward a target — so quiet passages still speak in sparse
clouds without making loud ones clip.

**The strip follows the source mode.** Mode-gated controls are placed and removed as
you attach or detach a sample rather than left on screen doing nothing. Attaching
hides **mix** (wet/dry over the live input) and reveals **speed** (the playhead
scanner, which only means anything with a sample). Detaching reverses it. **fdbk**
stays visible in both modes since 0.2.2 (sample-mode regeneration). Controls return to their
proper slot in the strip, not the end, and the state is reconciled on preset load —
including sample-less presets, which never fire the attach path.

**Grain field: shift-tap to change the X axis.** The field normally plots each grain
at its stereo position. Tap shift while the cursor is on the field and the X
axis becomes read position in the buffer/sample — where each grain is actually
reading — with the sub display swapping from the Dirac epigraph to an axis legend.
Tap shift again to go back.

This is how you *see* Snap and multiple playheads working. Turn HeadSpread up and the
cloud splits into one column per head; turn Snap up on rhythmic material and the
grains visibly collect onto the transients. Measured on a click track (each character
is roughly one pixel column of the real 42-px panel):

```
snap=0      ......-++#+##++-..........................
snap=1      ....+.--##--##----........................    grains gather on the hits

hsprd=0.0   ..#+......................................    heads stacked
hsprd=0.3   ..#..#+.##.##.............................    heads separating
hsprd=0.9   ..#........##........#........##..........    four distinct heads
```

Shift is free on this control because it has no gain/bias readouts to collide with
the firmware's `shift+sub` (numeric entry) and `shift+encoder` (fine adjust)
bindings. A shift *tap* is distinguished from shift-as-a-modifier, so holding shift
for something else never flips the axis. The mode is a view preference — it isn't
saved with the preset.

**Visualizer.** The grain-field display draws each grain as an upward triangle —
a Dirac-comb impulse — at its stereo position (horizontal) and pitch (vertical); the
triangle pulses with the grain's envelope and leaves a phosphor afterglow as grains
decay. (Impulse triangles are Dirac's signature.)

---

## License

Dirac is licensed under the **Apache License 2.0** — free to use, modify, and
embed, including commercially. If you distribute Dirac, a derivative, or a
product that embeds it, the license requires you to include the `LICENSE` and
`NOTICE` files with your distribution — that NOTICE carries the attribution.
In plain terms: **use it in your product, credit the source.**

## Build

```bash
cd ~/Documents/Claude/dirac
make dist ER301_SDK=~/er-301      # → build/am335x/dirac-<version>.pkg
```

Requires Docker (cross-compiles for the AM3358 / Cortex-A8). Bump `VERSION` in the
`Makefile` and `assets/toc.lua` on every flash — the firmware caches the `.so`
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
./t snapt           # Snap: mean |grain start - nearest onset| on a click track
```

---

## Changelog (recent)

- **0.2.8** — **Fix: feedback no longer fades out into an "overload"** (worst
  with multiple playheads). The regeneration loop was slowly filling with
  low-frequency energy the saturator kept re-creating; the pedestal railed the
  tanh and squeezed the audible signal to nothing over tens of seconds. The
  feedback path now has a ~120 Hz high-pass inside the loop (the same medicine
  Clouds uses): long-run audio content holds steady where it previously decayed
  to a sixth. Regeneration tails thin slightly below 120 Hz per pass — single
  passes keep full bandwidth. Also worth knowing: engaging **multi playheads
  drops the level a few dB by nature** (four decorrelated read positions
  power-sum where one head's overlapping grains amplitude-sum — chorus physics,
  not a bug); compensate with `level`.
- **0.2.7** — **Diatonic head voicing.** With Playheads → multi and a `scale`
  selected, the `htune` interval ladder snaps into the scale: `htune 4` + major
  = a true triad + octave (0-4-7-12), `htune 7` = in-key fifths, `htune 2` =
  diatonic clusters. Psprd scatter joins the same quantization, landing on chord
  tones instead of smearing chromatically. The root stays free — `semi`/V-Oct
  transpose the voicing as a shape and melodies are never snapped to the key.
  Scale off (or a single head) is bit-identical to 0.2.6.
- **0.2.6** — **Fix: sample mode no longer hears the lane.** Since 0.2.2, raising
  `fdbk` with a sample attached let audio from units to the left bleed into the
  cloud (the regeneration ring was still capturing the input). The ring is now a
  pure regeneration bed in sample mode — feedback granulates only the unit's own
  output, bootstrapped from the sample itself. Regeneration behavior is otherwise
  unchanged, and `fdbk 0` was never affected.
- **0.2.5** — **Every control now reaches the grain field.** Reverse grains point
  *down* (an upside-down Dirac comb); spectrally dark grains (Color, or the
  binaural head-shadow) draw dim; sample-mode regeneration grains (Feedback's
  share of the cloud) draw as hollow outlines; Diffuse doubles the phosphor
  afterglow — the halo you hear is the smear you see; the position-axis baseline
  crawls with the capture write head and parks when Capture is frozen; and glyph
  brightness is now the grain's *real* loudness (envelope × gain), so Level dims
  the field and Compress visibly evens it out. A small **snowflake** appears in
  the field's top-right corner while Capture is frozen (and melts away through
  the phosphor when you unfreeze). **Grain length now reads at a glance**: each
  arrowhead stands on a comb stem — ↑ — whose height scales with grain length
  (short grains are bare points, 1 s grains stand tall), and in the position
  view each grain drags a dim trail from where it spawned to where it's
  reading — its length as the material it actually covers, growing leftward
  for reverse grains. Display-side only — zero audio CPU change.
- **0.2.4** — **Removed Orbit.** It measured real (roughly double the stereo-balance
  swing, +254% image-movement rate at `dens 2 / gLen 500 ms`) but never earned its
  strip slot on hardware. Three things capped it: equal-power panning is only a ~5 dB
  cue, the `sin(πt)` envelope silences the departure and arrival so ~25% of the
  excursion is inaudible, and per-grain random directions partially cancel across a
  cloud. Coherent direction was tested as a fix and was *worse* at the sparse/long
  settings that showcase it. The honest verdict was "real but subtle," which is the
  category that clutters an interface without expanding what you can make. Removal
  also simplified the **Color** path (it no longer has to clamp Orbit's alpha-sweep
  endpoints) and the **Hold** path (no increments to park at loop time).
  *If grain motion comes back it should be doppler-based* — pitch shift is the cue
  the ear actually reads as movement, and it was deliberately avoided here.

- **0.2.3** — Removed **Glide** (glisson) after hardware testing — cut to keep the
  strip tight. Everything else from 0.2.2 unchanged.
- **0.2.2** — Four new sound features + one unlock. **Color**: per-grain low-pass
  cutoff scatter (the spectral Psprd) — dark and bright grains interleave in one
  cloud. **Glide**: glisson grains — each grain's pitch slides to a scattered
  destination over its life — the pitch analog of a spatial sweep. **Diffuse** (menu, off
  by default): four cascaded allpasses per channel on the wet path smear grain
  edges into a lush halo, and feed the regeneration loop so Feedback smears too.
  **Freeze** (menu): stops writing the capture ring — Hold freezes the *cloud*,
  Freeze freezes the *source*, with every control still live over the locked
  2.7 s (pair with Feedback for a self-sustaining instrument). And **Feedback now
  works in sample mode**: the ring becomes a regeneration bed and a
  feedback-sized share of grains granulates the unit's own output alongside the
  sample. Everything new is hard-bypassed at its default — all off = bit-identical
  render path to 0.2.1.
- **0.2.1** — Code-review fixes on 0.2.0 (sound-identical in normal use; install
  alongside 0.2.0 to A/B). Fixed **Orbit + Hold**: held orbiting grains kept
  integrating their travel every loop — gains grew without bound into a distorted
  drone (and with Binaural up could mute a channel); they now park at their travel
  destination. **Live-mode multi-playhead** offsets now scale into the ring room
  remaining behind the Playhead position, so at high playh + HeadSpread the "older"
  heads no longer wrap onto the freshest material (head order stays monotonic;
  spacing narrows gracefully toward playh = 1). **Snap** now runs at most 2 onset
  searches per audio block — bounds the worst-case CPU (~16×) at extreme
  density/short-grain settings where snapping is inaudible anyway; normal densities
  never hit the cap. Test harness compiles again as documented (public accessors
  instead of private pokes) and gained an Orbit+Hold regression case.
- **0.2.0** — Three new sound features and three display/UI changes. Everything new
  is off or neutral by default, so a v0.1.18 patch sounds the same.

  *Sound:*

  **Snap** *(new control, 0–1, default 0)* — locks grain starts to transients.
  A coarse→fine onset search at spawn (3 stages × 12 probes over ±75 ms, scoring
  energy-rise) pulls each grain's read position onto the nearest hit. Turns a
  granulated drum loop back into hits instead of mush. Verified **3.2× closer** to
  onsets on a click track. Spawn-time only; hard-bypassed at 0.

  **Orbit** *(new control, 0–1, default 0)* — grains travel the field over their
  life instead of holding one pan. Pan gains and the binaural head-shadow are
  lerped together, so the timbre moves with the position; ITD deliberately stays
  fixed (sweeping it would doppler the read). Two adds per sample, and only for
  grains actually in motion. **Density is the lever** — every grain travels a random
  direction, so dense clouds average into a blur; try `dens 2 / grains 3 / gLen
  500 ms`. Grain length sets the rate, not the distance.

  **Multiple playheads** *(new controls `Heads` / `HeadSpread` / `HeadTune`, hidden
  by default)* — N read points share one buffer, grains dealt round-robin, each head
  offset in position and transposed by a fixed interval. Instant granular chords, or
  one phrase heard at several points in its own history at once. **Spawn-time only**
  — no per-sample cost and no extra grains, so the `Grains` cap and density are
  unchanged. Gated behind a new **Playheads** menu option (`single` / `multi`,
  default `single`) to keep the strip short: `single` hides the three controls *and*
  hard-bypasses the DSP, so a stray Heads CV can't engage it.

  *Display and UI:*

  **Mode-aware control strip** — `mix` and `fdbk` are hidden once a sample is
  attached, `speed` is hidden in live mode. All three are already inert in the wrong
  mode at the DSP level, so this removes dead knobs rather than changing behaviour.

  **Shift-tap the grain field to switch its X axis** from stereo position to
  buffer read position — the first display that shows *where* grains are reading, so
  Snap and multi-playhead are finally visible rather than inferred. Sub display swaps
  to an axis legend. No new control, no strip slot.

  **Texture is now readable in the grain field** — the glyphs morph with the knob:
  a leaning wedge at Percussive, a symmetric arrowhead at Hann, a flat-topped
  trapezoid at Tukey. No new UI element, just a more informative display.

  *Note:* with everything at defaults this release is numerically equivalent to
  v0.1.18 but no longer bit-identical — max deviation 3.5e-5 (≈ −89 dBFS), traced to
  FMA reassociation in the multi-playhead transpose expression under `-ffast-math`,
  not a behaviour change.

- **0.1.18** — **Speed**: playhead scan control, decoupling time from pitch (sample
  mode). Grain pitch stays per-grain read speed; Speed moves where grains spawn —
  1 = original tempo, 0.5 = half-time, negative = reverse, 0 = parked (identical to
  before, bit-verified). Granular time-stretch in the Clouds/Nebulae tradition:
  PosJtr/Psprd tame the comb at pitch 0, GrainLen sets the stutter-vs-wash character
  of slowed transients. Costs ~nothing (block-rate accumulator). Keep Speed at 0
  when CV-modulating Playhead (knob/CV changes re-seat the scan).
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
