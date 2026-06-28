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

    mCapBuf.assign(kCapBufSize, 0.0f);
    mFeedBuf.assign(FRAMELENGTH, 0.0f);
    mEnvTable.assign(kEnvTableSize + 1, 0.0f);
    buildEnvTable(0.0f);
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

/* ── envelope table ────────────────────────────────────────────────────────── */
// Texture morphs THREE shapes:  Percussive (0) → Hann (0.5) → Tukey (1).
//   • Percussive: fast attack + exponential decay → crisp transient "bits" for
//     sparse short-grain clouds. Still zero-ended (click-free) via a release taper.
//   • Hann (0.5, the default): smooth rounded grain — the classic neutral shape.
//   • Tukey (1): flat top → more sustained / fuller grains.
// Read by phase in [0, kEnvTableSize]; index kEnvTableSize is a 0 guard point.
void Dirac::buildEnvTable(float texture)
{
    const float taper = 0.125f;   // Tukey cosine-taper fraction at each end
    const float atk   = 0.02f;    // percussive attack fraction
    const float rel   = 0.04f;    // percussive release-to-zero taper (click-free end)
    // Exponential decay built INCREMENTALLY: one expf for the per-step ratio, then a
    // single multiply per entry. Texture is rebuilt EVERY block while the encoder
    // moves, so an expf PER entry (1025×/block) spiked the CPU — same trap that got
    // Fog removed in v0.1.6. exp(-4·(t−atk)/(1−atk)) sampled at uniform steps is a
    // geometric series, so the ratio below reproduces it exactly.
    const float decayRatio = expf(-4.0f / ((1.0f - atk) * float(kEnvTableSize)));
    const float invRel     = 1.0f / rel;
    float decayVal = 1.0f;
    bool  inDecay  = false;
    for (int i = 0; i <= kEnvTableSize; ++i) {
        const float t = float(i) / float(kEnvTableSize);

        const float hann = 0.5f * (1.0f - cosf(kTwoPi * t));

        float tukey;
        if      (t < taper)        tukey = 0.5f * (1.0f - cosf(kPi * t / taper));
        else if (t > 1.0f - taper) tukey = 0.5f * (1.0f - cosf(kPi * (1.0f - t) / taper));
        else                       tukey = 1.0f;

        // Percussive: linear fast attack to 1, then incremental exponential decay;
        // a short cosine release at the very end forces a clean zero (no click).
        float perc;
        if (t < atk) {
            perc = t / atk;
        } else {
            if (!inDecay) { inDecay = true; decayVal = 1.0f; }
            else          { decayVal *= decayRatio; }
            perc = decayVal;
        }
        if (t > 1.0f - rel) perc *= 0.5f * (1.0f - cosf(kPi * (1.0f - t) * invRel));

        float env;
        if (texture < 0.5f) {
            const float w = texture * 2.0f;          // 0→1 over Texture 0..0.5
            env = perc + w * (hann - perc);          // Percussive → Hann
        } else {
            const float w = (texture - 0.5f) * 2.0f; // 0→1 over Texture 0.5..1
            env = hann + w * (tukey - hann);         // Hann → Tukey
        }
        mEnvTable[i] = env;
    }
    mEnvTable[0]             = 0.0f;   // guarantee click-free onset
    mEnvTable[kEnvTableSize] = 0.0f;   // guarantee click-free tail
}

/* ── source read (linear interpolation) ───────────────────────────────────── */
inline float Dirac::readSrc(float pos, bool live, int sampleCount) const
{
    const float fp = floorf(pos);
    const float f  = pos - fp;
    if (live) {
        int i0 = int(fp) & (kCapBufSize - 1);
        int i1 = (i0 + 1) & (kCapBufSize - 1);
        return mCapBuf[i0] + f * (mCapBuf[i1] - mCapBuf[i0]);
    }
    if (!mpSample || sampleCount <= 0) return 0.0f;
    // pos is kept in [0, sampleCount) by the render loop, so NO per-sample modulo
    // (the A8 has no integer divide). i0 is already in range; i1 wraps at the end.
    int i0 = int(fp);
    // Defensive clamp: the render loop's single compare-subtract wrap assumes the
    // pitch increment is smaller than the sample. A tiny sample + extreme pitch
    // (high SemiShift/Psprd) can leave i0 ≥ sampleCount → an out-of-bounds read.
    // Clamp here (cheap compares, no divide) so a degenerate input stays safe.
    if      (i0 >= sampleCount) i0 = sampleCount - 1;
    else if (i0 < 0)            i0 = 0;
    int i1 = (i0 + 1 >= sampleCount) ? 0 : i0 + 1;
    const int nc = int(mpSample->mChannelCount);
    const float *d = mpSample->mpData;
    if (nc <= 1) return d[i0] + f * (d[i1] - d[i0]);
    const float *p0 = d + size_t(i0) * nc;
    const float *p1 = d + size_t(i1) * nc;
    const float s0 = 0.5f * (p0[0] + p0[1]);
    const float s1 = 0.5f * (p1[0] + p1[1]);
    return s0 + f * (s1 - s0);
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
                       int sampleOffset, bool liveMode, float compress, float binaural)
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

    const float psprdSemi = (psprd > 0.0f) ? (randBipolar() * psprd) : 0.0f;
    const float semis     = semiShift + psprdSemi;
    const float incL      = powf(2.0f, semis / 12.0f);

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
    mVizSprd = sprd;  mVizDetune = detune;
    const float revprob  = std::max(0.0f,  std::min(mRevProbIn.buffer()[0], 1.0f));
    const float psprd    = std::max(0.0f,  std::min(mPsprdIn.buffer()[0],  12.0f));
    const float texture  = std::max(0.0f,  std::min(mTextureIn.buffer()[0], 1.0f));
    const int   grains   = std::max(1, std::min(int(mGrainsIn.buffer()[0] + 0.5f), kMaxGrains));
    const bool  holdOn   = (mHoldIn.buffer()[0] > 0.5f);
    const float semiShift= roundf(std::max(-24.0f, std::min(mSemiShiftIn.buffer()[0], 24.0f)));
    const float grainLenSec = std::max(0.001f, std::min(mGrainLenIn.buffer()[0],
                                       float(kMaxGrainSamp) / float(kSampleRate)));
    const int   grainDuration = std::max(64, std::min(int(grainLenSec * float(kSampleRate)),
                                                      kMaxGrainSamp));
    const float *fireBuf = mFireIn.buffer();

    // ── Capture live input (+ feedback reinjection) into the ring ──────────
    {
        const float *in = mIn.buffer();
        if (feedback > 0.0001f) {
            for (int s = 0; s < blockSize; ++s) {
                mCapBuf[mCapWrite] = sanitize(in[s] + feedback * mFeedBuf[s]);
                if (++mCapWrite >= kCapBufSize) mCapWrite = 0;
            }
        } else {
            for (int s = 0; s < blockSize; ++s) {
                mCapBuf[mCapWrite] = sanitize(in[s]);
                if (++mCapWrite >= kCapBufSize) mCapWrite = 0;
            }
        }
    }

    // ── Envelope table: rebuild only when Texture changes by ≥1% ───────────
    // The rebuild loops 1025 entries (cosf each); rebuilding every block while the
    // Texture encoder sweeps is the CPU the user hit. Quantizing to a 1% grid caps
    // it at ~101 rebuilds across the whole range — inaudible stepping, cheap.
    const float texQ = roundf(texture * 100.0f) * 0.01f;
    if (texQ != mEnvCacheTex) {
        buildEnvTable(texQ);
        mEnvCacheTex = texQ;
    }

    if (!liveMode) mCurrentIndex = int(playhead * float(sampleCount > 1 ? sampleCount - 1 : 0));
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
            spawnGrain(playhead, sampleCount, posJtr, sprd, detune, level,
                       (revprob > 0.0f) && (randUnipolar() < revprob), psprd, grains,
                       holdOn, semiShift, grainDuration, hopSamples, spawnAt, liveMode, compress, binaural);
            mSpawnTimer += hopSamples;
        }
    } else {
        mSpawnTimer = 0.0f;
    }
    {
        float last = mLastFire;
        for (int s = 0; s < blockSize; ++s) {
            if (fireBuf[s] > 0.5f && last <= 0.5f) {
                spawnGrain(playhead, sampleCount, posJtr, sprd, detune, level,
                           (revprob > 0.0f) && (randUnipolar() < revprob), psprd, grains,
                           holdOn, semiShift, grainDuration, hopSamples, s, liveMode, compress, binaural);
            }
            last = fireBuf[s];
        }
        mLastFire = last;
    }

    // ── Render active grains (time-domain overlap-add) ─────────────────────
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
        const float lpAL = g.lpAlphaL, lpAR = g.lpAlphaR;   // head-shadow LP (1 = transparent)
        float lpL = g.lpStateL, lpR = g.lpStateR;

        // Edge fade (anti-click): fade the grain as its read nears a SEAM — the
        // write head in live mode (read/write collision guard) or the sample loop
        // boundary in sample mode (boundary crossfade). In a dense cloud the
        // overlapping grains cover the dip, so it reads as a crossfade. Skipped
        // for very short samples (no interior to fade toward).
        const float period = live ? float(kCapBufSize) : float(sampleCount);
        const float halfP  = 0.5f * period;
        const bool  doSeam = live ? true : (sampleCount > int(4.0f * kSeamGuard));
        float relSeam = 0.0f;
        if (doSeam) {
            const float baseP = live ? (readL - float(mCapWrite)) : readL;
            relSeam = baseP - period * floorf(baseP / period);   // [0, period)
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
            const float distNow   = (relSeam < halfP) ? relSeam : (period - relSeam);
            const float maxTravel = ((incL >= 0.0f) ? incL : -incL) * float(toRender);
            seamActive = (distNow <= kSeamGuard + maxTravel + 1.0f);
        }

        for (int s = 0; s < toRender; ++s) {
            const int   ei = int(envPh);
            const float fr = envPh - float(ei);
            const float env = mEnvTable[ei] + fr * (mEnvTable[ei + 1] - mEnvTable[ei]);
            float fade = 1.0f;
            if (seamActive) {
                const float dist = (relSeam < halfP) ? relSeam : (period - relSeam);
                if (dist < kSeamGuard) fade = dist * kInvSeamGuard;
                relSeam += incL;
                if      (relSeam >= period) relSeam -= period;
                else if (relSeam <  0.0f)   relSeam += period;
            }
            const float e  = env * fade;
            const float sL = readSrc(readL, live, sampleCount);
            const float sR = st ? readSrc(readR, live, sampleCount) : sL;
            // Head-shadow one-pole LP per ear (transparent when alpha == 1).
            lpL += lpAL * (sL - lpL);
            lpR += lpAR * (sR - lpR);
            outL[outStart + s] += gL * e * lpL;
            outR[outStart + s] += gR * e * lpR;
            readL += incL; readR += incR; envPh += g.envIncr;
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
    if (feedback > 0.0001f) {
        const float drive = 2.5f;
        for (int s = 0; s < blockSize; ++s)
            mFeedBuf[s] = sanitize(tanhf(drive * 0.5f * (outL[s] + outR[s])));
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
