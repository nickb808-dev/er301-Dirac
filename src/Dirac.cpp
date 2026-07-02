/* Dirac.cpp — Classic (time-domain) Granular Synthesizer for the ER-301
 *
 * Grains are read directly from the source (sample at the Playhead, or the live
 * capture ring) with linear interpolation, windowed by a morphable envelope,
 * pitched by playback speed, and overlap-added. Feedback reinjects output into
 * the capture ring for regenerating clouds. No FFT. See Dirac.h. */

#include "Dirac.h"

#include <algorithm>
#include <cmath>

namespace dirac {

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

    mCapBuf.assign(kCapBufSize, 0.0f);
    mFeedBuf.assign(FRAMELENGTH, 0.0f);
    mPercTable.assign(kEnvTableSize + 1, 0.0f);
    mHannTable.assign(kEnvTableSize + 1, 0.0f);
    mTukeyTable.assign(kEnvTableSize + 1, 0.0f);
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
}

/* ── source read (linear interpolation) ───────────────────────────────────── */
// readHoisted (v0.1.17): the interpolated read with all object state passed in as
// plain args. The render loop pre-loads cap/sd/nc ONCE per grain per block — the
// out-buffer stores are same-typed float writes, so the compiler could not hoist
// the mpSample->mpData / mChannelCount / mCapBuf loads out of the hot loop itself.
inline float Dirac::readHoisted(const float *cap, const float *sd, int nc,
                                int sampleCount, bool live, float pos)
{
    const float fp = floorf(pos);
    const float f  = pos - fp;
    if (live) {
        const int i0 = int(fp) & (kCapBufSize - 1);
        const int i1 = (i0 + 1) & (kCapBufSize - 1);
        return cap[i0] + f * (cap[i1] - cap[i0]);
    }
    if (!sd || sampleCount <= 0) return 0.0f;
    // pos is kept in [0, sampleCount) by the render loop, so NO per-sample modulo
    // (the A8 has no integer divide). i0 is already in range; i1 wraps at the end.
    int i0 = int(fp);
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
    return readHoisted(mCapBuf.data(), mpSample ? mpSample->mpData : nullptr,
                       mpSample ? int(mpSample->mChannelCount) : 1,
                       sampleCount, live, pos);
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
                       int scaleSel)
{
    const int slot = findSlot(grainsCap);
    if (slot < 0) return;
    Grain &g = mGrains[slot];

    const float overlap  = (hopSamples > 0.0f) ? float(grainDuration) / hopSamples : 1.0f;
    const float densComp = (overlap > 1.0f) ? (1.0f / sqrtf(overlap)) : 1.0f;
    float       scale    = level * densComp;
    const float envIncr  = float(kEnvTableSize) / float(grainDuration);
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
    float basePos;
    if (liveMode) {
        const float minDelay = float(grainDuration);
        const float maxDelay = float(kCapBufSize - grainDuration);
        const float delay    = minDelay + playhead * (maxDelay - minDelay);
        basePos = float(mCapWrite) - delay;   // negative is fine: readSrc masks the ring
    } else {
        basePos = playhead * float(sampleCount > 1 ? sampleCount - 1 : 0);
    }

    float psprdSemi = (psprd > 0.0f) ? (randBipolar() * psprd) : 0.0f;
    if (scaleSel > 0) psprdSemi = snapToScale(psprdSemi, scaleSel);   // quantize scatter to a scale
    const float semis     = semiShift + psprdSemi;
    const float incL      = expf(kLog2Over12 * semis);   // 2^(semis/12), matches detuneRatio

    const float jit      = (posJtr > 0.0f) ? (posJtr * 4800.0f * randBipolar()) : 0.0f;
    const float startPos = basePos + jit;

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
            float pp = startPos + span * (float(k) / float(nPeek - 1));
            if (!liveMode && sampleCount > 0)         // wrap to match the grain's read
                pp -= float(sampleCount) * floorf(pp / float(sampleCount));
            const float sv = readSrc(pp, liveMode, sampleCount);
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
    g.envIncr    = envIncr;
    g.envTex     = mCurTexture;   // capture Texture for this grain's life (no mid-grain jump)
    g.reverse    = reverseProb;   // per-spawn reverse decision (computed at call site)
    g.live       = liveMode;
    g.pitchIncrL = incL;
    g.pitchIncrR = incL * detuneRatio;
    g.scale      = scale;
    g.held       = held;
    g.srcIndex   = int(startPos);

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

    g.readPosL   = startPos - delayL;  g.readPos0L = g.readPosL;
    g.readPosR   = startPos - delayR;  g.readPos0R = g.readPosR;

    g.startDelay = sampleOffset;
    g.pitchSemi  = semiShift + psprdSemi;
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

    // ── Parameters ─────────────────────────────────────────────────────────
    // "Rate" is now a DENSITY control = target grain overlap. 0 = trigger-only;
    // otherwise the spawn rate is derived from grain length so grains overlap (no
    // more silent/gappy low end). Floored at kDensFloor so it never near-stops.
    const float dens     = std::max(0.0f,  std::min(mRateIn.buffer()[0],   16.0f));
    const float playhead = std::max(0.0f,  std::min(mPlayheadIn.buffer()[0], 1.0f));
    const float posJtr   = std::max(0.0f,  std::min(mPosJtrIn.buffer()[0],  1.0f));
    const float sprd     = std::max(0.0f,  std::min(mSprdIn.buffer()[0],    1.0f));
    const float detune   = std::max(0.0f,  std::min(mDetuneIn.buffer()[0],  2.0f));
    const float level    = std::max(0.0f,  std::min(mLevelIn.buffer()[0],   1.0f));
    const float mix      = std::max(0.0f,  std::min(mMixIn.buffer()[0],     1.0f));
    const float feedback = std::max(0.0f,  std::min(mFeedbackIn.buffer()[0], 1.0f));
    const float compress = std::max(0.0f,  std::min(mCompressIn.buffer()[0], 1.0f));
    const float binaural = std::max(0.0f,  std::min(mBinauralIn.buffer()[0], 1.0f));
    const int   scale    = std::max(0, std::min(int(mScaleIn.buffer()[0] + 0.5f), 6));
    mVizSprd = sprd;  mVizDetune = detune;
    const float revprob  = std::max(0.0f,  std::min(mRevProbIn.buffer()[0], 1.0f));
    const float psprd    = std::max(0.0f,  std::min(mPsprdIn.buffer()[0],  12.0f));
    const float texture  = std::max(0.0f,  std::min(mTextureIn.buffer()[0], 1.0f));
    const int   grains   = std::max(1, std::min(int(mGrainsIn.buffer()[0] + 0.5f), kMaxGrains));
    const bool  holdOn   = (mHoldIn.buffer()[0] > 0.5f);
    // Transpose = SemiShift (coarse, integer semitones) + V/Oct (smooth, 1 V = 1 oct).
    // Clamped to ±4 octaves so extreme reads don't alias into garbage.
    const float semiKnob = roundf(std::max(-24.0f, std::min(mSemiShiftIn.buffer()[0], 24.0f)));
    const float voct     = mVoctIn.buffer()[0];
    const float semiShift= std::max(-kMaxTranspose,
                             std::min(semiKnob + voct * kVoctToSemis, kMaxTranspose));
    const float speed    = std::max(-4.0f, std::min(mSpeedIn.buffer()[0],  4.0f));
    const float grainLenSec = std::max(0.001f, std::min(mGrainLenIn.buffer()[0],
                                       float(kMaxGrainSamp) / float(kSampleRate)));
    const int   grainDuration = std::max(64, std::min(int(grainLenSec * float(kSampleRate)),
                                                      kMaxGrainSamp));
    const float *fireBuf = mFireIn.buffer();

    // ── Capture live input (+ feedback reinjection) into the ring ──────────
    // Feedback is a LIVE-mode feature (grains read the ring only in live mode), so
    // the whole feedback path is gated on liveMode — in sample mode the ring keeps
    // capturing plain input (warm for detach) but pays no tanh/mix cost.
    // Ring advance uses the power-of-two mask (kCapBufSize), not a compare-branch.
    {
        const float *in = mIn.buffer();
        if (liveMode && feedback > 0.0001f) {
            for (int s = 0; s < blockSize; ++s) {
                mCapBuf[mCapWrite] = sanitize(in[s] + feedback * mFeedBuf[s]);
                mCapWrite = (mCapWrite + 1) & (kCapBufSize - 1);
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
    if (freeRun) {
        mSpawnTimer -= float(blockSize);
        while (mSpawnTimer <= 0.0f) {
            int spawnAt = blockSize + int(mSpawnTimer);
            if (spawnAt < 0)          spawnAt = 0;
            if (spawnAt >= blockSize) spawnAt = blockSize - 1;
            spawnGrain(playheadEff, sampleCount, posJtr, sprd, detune, level,
                       (revprob > 0.0f) && (randUnipolar() < revprob), psprd, grains,
                       holdOn, semiShift, grainDuration, hopSamples, spawnAt, liveMode, compress, binaural, scale);
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
                           holdOn, semiShift, grainDuration, hopSamples, s, liveMode, compress, binaural, scale);
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
        float readL = g.readPosL, readR = g.readPosR, envPh = g.envPhase;
        const float envInc = g.envIncr;   // hoisted: g.* float loads can alias out-buffer stores
        const float lpAL = g.lpAlphaL, lpAR = g.lpAlphaR;   // head-shadow LP (1 = transparent)
        float lpL = g.lpStateL, lpR = g.lpStateR;
        const bool doShadow = (lpAL < 0.9999f) || (lpAR < 0.9999f);   // binaural head-shadow engaged?
        // This grain's envelope = blend of two static shape tables by its captured
        // Texture (perc→Hann below 0.5, Hann→Tukey above). Selected once per grain.
        // At an exact node (Texture 0 or the 0.5 default) ew == 0 → skip the second
        // table lerp entirely (bit-identical: ea + 0·(eb−ea) == ea).
        const float *eA, *eB; float ew;
        if (g.envTex < 0.5f) { eA = mPercTable.data(); eB = mHannTable.data();  ew = g.envTex * 2.0f; }
        else                 { eA = mHannTable.data(); eB = mTukeyTable.data(); ew = (g.envTex - 0.5f) * 2.0f; }
        const bool blend = (ew != 0.0f);

        // Edge fade (anti-click): fade the grain as its read nears a SEAM — the
        // write head in live mode (read/write collision guard) or the sample loop
        // boundary in sample mode (boundary crossfade). In a dense cloud the
        // overlapping grains cover the dip, so it reads as a crossfade. Skipped
        // for very short samples (no interior to fade toward).
        const float period = live ? float(kCapBufSize) : float(sampleCount);
        const float halfP  = 0.5f * period;
        const bool  doSeam = live ? true : (sampleCount > int(4.0f * kSeamGuard));
        // v0.1.17: the R read gets its OWN seam distance. With Detune the R read
        // runs at a different speed and drifts far from L (»kSeamGuard over a long
        // grain) — tracking only L let R cross a seam unfaded (R-channel click).
        // ITD's read offset (Binaural) is the same story at smaller scale.
        float relSeamL = 0.0f, relSeamR = 0.0f;
        if (doSeam) {
            const float basePL = live ? (readL - float(mCapWrite)) : readL;
            relSeamL = basePL - period * floorf(basePL / period);   // [0, period)
            if (st) {
                const float basePR = live ? (readR - float(mCapWrite)) : readR;
                relSeamR = basePR - period * floorf(basePR / period);
            }
        }

        // Sample mode: keep read positions wrapped to [0, sampleCount) so the
        // per-sample read uses a cheap compare/subtract instead of a software
        // modulo (no hardware integer divide on the A8). Live mode masks in readSrc.
        const float wN = float(sampleCount);
        if (!live) {
            readL -= wN * floorf(readL / wN);
            readR -= wN * floorf(readR / wN);
        }

        // Gate the per-sample edge fade: only run it when the read can actually come
        // within kSeamGuard of the seam THIS block. Most grains are far from the
        // seam → skip all the fade work (the common-case CPU saving).
        bool seamActive = false;
        if (doSeam) {
            const float distL = (relSeamL < halfP) ? relSeamL : (period - relSeamL);
            const float travL = ((incL >= 0.0f) ? incL : -incL) * float(toRender);
            seamActive = (distL <= kSeamGuard + travL + 1.0f);
            if (st && !seamActive) {
                const float distR = (relSeamR < halfP) ? relSeamR : (period - relSeamR);
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
                const float distL = (relSeamL < halfP) ? relSeamL : (period - relSeamL);
                if (distL < kSeamGuard) fadeL = distL * kInvSeamGuard;
                relSeamL += incL;
                if      (relSeamL >= period) relSeamL -= period;
                else if (relSeamL <  0.0f)   relSeamL += period;
                if (st) {                         // R fades on its own seam distance
                    const float distR = (relSeamR < halfP) ? relSeamR : (period - relSeamR);
                    if (distR < kSeamGuard) fadeR = distR * kInvSeamGuard;
                    relSeamR += incR;
                    if      (relSeamR >= period) relSeamR -= period;
                    else if (relSeamR <  0.0f)   relSeamR += period;
                } else {
                    fadeR = fadeL;
                }
            }
            const float eL = env * fadeL;
            const float eR = env * fadeR;
            const float sL = readHoisted(capData, smpData, smpNc, sampleCount, live, readL);
            const float sR = st ? readHoisted(capData, smpData, smpNc, sampleCount, live, readR) : sL;
            if (doShadow) {                       // binaural: one-pole LP on the far ear
                lpL += lpAL * (sL - lpL);
                lpR += lpAR * (sR - lpR);
                outL[outStart + s] += gL * eL * lpL;
                outR[outStart + s] += gR * eR * lpR;
            } else {                              // common case: no filter, no extra work
                outL[outStart + s] += gL * eL * sL;
                outR[outStart + s] += gR * eR * sR;
            }
            readL += incL; readR += incR; envPh += envInc;
            if (!live) {
                if      (readL >= wN) readL -= wN; else if (readL < 0.0f) readL += wN;
                if      (readR >= wN) readR -= wN; else if (readR < 0.0f) readR += wN;
            }
        }
        g.readPosL = readL; g.readPosR = readR; g.envPhase = envPh;
        g.lpStateL = lpL;   g.lpStateR = lpR;
        g.age += toRender;
        g.startDelay = 0;
        if (g.age >= g.duration) {
            if (g.held && holdOn) {
                g.age = 0; g.envPhase = 0.0f;
                g.readPosL = g.readPos0L; g.readPosR = g.readPos0R;
            } else {
                g.active = false;
            }
        }
    }

    // ── Feedback source: soft-clipped wet mono (tanh keeps regeneration stable) ──
    // `drive` is a makeup gain INSIDE the tanh: the round-trip loses ~8 dB (densComp +
    // mono-sum + envelope average), so without it the loop never sustains even at
    // feedback≈1. Driving the tanh recovers that so feedback approaches unity loop gain
    // near 1.0; tanh still bounds mFeedBuf to [-1,1] → saturates into a stable drone,
    // no runaway. Host-tuned: 0=silent, ~0.8=clear tail, ~1=stable drone.
    if (liveMode && feedback > 0.0001f) {
        const float drive = 2.5f;
        for (int s = 0; s < blockSize; ++s)
            mFeedBuf[s] = sanitize(tanhf(drive * 0.5f * (outL[s] + outR[s])));
        mFeedPrimed = true;
    } else if (mFeedPrimed) {
        // Leaving the feedback path: clear so re-entering doesn't inject one block
        // of stale audio from whenever feedback last ran.
        std::fill(mFeedBuf.begin(), mFeedBuf.end(), 0.0f);
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
