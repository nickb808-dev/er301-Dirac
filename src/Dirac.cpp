/* Dirac.cpp — Classic (time-domain) Granular Synthesizer for the ER-301
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * Grains are read directly from the source (sample at the Playhead, or the live
 * capture ring) with linear interpolation, windowed by a morphable envelope,
 * pitched by playback speed, and overlap-added. Feedback reinjects output into
 * the capture ring for regenerating clouds. No FFT. See Dirac.h. */

#include "Dirac.h"

#include <algorithm>
#include <cmath>

namespace dirac {

// C++11: static constexpr arrays are ODR-used when indexed in a loop, so they
// need out-of-line definitions (C++17 would make them implicitly inline).
constexpr int Dirac::kApSizeL[Dirac::kApStages];
constexpr int Dirac::kApSizeR[Dirac::kApStages];

// Finiteness guard (see planck v0.8.2). A NaN/Inf from a corrupt sample read or a
// glitchy input poisons the capture ring + feedback loop (tanh(NaN)=NaN) and held
// grains → non-finite output that escapes downstream and survives deleting the unit.
// BIT test on the exponent (all 1s ⇒ Inf/NaN) — bit-level because -ffast-math
// optimizes away `x==x` / isnan().
static inline float sanitize(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    return ((v.u & 0x7F800000u) == 0x7F800000u) ? 0.0f : x;
}

// Final-stage soft limiter (see planck v0.8.1). Dirac's Level already attenuates
// per grain, but dense overlap can still sum past ±1.0; this is transparent below
// ±0.9 and asymptotes to ±1.0 above so the output can't hard-clip the DAC.
static inline float softLimit(float x)
{
    const float T  = 0.9f;
    const float ax = (x >= 0.0f) ? x : -x;
    if (ax <= T) return x;
    const float e   = ax - T;
    const float lim = T + (1.0f - T) * (e / (e + (1.0f - T)));
    return (x >= 0.0f) ? lim : -lim;
}

// Snap a semitone offset to the nearest degree of a musical scale (0 = off).
// Scales: 1 Chromatic · 2 Major · 3 Minor · 4 Penta-maj · 5 Penta-min · 6 Whole-tone.
static inline float snapToScale(float semis, int scale)
{
    if (scale <= 0) return semis;
    static const int kDeg[6][12] = {
        {0,1,2,3,4,5,6,7,8,9,10,11},
        {0,2,4,5,7,9,11, 0,0,0,0,0},
        {0,2,3,5,7,8,10, 0,0,0,0,0},
        {0,2,4,7,9, 0,0,0,0,0,0,0},
        {0,3,5,7,10, 0,0,0,0,0,0,0},
        {0,2,4,6,8,10, 0,0,0,0,0,0},
    };
    static const int kLen[6] = {12, 7, 7, 5, 5, 6};
    if (scale > 6) scale = 6;
    const int *deg = kDeg[scale - 1];
    const int  n   = kLen[scale - 1];
    const float oct  = floorf(semis / 12.0f);
    const float frac = semis - oct * 12.0f;              // [0, 12)
    float bestD = float(deg[0]), bestDist = fabsf(frac - float(deg[0]));
    for (int i = 1; i < n; ++i) {
        const float d = fabsf(frac - float(deg[i]));
        if (d < bestDist) { bestDist = d; bestD = float(deg[i]); }
    }
    const float dwrap = fabsf(frac - 12.0f);             // nearest may be next-octave root
    if (dwrap < bestDist) bestD = 12.0f;
    return oct * 12.0f + bestD;
}

/* ── ctor / dtor ───────────────────────────────────────────────────────────── */

Dirac::Dirac()
{
    addOutput(mOutL);
    addOutput(mOutR);
    addInput(mIn);
    addInput(mPlayheadIn);
    addInput(mRateIn);
    addInput(mPosJtrIn);
    addInput(mSprdIn);
    addInput(mDetuneIn);
    addInput(mLevelIn);
    addInput(mRevProbIn);
    addInput(mPsprdIn);
    addInput(mTextureIn);
    addInput(mGrainsIn);
    addInput(mFireIn);
    addInput(mHoldIn);
    addInput(mSemiShiftIn);
    addInput(mGrainLenIn);
    addInput(mMixIn);
    addInput(mFeedbackIn);
    addInput(mCompressIn);
    addInput(mBinauralIn);
    addInput(mScaleIn);
    addInput(mVoctIn);
    addInput(mSpeedIn);   // appended last (v0.1.18) — preserves existing port indices
    addOption(mMultiHeadOpt);  // v0.2.0 — multi-playhead enable (default Off)
    addInput(mHeadsIn);        // v0.2.0 — appended after Speed, same reasoning
    addInput(mHeadSpreadIn);
    addInput(mHeadTuneIn);
    addInput(mSnapIn);         // v0.2.0 — onset-locked spawning
    addInput(mColorIn);        // v0.2.2 — per-grain cutoff scatter
    addInput(mTiltIn);         // v0.2.10 — envelope peak position (appended)
    addInput(mTiltSprdIn);     // v0.2.10 — per-grain tilt scatter
    addInput(mWindowIn);       // v0.2.10 — grain shape select (appended last)
    addOption(mDiffuseOpt);    // v0.2.2 — wet-path allpass diffuser (menu)
    addOption(mFreezeOpt);     // v0.2.2 — capture-ring write freeze (menu)

    mCapBuf.assign(kCapBufSize, 0.0f);
    mFeedBuf.assign(FRAMELENGTH, 0.0f);
    for (int k = 0; k < kApStages; ++k) {          // v0.2.2 diffuser delay lines
        mApBufL[k].assign(size_t(kApSizeL[k]), 0.0f);
        mApBufR[k].assign(size_t(kApSizeR[k]), 0.0f);
    }
    mPercTable.assign(kEnvTableSize + 1, 0.0f);
    mHannTable.assign(kEnvTableSize + 1, 0.0f);
    mTukeyTable.assign(kEnvTableSize + 1, 0.0f);
    for (int sh = 0; sh < kWinShapes; ++sh)         // v0.2.10 window banks
        for (int v = 0; v < kWinVariants; ++v)
            mWinBank[sh][v].assign(kEnvTableSize + 1, 0.0f);
    buildEnvTables();
}

Dirac::~Dirac() {}

void Dirac::setSample(od::Sample *sample)
{
    od::Head::setSample(sample);
    mEndIndex = sample ? int(sample->mSampleCount) : 0;
}

/* ── PRNG ──────────────────────────────────────────────────────────────────── */

float Dirac::randUnipolar()
{
    mRandState ^= mRandState << 13;
    mRandState ^= mRandState >> 17;
    mRandState ^= mRandState << 5;
    return float(mRandState & 0x7FFFFFFFu) / float(0x7FFFFFFFu);
}
float Dirac::randBipolar() { return randUnipolar() * 2.0f - 1.0f; }

/* ── envelope tables ───────────────────────────────────────────────────────── */
// Build the THREE shape tables ONCE. Texture (per grain, at spawn) blends two of
// them:  Percussive (0) → Hann (0.5) → Tukey (1).
//   • Percussive: fast attack + exponential decay → crisp transient "bits."
//   • Hann (0.5, default): smooth rounded grain.
//   • Tukey (1): flat top → fuller/sustained grains.
// Read by phase in [0, kEnvTableSize]; index kEnvTableSize is a 0 guard point.
void Dirac::buildEnvTables()
{
    const float taper = 0.125f;   // Tukey cosine-taper fraction at each end
    const float atk   = 0.02f;    // percussive attack fraction
    const float rel   = 0.04f;    // percussive release-to-zero taper (click-free end)
    // Exponential decay built incrementally (one expf for the ratio, then a multiply).
    const float decayRatio = expf(-4.0f / ((1.0f - atk) * float(kEnvTableSize)));
    const float invRel     = 1.0f / rel;
    float decayVal = 1.0f;
    bool  inDecay  = false;
    for (int i = 0; i <= kEnvTableSize; ++i) {
        const float t = float(i) / float(kEnvTableSize);

        mHannTable[i] = 0.5f * (1.0f - cosf(kTwoPi * t));

        if      (t < taper)        mTukeyTable[i] = 0.5f * (1.0f - cosf(kPi * t / taper));
        else if (t > 1.0f - taper) mTukeyTable[i] = 0.5f * (1.0f - cosf(kPi * (1.0f - t) / taper));
        else                       mTukeyTable[i] = 1.0f;

        float perc;
        if (t < atk) {
            perc = t / atk;
        } else {
            if (!inDecay) { inDecay = true; decayVal = 1.0f; }
            else          { decayVal *= decayRatio; }
            perc = decayVal;
        }
        if (t > 1.0f - rel) perc *= 0.5f * (1.0f - cosf(kPi * (1.0f - t) * invRel));
        mPercTable[i] = perc;
    }
    // Guarantee click-free onset & tail on every shape.
    mPercTable[0] = mHannTable[0] = mTukeyTable[0] = 0.0f;
    mPercTable[kEnvTableSize] = mHannTable[kEnvTableSize] = mTukeyTable[kEnvTableSize] = 0.0f;

    // ── Window BANKS (v0.2.10): 5 variants per fixed shape, built ONCE.
    // Variant 2 is the canonical v0.2.9 shape (textr 0.5 = same table); the
    // Texture dial/CV blends adjacent variants at render — in each Window
    // mode Texture is that shape's own macro parameter:
    //   bell width | sinc ring density (2..9 lobes) | tri curvature |
    //   decay fall time | ramp rise time | tukey taper | square edge.
    // Init-only trig/exp cost; NO rebuilds at runtime (v0.1.13 law).
    {
        const float kSigma [kWinVariants] = {0.06f, 0.09f, 0.13f, 0.18f, 0.25f};  // bell
        const float kLobes [kWinVariants] = {2.0f, 3.0f, 4.0f, 6.0f, 9.0f};       // sinc
        const float kTriPow[kWinVariants] = {2.5f, 1.6f, 1.0f, 0.7f, 0.45f};      // tri
        const float kFallK [kWinVariants] = {12.0f, 8.5f, 6.0f, 4.0f, 2.5f};      // decay/ramp
        const float kTaper [kWinVariants] = {0.02f, 0.06f, 0.125f, 0.25f, 0.5f};  // tukey
        const float kSqEdge[kWinVariants] = {0.003f, 0.006f, 0.01f, 0.03f, 0.08f};// square
        const float kEdge   = 0.01f;    // decay/ramp attack-edge taper
        const float invEdge = 1.0f / kEdge;
        for (int v = 0; v < kWinVariants; ++v) {
            float *bell = mWinBank[0][v].data(), *sinc = mWinBank[1][v].data(),
                  *tri  = mWinBank[2][v].data(), *dec  = mWinBank[3][v].data(),
                  *ramp = mWinBank[4][v].data(), *tuk  = mWinBank[5][v].data(),
                  *sq   = mWinBank[6][v].data();
            const float sqE = kSqEdge[v], invSqE = 1.0f / sqE;
            const float tap = kTaper[v];
            for (int i = 0; i <= kEnvTableSize; ++i) {
                const float t = float(i) / float(kEnvTableSize);
                {   // BELL — Gaussian, width by variant
                    const float z = (t - 0.5f) / kSigma[v];
                    bell[i] = expf(-0.5f * z * z);
                }
                {   // SINC — SIGNED ringing, lobe count by variant
                    const float x = (t - 0.5f) * 2.0f * kLobes[v];
                    sinc[i] = (fabsf(x) < 1e-6f) ? 1.0f : sinf(kPi * x) / (kPi * x);
                }
                {   // TRI — curvature by variant (spike .. linear .. bulge)
                    tri[i] = powf(1.0f - fabsf(2.0f * t - 1.0f), kTriPow[v]);
                }
                {   // DECAY / RAMP — fall/rise time by variant, tapered edges
                    const float eIn  = (t < kEdge)        ? 0.5f * (1.0f - cosf(kPi * t * invEdge))          : 1.0f;
                    const float eOut = (t > 1.0f - kEdge) ? 0.5f * (1.0f - cosf(kPi * (1.0f - t) * invEdge)) : 1.0f;
                    dec[i]  = eIn  * expf(-kFallK[v] * t);
                    ramp[i] = eOut * expf(-kFallK[v] * (1.0f - t));
                }
                {   // TUKEY — taper fraction by variant (near-square .. hann)
                    if      (t < tap)        tuk[i] = 0.5f * (1.0f - cosf(kPi * t / tap));
                    else if (t > 1.0f - tap) tuk[i] = 0.5f * (1.0f - cosf(kPi * (1.0f - t) / tap));
                    else                     tuk[i] = 1.0f;
                }
                {   // SQUARE — edge hardness by variant
                    const float eIn  = (t < sqE)        ? 0.5f * (1.0f - cosf(kPi * t * invSqE))          : 1.0f;
                    const float eOut = (t > 1.0f - sqE) ? 0.5f * (1.0f - cosf(kPi * (1.0f - t) * invSqE)) : 1.0f;
                    sq[i] = eIn * eOut;
                }
            }
            for (int sh = 0; sh < kWinShapes; ++sh) {   // click-free guarantees
                mWinBank[sh][v][0] = 0.0f;
                mWinBank[sh][v][kEnvTableSize] = 0.0f;
            }
        }
    }
}

/* ── source read (linear interpolation) ───────────────────────────────────── */
// readHoisted (v0.1.17): the interpolated read with all object state passed in as
// plain args. The render loop pre-loads cap/sd/nc ONCE per grain per block — the
// out-buffer stores are same-typed float writes, so the compiler could not hoist
// the mpSample->mpData / mChannelCount / mCapBuf loads out of the hot loop itself.
// v0.2.11: the position arrives as a whole sample + a fraction, so there is
// no floorf here — on the A8 (no VFP round instruction) floorf is a libm CALL,
// and this ran it once per source read: up to ~4000 calls a block.
inline float Dirac::readHoisted(const float *cap, const float *sd, int nc,
                                int sampleCount, bool live, int i0, float f)
{
    if (live) {
        i0 &= (kCapBufSize - 1);
        const int i1 = (i0 + 1) & (kCapBufSize - 1);
        return cap[i0] + f * (cap[i1] - cap[i0]);
    }
    if (!sd || sampleCount <= 0) return 0.0f;
    // i0 is kept in [0, sampleCount) by the render loop, so NO per-sample modulo
    // (the A8 has no integer divide). i1 wraps at the end.
    // Defensive clamp: the render loop's single compare-subtract wrap assumes the
    // pitch increment is smaller than the sample. A tiny sample + extreme pitch
    // (high SemiShift/Psprd) can leave i0 ≥ sampleCount → an out-of-bounds read.
    // Clamp here (cheap compares, no divide) so a degenerate input stays safe.
    if      (i0 >= sampleCount) i0 = sampleCount - 1;
    else if (i0 < 0)            i0 = 0;
    const int i1 = (i0 + 1 >= sampleCount) ? 0 : i0 + 1;
    if (nc <= 1) return sd[i0] + f * (sd[i1] - sd[i0]);
    const float *p0 = sd + size_t(i0) * nc;
    const float *p1 = sd + size_t(i1) * nc;
    const float s0 = 0.5f * (p0[0] + p0[1]);
    const float s1 = 0.5f * (p1[0] + p1[1]);
    return s0 + f * (s1 - s0);
}

// Convenience wrapper for cold paths (spawn-time compress peek).
inline float Dirac::readSrc(float pos, bool live, int sampleCount) const
{
    const float fp = floorf(pos);
    return readHoisted(mCapBuf.data(), mpSample ? mpSample->mpData : nullptr,
                       mpSample ? int(mpSample->mChannelCount) : 1,
                       sampleCount, live, int(fp), pos - fp);
}

/* ── findSlot ──────────────────────────────────────────────────────────────── */
// Return a free grain slot, or -1 if at capacity. We deliberately do NOT steal an
// active grain: hard-cutting a still-playing grain mid-content is an audible click
// (especially on complex audio), and it's exactly what made raising GrainLen or
// lowering Grains click. At capacity we simply skip the spawn — existing grains
// finish naturally and density self-limits to the Grains cap (click-free).
int Dirac::findSlot(int grainsCap)
{
    int nActive = 0;
    for (int i = 0; i < kMaxGrains; ++i) if (mGrains[i].active) nActive++;
    if (nActive >= grainsCap) return -1;
    for (int i = 0; i < kMaxGrains; ++i) if (!mGrains[i].active) return i;
    return -1;
}

/* ── spawnGrain ────────────────────────────────────────────────────────────── */
void Dirac::spawnGrain(float playhead, int sampleCount, float posJtr, float sprd,
                       float detune, float level, bool reverseProb,
                       float psprd, int grainsCap, bool held,
                       float semiShift, int grainDuration, float hopSamples,
                       int sampleOffset, bool liveMode, float compress, float binaural,
                       int scaleSel, int heads, float headSpread, float headTune,
                       float snap,
                       float feedback, float color, float tilt, float tiltSprd)
{
    const int slot = findSlot(grainsCap);
    if (slot < 0) return;
    Grain &g = mGrains[slot];

    // ── Sample-mode feedback (v0.2.2): regeneration grains ─────────────────
    // In live mode every grain reads the ring, so feedback regenerates by
    // construction. In sample mode the ring now captures the wet output too
    // (see process()), and a FEEDBACK-SIZED SHARE of new grains reads the ring
    // instead of the sample: fdbk 0.3 → ~30% regen grains mixed into the cloud,
    // 1.0 → the cloud granulates almost only its own output. Per-grain source
    // choice keeps the render loop untouched (g.live already branches the read).
    bool srcLive = liveMode;
    if (!liveMode && feedback > 0.0001f && randUnipolar() < feedback)
        srcLive = true;

    // ── Multi-playhead (v0.2.0) ────────────────────────────────────────────
    // Deal this grain to the next of N playheads (round robin). Head h reads the
    // SAME buffer, offset in position and transposed by h·HeadTune. At
    // HeadSpread = 1 the heads sit evenly distributed across the whole buffer.
    // heads <= 1 short-circuits to the single-head path (bit-identical to v0.1.18)
    // and costs nothing per sample — this is spawn-time only.
    int   head    = 0;
    float headOff = 0.0f;      // position offset, in normalised buffer units
    if (heads > 1) {
        head = mHeadCycle % heads;
        mHeadCycle = (mHeadCycle + 1) % heads;
        headOff = (float(head) * headSpread) / float(heads);
    }

    const float overlap  = (hopSamples > 0.0f) ? float(grainDuration) / hopSamples : 1.0f;
    const float densComp = (overlap > 1.0f) ? (1.0f / sqrtf(overlap)) : 1.0f;
    float       scale    = level * densComp;
    // Tilt (v0.2.10): the envelope phase reaches the TABLE MIDPOINT after
    // tilt·duration samples and the end after the remainder — two rates, one
    // compare at render. tilt 0.5 → both rates equal the classic
    // kEnvTableSize/duration EXACTLY (power-of-two scaling is lossless), so
    // the default is bit-identical to v0.2.9.
    // TiltSpread: each grain draws its own peak position around Tilt. Gated
    // so tsprd 0 makes no PRNG draw (bit-identity, and Psprd et al. keep
    // their random sequence).
    float tiltEff = tilt;
    if (tiltSprd > 0.0001f) {
        tiltEff += tiltSprd * randBipolar();
        if (tiltEff < -1.0f) tiltEff = -1.0f; else if (tiltEff > 1.0f) tiltEff = 1.0f;
    }
    float tiltPos = 0.5f + 0.5f * tiltEff;          // −1..1 → peak position 0..1
    const float tiltCl   = (tiltPos < 0.02f) ? 0.02f : (tiltPos > 0.98f ? 0.98f : tiltPos);
    const float kHalfTab = float(kEnvTableSize / 2);
    const float envIncrA = kHalfTab / (tiltCl * float(grainDuration));
    const float envIncrB = kHalfTab / ((1.0f - tiltCl) * float(grainDuration));
    const bool  detuned  = (detune > 0.0001f);
    const float detuneRatio = detuned ? expf(kLog2Over12 * detune) : 1.0f;

    // Base read position (sample index, or ring index for the live scan).
    //
    // Live reads anchor BEHIND the write head by a delay, so grains pick up the
    // freshly fed-back output (written just behind mCapWrite) and Feedback actually
    // regenerates. Reading forward from there marches toward the write head; the
    // edge-fade guards the collision. Playhead is a DELAY-TIME control in live mode:
    //   0 → freshest feedback (one grain-length back) = tightest regeneration
    //   1 → longest delay (most of the ring)          = long echo / smear
    float headGap = 0.0f;   // spacing between adjacent playheads (Snap lane width)
    // double (v0.2.11): a float position is 0.25 samples coarse at 60 s, and
    // the fractional ITD / jitter below would be rounded away. Spawn-time
    // only, so double costs nothing that matters.
    double basePos;
    if (liveMode) {
        const float minDelay = float(grainDuration);
        const float maxDelay = float(kCapBufSize - grainDuration);
        const float delay    = minDelay + playhead * (maxDelay - minDelay);
        basePos = double(mCapWrite) - double(delay);   // negative is fine: placed into the ring below
        // Head offset = further back in the ring (older material per head), scaled
        // into the ROOM remaining behind the knob position (v0.2.1). Scaling by the
        // full span let delay + offset exceed maxDelay near playh 1 — the ring mask
        // then wrapped an "older" head onto the FRESHEST material, right in the
        // write-head collision zone, and head order scrambled. With `room`, heads
        // always fit, order stays monotonic, and spacing shrinks gracefully to
        // stacked at playh = 1 (the same state HeadSpread ≈ 0 means).
        const float room = maxDelay - delay;
        basePos -= double(headOff * room);
        headGap  = (heads > 1) ? (headSpread / float(heads)) * room : 0.0f;
    } else if (srcLive) {
        // Sample-mode REGEN grain (v0.2.2): tight anchor just behind the write
        // head, where the freshly reinjected wet output lives — the same
        // "freshest feedback" position live mode uses at playh 0. Playhead
        // stays the SAMPLE position control; head offsets don't apply (the
        // head's pitch transpose still does, so regen grains join the chord).
        // The 4800-sample margin keeps posJtr's bipolar spray (±4800·posJtr)
        // behind the write head; the seam fade guards the rest.
        basePos = double(mCapWrite) - double(grainDuration) - 4800.0;
    } else {
        const double span = double(sampleCount > 1 ? sampleCount - 1 : 0);
        basePos = double(playhead) * span + double(headOff) * span;
        if (sampleCount > 0)                  // wrap the head offset into the sample
            basePos -= double(sampleCount) * floor(basePos / double(sampleCount));
        headGap  = (heads > 1) ? float((headSpread / float(heads)) * span) : 0.0f;
    }

    // Pitch offset from the played root = Psprd scatter + this head's chord rung
    // (HeadTune = 7 → root, +7, +14 …). With Scale > 0 the COMBINED offset snaps
    // to the scale (v0.2.7): the htune ladder becomes DIATONIC — htune 4 in
    // major voices 0-4-7-12 (a real triad, not the chromatic 0-4-8-12) — and the
    // scatter lands on chord tones instead of smearing around them. Quantized
    // BEFORE semiShift (same choice as v0.1.14 Psprd): the ROOT stays free —
    // V/Oct melodies are never snapped, the voicing transposes as a fixed shape.
    // Scale 0, or heads = 1 with any scale, is bit-identical to v0.2.6.
    float offSemi = (psprd > 0.0f) ? (randBipolar() * psprd) : 0.0f;
    offSemi += float(head) * headTune;
    if (scaleSel > 0) offSemi = snapToScale(offSemi, scaleSel);
    const float semis     = semiShift + offSemi;
    const float incL      = expf(kLog2Over12 * semis);   // 2^(semis/12), matches detuneRatio

    const float jit      = (posJtr > 0.0f) ? (posJtr * 4800.0f * randBipolar()) : 0.0f;
    double      startPos = basePos + double(jit);

    // ── SNAP: onset-locked spawning (v0.2.0) ───────────────────────────────
    // Normally a grain reads wherever the metronome puts it, so granulating
    // rhythmic material lands in the gaps as often as on the hits. Snap scans a
    // window around the target for the strongest ENERGY RISE (a transient) and
    // pulls the read there: 0 = free (classic), 1 = locked to the nearest onset.
    // Same cheap spawn-time buffer peek Compress uses — no per-sample cost.
    //
    // BUDGET (v0.2.1): the search is 360 source reads (3 stages × 12 candidates ×
    // 10 taps); worst-case spawn rate (64-sample grains at overlap 16 ≈ 12k/s)
    // made that ~4.3M reads/s — a CPU spike in exactly the short-grain case where
    // snapping is inaudible anyway (grains shorter than the transient). At most
    // kSnapPerBlock searches run per block; grains past the budget spawn unsnapped
    // for that block. Musical densities (a few spawns per block) never hit the cap.
    // NOTE: per-spawn (not cached per block) is deliberate — posJtr sprays each
    // grain to a different region and Snap reels each onto a DIFFERENT hit; a
    // shared cached result would collapse that variety (README quick start #7).
    if (snap > 0.0001f && mSnapBudget > 0) {
        --mSnapBudget;
        // COARSE → FINE search. Each stage tests kCand positions across a window,
        // then the next stage re-searches a window one grid-step wide around the
        // winner. Three stages of 12 give ~30-sample resolution over ±75 ms for
        // the cost of 36 probes — a flat grid that fine would need 240.
        const int   kCand  = 12;
        const int   kTaps  = 5;                     // energy taps either side
        const float kStep  = 40.0f;                 // tap spacing (samples) → ±200 window
        float       win    = 3600.0f;               // ±75 ms, shrinks each stage
        // Keep each head in its OWN lane. Without this the ±75 ms window is wider
        // than the head spacing at moderate HeadSpread, so heads wander onto each
        // other's transients and the position ladder collapses into clusters.
        // Below kMinLane the heads are effectively stacked anyway — let them
        // converge on a shared onset (that's what HeadSpread ≈ 0 is for).
        const float kMinLane = 400.0f;
        if (headGap > 0.0f) {
            const float lane = headGap * 0.5f;
            if (lane >= kMinLane && lane < win) win = lane;
        }
        const float sp0 = float(startPos);
        float bestPos = sp0, bestScore = -1e30f;
        for (int stage = 0; stage < 3; ++stage) {
            const float centre = bestPos;
            float stagePos = centre, stageScore = -1e30f;
            for (int c = 0; c < kCand; ++c) {
                const float f    = (float(c) / float(kCand - 1)) * 2.0f - 1.0f;   // -1..+1
                float       cand = centre + win * f;
                if (!srcLive && sampleCount > 0)    // keep the probe inside the sample
                    cand -= float(sampleCount) * floorf(cand / float(sampleCount));
                float after = 0.0f, before = 0.0f;
                for (int k = 1; k <= kTaps; ++k) {
                    const float a = readSrc(cand + float(k) * kStep, srcLive, sampleCount);
                    const float b = readSrc(cand - float(k) * kStep, srcLive, sampleCount);
                    after  += a * a;
                    before += b * b;
                }
                const float score = after - before; // positive = rising edge
                if (score > stageScore) { stageScore = score; stagePos = cand; }
            }
            bestScore = stageScore; bestPos = stagePos;
            win *= 2.0f / float(kCand - 1);         // next window = one grid step
        }
        if (bestScore > 0.0f)                       // only pull toward a real rise
            startPos += double(snap * (bestPos - sp0));
    }

    // ── Per-grain leveling ("compression") ─────────────────────────────────
    // Peek the source where this grain will read, estimate its level, and apply a
    // makeup gain toward a target so quiet source bits still speak in sparse clouds
    // (and loud bits don't dominate). Clamped to ±12 dB; blended by `compress`
    // (0 = off/natural). 6 cheap reads at spawn — no per-sample cost.
    if (compress > 0.0001f) {
        const int   nPeek  = 6;
        const float span   = float(grainDuration) * incL;
        float sum = 0.0f;
        for (int k = 0; k < nPeek; ++k) {
            float pp = float(startPos) + span * (float(k) / float(nPeek - 1));
            if (!srcLive && sampleCount > 0)          // wrap to match the grain's read
                pp -= float(sampleCount) * floorf(pp / float(sampleCount));
            const float sv = readSrc(pp, srcLive, sampleCount);
            sum += sv * sv;
        }
        const float localRMS = sqrtf(sum / float(nPeek));
        const float kTarget  = 0.3f;
        float makeup = kTarget / std::max(localRMS, 0.02f);   // 0.02 floor: don't boost silence to noise
        if      (makeup > 4.0f)  makeup = 4.0f;               // +12 dB ceiling
        else if (makeup < 0.25f) makeup = 0.25f;              // −12 dB floor
        scale *= 1.0f + compress * (makeup - 1.0f);
    }

    g.duration   = grainDuration;
    g.age        = 0;
    g.envPhase   = 0.0f;
    g.envIncrA   = envIncrA;
    g.envIncrB   = envIncrB;
    g.envTex     = mCurTexture;   // capture Texture for this grain's life (no mid-grain jump)
    g.envShape   = int8_t(mCurWindow);   // v0.2.9: fixed window (0 = Texture morph)
    g.reverse    = reverseProb;   // per-spawn reverse decision (computed at call site)
    g.live       = srcLive;   // v0.2.2: regen grains read the ring in sample mode
    g.pitchIncrL = incL;
    g.pitchIncrR = incL * detuneRatio;
    g.scale      = scale;
    g.held       = held;

    // stereo pan (equal-power, centre = unity) — this is the azimuth proxy
    float pan = randBipolar() * sprd;
    if (pan < -1.0f) pan = -1.0f; else if (pan > 1.0f) pan = 1.0f;
    g.panPos = pan;
    g.gainL  = sqrtf(1.0f - pan);   // equal-power ILD
    g.gainR  = sqrtf(1.0f + pan);

    // ── Binaural (ITD + head-shadow), scaled by `binaural` [0,1] ───────────
    // Azimuth = pan·90°. ITD: the FAR ear reads an earlier (delayed) position,
    // ±31 samples at full side. Head shadow: a one-pole LP on the far ear whose
    // cutoff falls 20 kHz→4 kHz with |sin(az)|. binaural=0 → no offset, alpha=1
    // (transparent), i.e. plain stereo pan. Engages the separate R read.
    float delayL = 0.0f, delayR = 0.0f;
    g.lpStateL = 0.0f; g.lpStateR = 0.0f;
    g.lpAlphaL = 1.0f; g.lpAlphaR = 1.0f;
    if (binaural > 0.0001f) {
        const float sinAz = sinf(pan * (kPi * 0.5f));     // pan ±1 → ±90°
        const float itd   = kMaxITDSamples * sinAz * binaural;
        delayL = (itd > 0.0f) ?  itd : 0.0f;              // source right → left delayed
        delayR = (itd < 0.0f) ? -itd : 0.0f;
        const float shadow = fabsf(sinAz) * binaural;     // 0..1
        const float fc     = 20000.0f - 16000.0f * shadow;
        const float alpha  = 1.0f - expf(-kTwoPi * fc / float(kSampleRate));
        if (sinAz >= 0.0f) g.lpAlphaL = alpha;            // left is the far (shadowed) ear
        else               g.lpAlphaR = alpha;
        g.stereo = true;                                  // ITD needs the separate R read
    } else {
        g.stereo = detuned;
    }

    // ── COLOR (v0.2.2): per-grain cutoff scatter — the spectral Psprd ───────
    // Each grain draws a random low-pass cutoff, log-uniform from 20 kHz down to
    // ~600 Hz at Color = 1, so dark and bright grains interleave in one cloud.
    // Reuses the binaural head-shadow one-pole per ear (min-combine keeps the
    // darker of shadow vs colour). Render cost is the already-gated LP path;
    // Color 0 = bit-identical.
    if (color > 0.0001f) {
        const float u  = randUnipolar();
        const float fc = 20000.0f * expf(-3.5f * color * u);   // 20 kHz → ~600 Hz
        const float aC = 1.0f - expf(-kTwoPi * fc / float(kSampleRate));
        if (g.lpAlphaL > aC) g.lpAlphaL = aC;
        if (g.lpAlphaR > aC) g.lpAlphaR = aC;
    }


    // Place the read positions: whole sample (wrapped into the ring or the
    // sample) + fraction. floor() here is spawn-rate, not per sample.
    {
        const double pL = startPos - double(delayL), pR = startPos - double(delayR);
        const double pp[2] = { pL, pR };
        int idx[2]; float fr[2];
        for (int c = 0; c < 2; ++c) {
            double q = pp[c];
            if (!(q > -1.0e9 && q < 1.0e9)) q = 0.0;          // sticky-NaN guard
            if (!srcLive && sampleCount > 0)
                q -= double(sampleCount) * floor(q / double(sampleCount));
            const double fl = floor(q);
            int i = int(fl);
            float f = float(q - fl);
            if (!(f >= 0.0f && f < 1.0f)) f = 0.0f;
            if (srcLive) i &= (kCapBufSize - 1);
            else if (sampleCount > 0) { if (i >= sampleCount || i < 0) i = 0; }
            else i = 0;
            idx[c] = i; fr[c] = f;
        }
        g.idxL = g.idx0L = idx[0]; g.fracL = g.frac0L = fr[0];
        g.idxR = g.idx0R = idx[1]; g.fracR = g.frac0R = fr[1];
        g.srcIndex = idx[0];
    }

    g.startDelay = sampleOffset;
    g.pitchSemi  = semis;   // includes the head transpose → heads stack in the field viz
    g.active     = true;
}

/* ── process ───────────────────────────────────────────────────────────────── */
void Dirac::process()
{
    float *outL = mOutL.buffer();
    float *outR = mOutR.buffer();
    const int blockSize = FRAMELENGTH;

    for (int s = 0; s < blockSize; ++s) { outL[s] = 0.0f; outR[s] = 0.0f; }

    const int  sampleCount = mpSample ? int(mpSample->mSampleCount) : 0;
    const bool liveMode    = (sampleCount <= 0);

    // Every control read is sanitize()d (v0.2.11, standard §5). Until 0.2.10
    // NaN only fell out of the std::max(lo, std::min(x, hi)) clamps because
    // of the order those compare — the standard's warning, and a -ffast-math
    // build may assume no NaN at all. sanitize is the identity on finite
    // values, so nothing else changes. Host `nan` covers every input.
    // ── Parameters ─────────────────────────────────────────────────────────
    // "Rate" is now a DENSITY control = target grain overlap. 0 = trigger-only;
    // otherwise the spawn rate is derived from grain length so grains overlap (no
    // more silent/gappy low end). Floored at kDensFloor so it never near-stops.
    const float dens     = std::max(0.0f,  std::min(sanitize(mRateIn.buffer()[0]),   16.0f));
    const float playhead = std::max(0.0f,  std::min(sanitize(mPlayheadIn.buffer()[0]), 1.0f));
    const float posJtr   = std::max(0.0f,  std::min(sanitize(mPosJtrIn.buffer()[0]),  1.0f));
    const float sprd     = std::max(0.0f,  std::min(sanitize(mSprdIn.buffer()[0]),    1.0f));
    const float detune   = std::max(0.0f,  std::min(sanitize(mDetuneIn.buffer()[0]),  2.0f));
    const float level    = std::max(0.0f,  std::min(sanitize(mLevelIn.buffer()[0]),   1.0f));
    const float mix      = std::max(0.0f,  std::min(sanitize(mMixIn.buffer()[0]),     1.0f));
    const float feedback = std::max(0.0f,  std::min(sanitize(mFeedbackIn.buffer()[0]), 1.0f));
    const float compress = std::max(0.0f,  std::min(sanitize(mCompressIn.buffer()[0]), 1.0f));
    const float binaural = std::max(0.0f,  std::min(sanitize(mBinauralIn.buffer()[0]), 1.0f));
    const int   scale    = std::max(0, std::min(int(sanitize(mScaleIn.buffer()[0]) + 0.5f), 6));
    mVizSprd = sprd;  mVizDetune = detune;
    const float revprob  = std::max(0.0f,  std::min(sanitize(mRevProbIn.buffer()[0]), 1.0f));
    const float psprd    = std::max(0.0f,  std::min(sanitize(mPsprdIn.buffer()[0]),  12.0f));
    const float texture  = std::max(0.0f,  std::min(sanitize(mTextureIn.buffer()[0]), 1.0f));
    const int   grains   = std::max(1, std::min(int(sanitize(mGrainsIn.buffer()[0]) + 0.5f), kMaxGrains));
    const bool  holdOn   = (sanitize(mHoldIn.buffer()[0]) > 0.5f);
    // Transpose = SemiShift (coarse, integer semitones) + V/Oct (smooth, 1 V = 1 oct).
    // Clamped to ±4 octaves so extreme reads don't alias into garbage.
    const float semiKnob = roundf(std::max(-24.0f, std::min(sanitize(mSemiShiftIn.buffer()[0]), 24.0f)));
    const float voct     = sanitize(mVoctIn.buffer()[0]);
    const float semiShift= std::max(-kMaxTranspose,
                             std::min(semiKnob + voct * kVoctToSemis, kMaxTranspose));
    const float speed    = std::max(-4.0f, std::min(sanitize(mSpeedIn.buffer()[0]),  4.0f));
    // Multi-playhead (v0.2.0). heads = 1 → single-head path, bit-identical.
    // The Playheads option (menu) hard-bypasses the feature when Off, so a stray
    // Heads CV can't engage it and the spawn path stays on the classic branch.
    const bool  multiHead  = (mMultiHeadOpt.value() >= 2);
    const int   heads      = multiHead
                           ? std::max(1, std::min(int(sanitize(mHeadsIn.buffer()[0]) + 0.5f), kMaxHeads))
                           : 1;
    const float headSpread = std::max(0.0f, std::min(sanitize(mHeadSpreadIn.buffer()[0]), 1.0f));
    const float headTune   = std::max(-12.0f, std::min(sanitize(mHeadTuneIn.buffer()[0]), 12.0f));
    const float snap       = std::max(0.0f, std::min(sanitize(mSnapIn.buffer()[0]),  1.0f));
    const float color      = std::max(0.0f, std::min(sanitize(mColorIn.buffer()[0]), 1.0f));
    // Tilt is BIPOLAR: −1 attack … 0 symmetric (default/unpatched) … +1 swell.
    const float tilt       = std::max(-1.0f, std::min(sanitize(mTiltIn.buffer()[0]), 1.0f));
    const float tiltSprd   = std::max(0.0f, std::min(sanitize(mTiltSprdIn.buffer()[0]), 1.0f));
    const float grainLenSec = std::max(0.001f, std::min(sanitize(mGrainLenIn.buffer()[0]),
                                       float(kMaxGrainSamp) / float(kSampleRate)));
    const int   grainDuration = std::max(64, std::min(int(grainLenSec * float(kSampleRate)),
                                                      kMaxGrainSamp));
    const float *fireBuf = mFireIn.buffer();

    // ── Capture into the ring (+ feedback reinjection) ─────────────────────
    // v0.2.2: feedback now reinjects in BOTH modes. Live mode: grains read the
    // ring, so feedback regenerates by construction. Sample mode: the ring is the
    // REGENERATION BED — wet output is written here and a feedback-sized share of
    // grains reads it instead of the sample (see spawnGrain). With feedback at 0
    // the ring captures plain input (warm for detach) and pays no tanh/mix cost.
    // Freeze (menu, v0.2.2) stops ALL writes: the ring locks while every control
    // stays live — the source freezes, not the cloud (that's Hold).
    // Ring advance uses the power-of-two mask (kCapBufSize), not a compare-branch.
    const bool frozen = (mFreezeOpt.value() >= 2);
    if (!frozen) {
        const float *in = mIn.buffer();
        if (feedback > 0.0001f) {
            if (liveMode) {
                for (int s = 0; s < blockSize; ++s) {
                    mCapBuf[mCapWrite] = sanitize(in[s] + feedback * mFeedBuf[s]);
                    mCapWrite = (mCapWrite + 1) & (kCapBufSize - 1);
                }
            } else {
                // Sample mode (v0.2.6 fix): the ring is a PURE regeneration bed —
                // feedback only, NO lane input. Selecting a sample must isolate
                // the unit from its input; v0.2.2–0.2.5 mixed `in` here too, so
                // upstream units bled audibly into the cloud whenever fdbk was
                // up (host-verified: silent sample + lane tone + fdbk 0.6 →
                // output RMS 0.097; now 0). Regen self-bootstraps from the
                // sample grains' own wet output. Trade-off: while fdbk is up in
                // sample mode the ring tracks the regen tail, not recent input,
                // so a detach lands on that tail instead of a warm input
                // capture — correct, since that's what you were hearing.
                for (int s = 0; s < blockSize; ++s) {
                    mCapBuf[mCapWrite] = sanitize(feedback * mFeedBuf[s]);
                    mCapWrite = (mCapWrite + 1) & (kCapBufSize - 1);
                }
            }
        } else {
            for (int s = 0; s < blockSize; ++s) {
                mCapBuf[mCapWrite] = sanitize(in[s]);
                mCapWrite = (mCapWrite + 1) & (kCapBufSize - 1);
            }
        }
    }

    // Envelope tables are static (built once); each grain captures Texture at spawn
    // and blends its own shape at render, so moving Texture never rebuilds anything
    // and never disturbs a playing grain (no clicks).
    mCurTexture = texture;
    // Window shape (v0.2.10): read once per block; spawns capture it, so CV
    // here selects a shape PER GRAIN (S&H = shape scatter).
    {
        const int w = int(sanitize(mWindowIn.buffer()[0]) + 0.5f);
        mCurWindow = (w < 0) ? 0 : (w > kWinShapes ? kWinShapes : w);
    }

    // ── Playhead scan (v0.1.18): Speed decouples TIME from PITCH (sample mode).
    // Grain pitch is per-grain read speed (SemiShift/V-Oct/Psprd, unchanged); Speed
    // moves WHERE grains spawn: 0 = parked, the knob is the position (legacy,
    // bit-identical); ±1 = original tempo forward/reverse, wrapping at the ends.
    // Moving the Playhead knob re-seats the scan there. Live mode ignores Speed
    // (Playhead remains the feedback delay-time control). Block-rate only.
    float playheadEff = playhead;
    if (!liveMode) {
        if (speed != 0.0f && sampleCount > 1) {
            if (fabsf(playhead - mLastPlayheadKnob) > 0.0005f)
                mScanPos = double(playhead);          // knob touched → re-seat scan
            mScanPos += double(speed) * double(blockSize) / double(sampleCount);
            mScanPos -= floor(mScanPos);              // wrap [0,1)
            // sticky-NaN guard on the accumulator (positive test, §5)
            if (!(mScanPos >= 0.0 && mScanPos < 1.0)) mScanPos = double(playhead);
            playheadEff = float(mScanPos);
        } else {
            mScanPos = double(playhead);              // parked: follow the knob
        }
        mLastPlayheadKnob = playhead;
    }

    if (!liveMode) mCurrentIndex = int(playheadEff * float(sampleCount > 1 ? sampleCount - 1 : 0));
    else           mCurrentIndex = 0;

    // Density → spawn period. overlap = grains deep; hop = grainLen / overlap.
    // Floored at kDensFloor so even the lowest setting spawns regularly (no gaps
    // of silence). dens < 0.05 → trigger-only (free-run off).
    const bool  freeRun   = (dens >= 0.05f);
    const float overlap   = (dens > kDensFloor) ? dens : kDensFloor;
    const float hopSamples = freeRun ? (float(grainDuration) / overlap) : float(grainDuration);

    // ── Spawn scheduling: free-run at the density + trig edges (decoupled) ──
    mSnapBudget = kSnapPerBlock;   // v0.2.1: bound the per-block Snap search cost
    if (freeRun) {
        mSpawnTimer -= float(blockSize);
        while (mSpawnTimer <= 0.0f) {
            int spawnAt = blockSize + int(mSpawnTimer);
            if (spawnAt < 0)          spawnAt = 0;
            if (spawnAt >= blockSize) spawnAt = blockSize - 1;
            spawnGrain(playheadEff, sampleCount, posJtr, sprd, detune, level,
                       (revprob > 0.0f) && (randUnipolar() < revprob), psprd, grains,
                       holdOn, semiShift, grainDuration, hopSamples, spawnAt, liveMode, compress, binaural, scale,
                       heads, headSpread, headTune, snap, feedback, color, tilt, tiltSprd);
            mSpawnTimer += hopSamples;
        }
    } else {
        mSpawnTimer = 0.0f;
    }
    {
        float last = mLastFire;
        for (int s = 0; s < blockSize; ++s) {
            if (fireBuf[s] > 0.5f && last <= 0.5f) {
                spawnGrain(playheadEff, sampleCount, posJtr, sprd, detune, level,
                           (revprob > 0.0f) && (randUnipolar() < revprob), psprd, grains,
                           holdOn, semiShift, grainDuration, hopSamples, s, liveMode, compress, binaural, scale,
                           heads, headSpread, headTune, snap, feedback, color, tilt, tiltSprd);
            }
            last = fireBuf[s];
        }
        mLastFire = last;
    }

    // ── Render active grains (time-domain overlap-add) ─────────────────────
    // Source state hoisted out of the hot loop (see readHoisted).
    const float *capData = mCapBuf.data();
    const float *smpData = mpSample ? mpSample->mpData : nullptr;
    const int    smpNc   = mpSample ? int(mpSample->mChannelCount) : 1;

    for (int i = 0; i < kMaxGrains; ++i) {
        Grain &g = mGrains[i];
        if (!g.active) continue;

        // startDelay is the within-block spawn offset (always < blockSize), so it
        // takes effect this same block; it never needs to carry across blocks.
        const int outStart = g.startDelay;
        const int toRender = std::min(g.duration - g.age, blockSize - outStart);
        const bool  live = g.live;
        const bool  st   = g.stereo;
        const float gL   = g.gainL * g.scale;
        const float gR   = g.gainR * g.scale;
        const float incL = g.reverse ? -g.pitchIncrL : g.pitchIncrL;
        const float incR = g.reverse ? -g.pitchIncrR : g.pitchIncrR;
        int   iL = g.idxL, iR = g.idxR;
        float fL = g.fracL, fR = g.fracR, envPh = g.envPhase;
        const float envIncA = g.envIncrA, envIncB = g.envIncrB;   // hoisted (Tilt two-rate)
        const float kHalfT  = float(kEnvTableSize / 2);
        const float lpAL = g.lpAlphaL, lpAR = g.lpAlphaR;   // head-shadow LP (1 = transparent)
        float lpL = g.lpStateL, lpR = g.lpStateR;
        // Binaural head-shadow or Color engaged?
        const bool doShadow = (lpAL < 0.9999f) || (lpAR < 0.9999f);
        // This grain's envelope = blend of two static shape tables by its captured
        // Texture (perc→Hann below 0.5, Hann→Tukey above). Selected once per grain.
        // At an exact node (Texture 0 or the 0.5 default) ew == 0 → skip the second
        // table lerp entirely (bit-identical: ea + 0·(eb−ea) == ea).
        const float *eA, *eB; float ew;
        if (g.envShape > 0) {   // fixed Window: Texture blends the shape's BANK
            float pos = g.envTex * float(kWinVariants - 1);   // 0..4
            int   k   = int(pos);
            if (k > kWinVariants - 2) k = kWinVariants - 2;
            eA = mWinBank[g.envShape - 1][k].data();
            eB = mWinBank[g.envShape - 1][k + 1].data();
            ew = pos - float(k);   // textr 0.5 → pos 2.0, ew 0 → single-lerp path
        }
        else if (g.envTex < 0.5f) { eA = mPercTable.data(); eB = mHannTable.data();  ew = g.envTex * 2.0f; }
        else                      { eA = mHannTable.data(); eB = mTukeyTable.data(); ew = (g.envTex - 0.5f) * 2.0f; }
        const bool blend = (ew != 0.0f);

        // The sample can change under a grain (a new file, a different
        // length): re-wrap once per block if the index is out of range. Rare,
        // so the modulo is fine here and never runs per sample.
        const int nS = sampleCount;
        if (!live && nS > 0) {
            if (iL < 0 || iL >= nS) { iL %= nS; if (iL < 0) iL += nS; }
            if (iR < 0 || iR >= nS) { iR %= nS; if (iR < 0) iR += nS; }
        }

        // Edge fade (anti-click): fade the grain as its read nears a SEAM — the
        // write head in live mode (read/write collision guard) or the sample loop
        // boundary in sample mode (boundary crossfade). In a dense cloud the
        // overlapping grains cover the dip, so it reads as a crossfade. Skipped
        // for very short samples (no interior to fade toward).
        // v0.2.11: the distance is derived from the index every sample, not
        // carried in a float accumulator — that accumulator drifted on long
        // samples exactly as the read position did (a +1 st grain at 40 s
        // lost ~0.06 samples a step), so the fade landed away from the seam.
        const int   capW   = mCapWrite;
        const int   capM   = kCapBufSize - 1;
        const float period = live ? float(kCapBufSize) : float(nS);
        const float halfP  = 0.5f * period;
        const bool  doSeam = live ? true : (nS > int(4.0f * kSeamGuard));

        // Gate the per-sample edge fade: only run it when the read can actually come
        // within kSeamGuard of the seam THIS block. Most grains are far from the
        // seam → skip all the fade work (the common-case CPU saving). With Detune
        // the R read runs at its own speed, so it gets its own distance (v0.1.17).
        bool seamActive = false;
        if (doSeam) {
            const float relL = live ? float((iL - capW) & capM) + fL : float(iL) + fL;
            const float distL = (relL < halfP) ? relL : (period - relL);
            const float travL = ((incL >= 0.0f) ? incL : -incL) * float(toRender);
            seamActive = (distL <= kSeamGuard + travL + 1.0f);
            if (st && !seamActive) {
                const float relR = live ? float((iR - capW) & capM) + fR : float(iR) + fR;
                const float distR = (relR < halfP) ? relR : (period - relR);
                const float travR = ((incR >= 0.0f) ? incR : -incR) * float(toRender);
                seamActive = (distR <= kSeamGuard + travR + 1.0f);
            }
        }

        for (int s = 0; s < toRender; ++s) {
            int ei = int(envPh);
            // Defensive clamp (v0.1.17): envPh is a float ACCUMULATOR; on very long
            // grains the rounding drift can nudge the final reads up to
            // kEnvTableSize, and eA[ei + 1] would then read past the table.
            if (ei > kEnvTableSize - 1) ei = kEnvTableSize - 1;
            const float fr  = envPh - float(ei);
            float env = eA[ei] + fr * (eA[ei + 1] - eA[ei]);
            if (blend) {
                const float eb = eB[ei] + fr * (eB[ei + 1] - eB[ei]);
                env += ew * (eb - env);
            }
            float fadeL = 1.0f, fadeR = 1.0f;
            if (seamActive) {
                const float relL = live ? float((iL - capW) & capM) + fL : float(iL) + fL;
                const float distL = (relL < halfP) ? relL : (period - relL);
                if (distL < kSeamGuard) fadeL = distL * kInvSeamGuard;
                if (st) {                         // R fades on its own seam distance
                    const float relR = live ? float((iR - capW) & capM) + fR : float(iR) + fR;
                    const float distR = (relR < halfP) ? relR : (period - relR);
                    if (distR < kSeamGuard) fadeR = distR * kInvSeamGuard;
                } else {
                    fadeR = fadeL;
                }
            }
            const float eL = env * fadeL;
            const float eR = env * fadeR;
            const float sL = readHoisted(capData, smpData, smpNc, nS, live, iL, fL);
            const float sR = st ? readHoisted(capData, smpData, smpNc, nS, live, iR, fR) : sL;
            if (doShadow) {                       // binaural: one-pole LP on the far ear
                lpL += lpAL * (sL - lpL);
                lpR += lpAR * (sR - lpR);
                outL[outStart + s] += gL * eL * lpL;
                outR[outStart + s] += gR * eR * lpR;
            } else {                              // common case: no filter, no extra work
                outL[outStart + s] += gL * eL * sL;
                outR[outStart + s] += gR * eR * sR;
            }
            // advance: the fraction takes the step, whole samples move the
            // index (truncate, then correct toward -inf for reverse steps —
            // no floorf call)
            fL += incL;
            { int k = int(fL); if (fL < float(k)) --k; iL += k; fL -= float(k); }
            if (st) {
                fR += incR;
                int k = int(fR); if (fR < float(k)) --k; iR += k; fR -= float(k);
            }
            envPh += (envPh < kHalfT) ? envIncA : envIncB;   // Tilt: peak-relative rates
            if (live) {
                iL &= capM; iR &= capM;
            } else if (nS > 0) {
                // one subtract covers any step shorter than the sample; a tiny
                // sample at an extreme pitch takes the (rare) modulo
                if (iL >= nS) { iL -= nS; if (iL >= nS) iL %= nS; }
                else if (iL < 0) { iL += nS; if (iL < 0) { iL %= nS; if (iL < 0) iL += nS; } }
                if (iR >= nS) { iR -= nS; if (iR >= nS) iR %= nS; }
                else if (iR < 0) { iR += nS; if (iR < 0) { iR %= nS; if (iR < 0) iR += nS; } }
            }
        }
        g.idxL = iL; g.fracL = fL; g.idxR = iR; g.fracR = fR; g.envPhase = envPh;
        g.lpStateL = lpL;   g.lpStateR = lpR;
        g.age += toRender;
        g.startDelay = 0;
        if (g.age >= g.duration) {
            if (g.held && holdOn) {
                g.age = 0; g.envPhase = 0.0f;
                g.idxL = g.idx0L; g.fracL = g.frac0L;
                g.idxR = g.idx0R; g.fracR = g.frac0R;
            } else {
                g.active = false;
            }
        }
    }

    // ── Diffuse (v0.2.2, menu option): wet-path allpass halo ────────────────
    // 4 cascaded Schroeder allpasses per channel (mutually-prime delays, L ≠ R
    // for stereo decorrelation, ~49 ms total path) smear grain edges into a
    // lush tail — the Clouds-style halo. Runs BEFORE the feedback tap so
    // regeneration is diffused too (tanh still bounds the loop). Off = hard
    // bypass (bit-identical), and the delay lines clear on Off so re-enabling
    // never replays a stale tail. Inputs are sanitized so a NaN can never
    // poison the delay lines; the final output stage sanitizes again anyway.
    if (mDiffuseOpt.value() >= 2) {
        for (int s = 0; s < blockSize; ++s) {
            float xl = sanitize(outL[s]), xr = sanitize(outR[s]);
            for (int k = 0; k < kApStages; ++k) {
                { const int j = mApIdxL[k]; const float z = mApBufL[k][j];
                  const float y = z - kApGain * xl;
                  mApBufL[k][j] = xl + kApGain * y;
                  if (++mApIdxL[k] >= kApSizeL[k]) mApIdxL[k] = 0;
                  xl = y; }
                { const int j = mApIdxR[k]; const float z = mApBufR[k][j];
                  const float y = z - kApGain * xr;
                  mApBufR[k][j] = xr + kApGain * y;
                  if (++mApIdxR[k] >= kApSizeR[k]) mApIdxR[k] = 0;
                  xr = y; }
            }
            outL[s] = xl; outR[s] = xr;
        }
        mDiffusePrimed = true;
    } else if (mDiffusePrimed) {
        for (int k = 0; k < kApStages; ++k) {
            std::fill(mApBufL[k].begin(), mApBufL[k].end(), 0.0f);
            std::fill(mApBufR[k].begin(), mApBufR[k].end(), 0.0f);
            mApIdxL[k] = 0; mApIdxR[k] = 0;
        }
        mDiffusePrimed = false;
    }

    // ── Feedback source: soft-clipped wet mono (tanh keeps regeneration stable) ──
    // `drive` is a makeup gain INSIDE the tanh: the round-trip loses ~8 dB (densComp +
    // mono-sum + envelope average), so without it the loop never sustains even at
    // feedback≈1. Driving the tanh recovers that so feedback approaches unity loop gain
    // near 1.0; tanh still bounds mFeedBuf to [-1,1] → saturates into a stable drone,
    // no runaway. Host-tuned: 0=silent, ~0.8=clear tail, ~1=stable drone.
    if (feedback > 0.0001f && !frozen) {
        const float drive = 2.5f;
        // Loop high-pass, fc ≈ 120 Hz @ 48 kHz (v0.2.8). Not just a DC blocker:
        // the tanh RE-CREATES low-frequency intermodulation products every pass
        // (difference tones between the dense grain partials), and with multiple
        // heads the audio band combs to ~half loop gain while LF keeps winning —
        // the pedestal rails the tanh and squeezes the audio out ("fades like an
        // overload"). Host sweep 20/60/120 Hz: at 120 the 4-head audio fraction
        // holds at 0.45 after 60 s (0.18 at 20 Hz), pedestal 0.023. Same corner
        // class as the Clouds feedback path. Sonic cost: the LOOP thins below
        // ~120 Hz per pass; single passes keep full bandwidth.
        const float aHp   = 0.98429f;
        for (int s = 0; s < blockSize; ++s) {
            const float mono = 0.5f * (outL[s] + outR[s]);
            const float hp   = mono - mFbHpX + aHp * mFbHpY;   // y = x − x₁ + a·y₁
            mFbHpX = mono; mFbHpY = hp;
            mFeedBuf[s] = sanitize(tanhf(drive * hp));
        }
        mFeedPrimed = true;
    } else if (mFeedPrimed) {
        // Leaving the feedback path: clear so re-entering doesn't inject one block
        // of stale audio from whenever feedback last ran.
        std::fill(mFeedBuf.begin(), mFeedBuf.end(), 0.0f);
        mFbHpX = 0.0f; mFbHpY = 0.0f;   // v0.2.8: don't carry stale HP state back in
        mFeedPrimed = false;
    }

    // ── Wet/dry Mix (LIVE mode only; dry = unity input passthrough) ────────
    if (liveMode && mix < 0.99995f) {
        const float *in  = mIn.buffer();
        const float  dry = 1.0f - mix;
        for (int s = 0; s < blockSize; ++s) {
            const float d = in[s] * dry;
            outL[s] = outL[s] * mix + d;
            outR[s] = outR[s] * mix + d;
        }
    }

    // ── Final stage: scrub non-finite (downstream protection) then soft-limit so
    //    dense-overlap spikes never hard-clip the DAC. sanitize first (softLimit's
    //    compares are no-ops on NaN under -ffast-math). ──
    for (int s = 0; s < blockSize; ++s) {
        outL[s] = softLimit(sanitize(outL[s]));
        outR[s] = softLimit(sanitize(outR[s]));
    }
}

} // namespace dirac
