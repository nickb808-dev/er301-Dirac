/* Dirac.h — Classic (time-domain) Granular Synthesizer for the ER-301
 *
 * A "classic" granular sibling of Planck (which is spectral). Dirac reads short
 * grains directly from the audio source with linear interpolation, multiplies
 * each by a windowed envelope, and overlap-adds many of them — the Beads/Clouds
 * approach. No FFT: crisp transients, classic formant-shifting pitch, low CPU.
 *
 * SOURCE (dual, automatic — same as Planck):
 *   • a static od::Sample (mpSample) read at a movable Playhead, OR
 *   • the LIVE input captured into a rolling ring when no sample is attached.
 *
 * PITCH: playback speed. A grain advances its read position by `pitchIncr`
 *   (= 2^(semitones/12)) per output sample. Chord = several voices at once.
 *
 * FEEDBACK: output is reinjected into the capture ring (ring = input + fb·out),
 *   so grains re-granulate previous output → regenerating clouds / smears /
 *   drones. Headline in live mode (the classic Beads feedback texture).
 *
 * Named for the Dirac comb (Ш) — a train of impulses, which is what a grain
 * stream is. Sibling to Planck and Bohr. */

#pragma once

#ifndef SWIGLUA
#include <od/objects/heads/Head.h>
#include <od/objects/Object.h>
#include <od/audio/Sample.h>
#include <od/config.h>

#include <vector>
#include <cmath>
#include <cstdint>
#endif

namespace dirac {

class Dirac : public od::Head
{
public:
    Dirac();
    virtual ~Dirac();

#ifndef SWIGLUA

    void process() override;
    void setSample(od::Sample *sample) override;

    /* ── Ports ─────────────────────────────────────────────────────────── */
    od::Outlet mOutL      {"OutL"};
    od::Outlet mOutR      {"OutR"};
    od::Inlet  mIn        {"In"};        // LIVE chain audio — captured when no sample attached
    od::Inlet  mPlayheadIn{"Playhead"};  // sample: read position [0,1]; live: scan into capture ring
    od::Inlet  mRateIn    {"Rate"};      // DENSITY: target grain overlap (0 = trigger-only)
    od::Inlet  mPosJtrIn  {"PosJtr"};    // per-grain read-position spray [0, 1]
    od::Inlet  mSprdIn    {"Spread"};    // stereo pan scatter [0, 1]
    od::Inlet  mDetuneIn  {"Detune"};    // stereo pitch detune, R vs L [semitones]
    od::Inlet  mLevelIn   {"Level"};     // output gain [0, 1]
    od::Inlet  mRevProbIn {"RevProb"};   // reverse grain probability [0, 1]
    od::Inlet  mPsprdIn   {"Psprd"};     // per-grain pitch scatter [0, 12] semitones
    od::Inlet  mTextureIn {"Texture"};   // grain envelope shape [0=Hann, 1=Tukey]
    od::Inlet  mGrainsIn  {"Grains"};    // polyphony cap [1, kMaxGrains]
    od::Inlet  mFireIn    {"Fire"};      // gate: rising edge fires one spawn event
    od::Inlet  mHoldIn    {"Hold"};      // latched toggle: held grains loop as a frozen cloud
    od::Inlet  mSemiShiftIn {"SemiShift"};  // global pitch transpose [-24, +24] st
    od::Inlet  mGrainLenIn  {"GrainLen"};   // grain duration in SECONDS
    od::Inlet  mMixIn       {"Mix"};        // wet/dry: 1 = grains, 0 = dry input (LIVE mode only)
    od::Inlet  mFeedbackIn  {"Feedback"};   // output→capture reinjection [0, 1] (LIVE mode)
    od::Inlet  mCompressIn  {"Compress"};   // per-grain leveling toward a target [0, 1]
    od::Inlet  mBinauralIn  {"Binaural"};   // 3D depth: ITD + head-shadow amount [0, 1]
    od::Inlet  mScaleIn     {"Scale"};      // quantize Psprd scatter to a scale (0 = off)
    od::Inlet  mVoctIn      {"V/Oct"};      // 1 V/oct pitch CV, summed with SemiShift
    od::Inlet  mSpeedIn     {"Speed"};      // playhead scan rate [-4,4]; 0 = parked (v0.1.18)
    // ── Multi-playhead (v0.2.0) ────────────────────────────────────────────
    // Grains are dealt round-robin to N playheads reading the SAME buffer at
    // different positions and pitches.  Heads = 1 is bit-identical to v0.1.18.
    // Multi-playhead is OFF by default so the strip stays uncluttered; the option
    // hard-bypasses the feature (heads forced to 1) regardless of the inlets.
    //   1 = Off (default) · 2 = On
    od::Option mMultiHeadOpt {"Playheads", 1};

    od::Inlet  mHeadsIn     {"Heads"};      // playhead count [1, kMaxHeads]
    od::Inlet  mHeadSpreadIn{"HeadSpread"}; // position offset between heads [0,1]
    od::Inlet  mHeadTuneIn  {"HeadTune"};   // pitch interval between heads [-12,+12] st
    // ── Snap (v0.2.0) ──────────────────────────────────────────────
    od::Inlet  mSnapIn      {"Snap"};       // pull grains onto transients [0,1]
    // ── v0.2.2 ─────────────────────────────────────────────────────────────
    // Color: per-grain LOW-PASS CUTOFF SCATTER (the spectral analog of Psprd).
    // Each grain draws a random cutoff, darker with Color — dark and bright
    // grains interleave in one cloud. Reuses the binaural head-shadow one-pole
    // (min-combined per ear), so render cost is the already-gated LP path.
    od::Inlet  mColorIn     {"Color"};      // cutoff scatter depth [0,1]; 0 = off
    // Diffuse (menu option, 1 = Off · 2 = On): 4 cascaded Schroeder allpasses
    // per channel on the WET path (before the feedback tap, so regeneration is
    // diffused too — the Clouds-style halo). Hard-bypassed when Off.
    od::Option mDiffuseOpt  {"Diffuse", 1};
    // Freeze (menu option, 1 = Run · 2 = Freeze): stops WRITING the capture
    // ring. Distinct from Hold: Hold freezes the CLOUD, Freeze freezes the
    // SOURCE — every control stays live over the locked 2.7 s.
    od::Option mFreezeOpt   {"Freeze", 1};

    /* ── Grain-field / head-display accessors ──────────────────────────── */
    // kMaxGrains is public so the host harness (test/host/main.cpp) can size
    // arrays with it at compile time; everything else in this block is the
    // supported read-only surface the viz and the harness share.
    static constexpr int kMaxGrains = 16;
    int   getGrainCount()         const { return kMaxGrains; }
    bool  isGrainActive(int i)    const { return mGrains[i].active; }
    int   getGrainSrcIndex(int i) const { return mGrains[i].srcIndex; }
    float getGrainEnvelope(int i) const {
        const Grain &g = mGrains[i];
        if (g.duration <= 0) return 0.0f;
        float p = float(g.age) / float(g.duration);
        if (p < 0.0f) p = 0.0f; else if (p > 1.0f) p = 1.0f;
        return sinf(kPi * p);
    }
    float getGrainPan(int i)      const { return mGrains[i].panPos; }     // stereo position [-1,1]
    float getGrainPitch(int i)    const { return mGrains[i].pitchSemi; }  // transpose in semitones
    int   getGrainDuration(int i) const { return mGrains[i].duration; }   // samples (mark length)
    // Where this grain is reading in the SOURCE, normalised to [0,1] — the
    // alternate X axis for the grain field (v0.2.0). Live: 0 = oldest material in
    // the ring, 1 = the write head (now). Sample: 0 = start, 1 = end. This is the
    // live read position, not the spawn position, so grains visibly drift as they
    // play (and reverse grains drift backwards).
    float getGrainReadPos01(int i) const {
        const float p = mGrains[i].readPosL;
        if (mpSample && mEndIndex > 0) {                    // sample mode
            const float n = float(mEndIndex);
            float q = p - n * floorf(p / n);                 // wrap into [0,n)
            return q / n;
        }
        const float n = float(kCapBufSize);                 // live ring
        float age = float(mCapWrite) - p;                   // samples behind the write head
        age -= n * floorf(age / n);                         // wrap into [0,n)
        return 1.0f - age / n;                              // 1 = newest
    }
    // Spawn position on the same [0,1] axis as getGrainReadPos01 — the anchor
    // for the position-mode read-span trail (v0.2.5).
    float getGrainSpawnPos01(int i) const {
        const float p = mGrains[i].readPos0L;
        if (mpSample && mEndIndex > 0) {
            const float n = float(mEndIndex);
            float q = p - n * floorf(p / n);
            return q / n;
        }
        const float n = float(kCapBufSize);
        float age = float(mCapWrite) - p;
        age -= n * floorf(age / n);
        return 1.0f - age / n;
    }
    float getVizSpread()          const { return mVizSprd; }
    float getVizDetune()          const { return mVizDetune; }
    float getVizTexture()         const { return mCurTexture; }   // glyph shape (v0.2.0)
    // ── v0.2.5: every control reaches the field. All read state the DSP
    // already keeps per grain — display-rate only, zero audio-thread cost.
    bool  getGrainReverse(int i)  const { return mGrains[i].reverse; }  // glyph points down
    bool  getGrainLive(int i)     const { return mGrains[i].live; }     // + isSampleMode → regen grain
    bool  isSampleMode()          const { return mpSample != nullptr; }
    // Darker of the two filter alphas: Color scatter (and the binaural shadow)
    // → glyph brightness. 1 = fully open.
    float getGrainAlphaMin(int i) const {
        const Grain &g = mGrains[i];
        return (g.lpAlphaL < g.lpAlphaR) ? g.lpAlphaL : g.lpAlphaR;
    }
    // The grain's real gain (level · density comp · Compress makeup · pan) —
    // honest brightness: Compress evens the field, Level dims it.
    float getGrainGain(int i)     const {
        const Grain &g = mGrains[i];
        return g.scale * 0.5f * (g.gainL + g.gainR);
    }
    // NOT const: the device SDK's od::Option::value() is non-const (the host
    // stub's is const, which is why this compiled in the harness).
    bool  getDiffuseOn()                { return mDiffuseOpt.value() >= 2; }
    bool  getFreezeOn()                 { return mFreezeOpt.value()  >= 2; }  // snowflake
    int   getCapWritePos()        const { return mCapWrite; }  // baseline crawl; parks on Freeze
    // Harness-facing accessors (v0.2.0): spawn read position + current pan
    // gains, so the host tests don't need to reach into private grain state.
    float getGrainSpawnPos(int i) const { return mGrains[i].readPos0L; }
    float getGrainGainL(int i)    const { return mGrains[i].gainL; }
    float getGrainGainR(int i)    const { return mGrains[i].gainR; }

private:
    /* ── Constants ─────────────────────────────────────────────────────── */
    static constexpr int   kSampleRate    = 48000;
    static constexpr int   kMaxHeads      = 4;       // multi-playhead cap (v0.2.0)
    static constexpr int   kCapBufSize    = 131072;  // live capture ring ≈ 2.73 s (power of two)
    static constexpr int   kMaxGrainSamp  = 96000;   // grain length ceiling (2 s)
    static constexpr int   kEnvTableSize  = 1024;    // grain envelope table (read by phase)
    static constexpr float kDensFloor     = 0.25f;   // min density (overlap) so it never near-stops
    static constexpr float kSeamGuard     = 256.0f;  // edge-fade zone near a seam (samples, ~5.3 ms)
    static constexpr float kInvSeamGuard  = 1.0f / 256.0f;
    static constexpr float kPi            = 3.14159265358979f;
    static constexpr float kTwoPi         = 6.28318530717959f;
    static constexpr float kLog2Over12    = 0.05776226504f;   // ln(2)/12
    static constexpr float kMaxITDSamples = 31.2f;            // ~0.65 ms Woodworth max ITD @ ±90°
    static constexpr float kVoctToSemis   = 120.0f;           // FULLSCALE_IN_VOLTS(10)×12 st/oct; 1V=1oct
    static constexpr float kMaxTranspose  = 48.0f;            // ±4-octave transpose ceiling

    /* ── Grain pool ─────────────────────────────────────────────────────── */
    struct Grain {
        bool  active  = false;
        bool  held    = false;
        bool  reverse = false;
        bool  stereo  = false;   // render a separate R read (detune/spread engaged)
        int   duration = 0;
        int   age      = 0;
        int   startDelay = 0;
        int   srcIndex = 0;      // integer source position (head-display tick)
        float readPosL = 0.0f, readPosR = 0.0f;   // fractional read positions
        float readPos0L = 0.0f, readPos0R = 0.0f; // spawn positions (hold loop)
        float pitchIncrL = 1.0f, pitchIncrR = 1.0f;
        float envPhase = 0.0f, envIncr = 0.0f;    // envelope table phase
        float envTex   = 0.5f;                     // Texture captured at spawn (held for life)
        float gainL = 1.0f, gainR = 1.0f;         // equal-power pan (centre = unity)
        float lpStateL = 0.0f, lpStateR = 0.0f;   // binaural head-shadow 1-pole LP state
        float lpAlphaL = 1.0f, lpAlphaR = 1.0f;   // LP coeff per ear (1 = transparent)
        float scale = 1.0f;                       // level/density compensation
        bool  live  = false;     // read from capture ring (true) or sample (false)
        // viz
        float panPos = 0.0f;     // [-1,1] grain-field X
        float pitchSemi = 0.0f;  // grain-field Y
    };
    Grain mGrains[kMaxGrains];

    float mVizSprd = 0.0f, mVizDetune = 0.0f;

    /* ── Live capture ring + feedback ──────────────────────────────────── */
    std::vector<float> mCapBuf;        // kCapBufSize
    int                mCapWrite = 0;
    std::vector<float> mFeedBuf;        // last block's output mono (feedback source)
    bool               mFeedPrimed = false;  // mFeedBuf holds live data (cleared on leaving fb)

    /* ── Envelope table (morphed Hann→Tukey, rebuilt on Texture change) ─── */
    // Three STATIC shape tables built once (ctor), never rebuilt. Each grain blends
    // two of them by the Texture it captured at spawn, so modulating Texture affects
    // only NEW grains — no mid-grain envelope jump (= no clicks) and no rebuild cost.
    std::vector<float> mPercTable;      // kEnvTableSize + 1
    std::vector<float> mHannTable;
    std::vector<float> mTukeyTable;
    float mCurTexture = 0.5f;           // Texture at the current block (captured per spawn)
    void  buildEnvTables();

    /* ── Spawn timing ───────────────────────────────────────────────────── */
    float mSpawnTimer = 0.0f;
    float mLastFire   = 0.0f;

    /* ── Playhead scan (v0.1.18: pitch/time decoupling, sample mode) ────── */
    // Speed != 0 advances the spawn position through the sample independently
    // of grain pitch (granular time-stretch). double: block-rate only, and
    // float lacks sample resolution on long samples.
    double mScanPos          = 0.0;    // normalized [0,1)
    float  mLastPlayheadKnob = -1.0f;  // reseat detector (knob touch re-seats scan)
    int    mHeadCycle        = 0;      // round-robin dealer: next head to spawn on
    // Snap search budget (v0.2.1): at most kSnapPerBlock coarse→fine onset
    // searches per block; grains past it spawn unsnapped. Bounds the worst-case
    // spawn-rate cost (~16×) without caching results across grains — per-grain
    // searches are what make posJtr+Snap land on DIFFERENT hits.
    static constexpr int kSnapPerBlock = 2;
    int    mSnapBudget       = 0;

    /* ── Diffuser (v0.2.2): 4 cascaded Schroeder allpasses per channel ───── */
    // Delay lengths are mutually-prime, slightly different L vs R for stereo
    // decorrelation (~49 ms total path). Fixed g — one knob of character, zero
    // knobs of panel space. Buffers cleared when the option turns Off so
    // re-enabling never replays a stale tail.
    static constexpr int   kApStages = 4;
    static constexpr int   kApSizeL[kApStages] = { 113, 337, 719, 1201 };
    static constexpr int   kApSizeR[kApStages] = { 127, 347, 743, 1213 };
    static constexpr float kApGain   = 0.6f;
    std::vector<float> mApBufL[kApStages], mApBufR[kApStages];
    int  mApIdxL[kApStages] = {0}, mApIdxR[kApStages] = {0};
    bool mDiffusePrimed = false;   // buffers hold energy → clear once on Off

    /* ── Feedback DC-blocker (v0.2.8) ──────────────────────────────────────── */
    // One-pole high-pass (~20 Hz) on the mono sum BEFORE the tanh. Without it
    // the loop integrates DC/sub-audio (granular overlap-AM has strong LF
    // content); the pedestal rails the tanh, whose small-signal slope then
    // squeezes the actual audio to nothing — "fades out like an overload".
    // Multi-head made it faster (LF sums coherently across all taps; audio
    // combs), which is how it was found.
    float mFbHpX = 0.0f, mFbHpY = 0.0f;

    /* ── PRNG ───────────────────────────────────────────────────────────── */
    uint32_t mRandState = 0x1234abcdu;
    float randUnipolar();
    float randBipolar();

    /* ── Helpers ────────────────────────────────────────────────────────── */
    // readHoisted: interpolated source read with all object state passed as args, so
    // the render loop hoists the mpSample->mpData / mChannelCount / mCapBuf.data()
    // loads OUT of the hot loop (same-typed float stores to the out buffers may alias
    // member loads, so the compiler cannot hoist them itself).
    static inline float readHoisted(const float *cap, const float *sd, int nc,
                                    int sampleCount, bool live, float pos);
    inline float readSrc(float pos, bool live, int sampleCount) const;
    void  spawnGrain(float playhead, int sampleCount, float posJtr, float sprd,
                     float detune, float level, bool reverseProb,
                     float psprd, int grainsCap, bool held,
                     float semiShift, int grainDuration, float hopSamples,
                     int sampleOffset, bool liveMode, float compress, float binaural,
                     int scaleSel, int heads, float headSpread, float headTune,
                     float snap,
                     float feedback, float color);
    int   findSlot(int grainsCap);

#endif // SWIGLUA
};

} // namespace dirac
