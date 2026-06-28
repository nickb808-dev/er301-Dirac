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

    /* ── Grain-field / head-display accessors ──────────────────────────── */
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
    float getVizSpread()          const { return mVizSprd; }
    float getVizDetune()          const { return mVizDetune; }

private:
    /* ── Constants ─────────────────────────────────────────────────────── */
    static constexpr int   kSampleRate    = 48000;
    static constexpr int   kMaxGrains     = 16;
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

    /* ── Envelope table (morphed Hann→Tukey, rebuilt on Texture change) ─── */
    std::vector<float> mEnvTable;       // kEnvTableSize + 1 (guard point)
    float mEnvCacheTex = -1.0f;
    void  buildEnvTable(float texture);

    /* ── Spawn timing ───────────────────────────────────────────────────── */
    float mSpawnTimer = 0.0f;
    float mLastFire   = 0.0f;

    /* ── PRNG ───────────────────────────────────────────────────────────── */
    uint32_t mRandState = 0x1234abcdu;
    float randUnipolar();
    float randBipolar();

    /* ── Helpers ────────────────────────────────────────────────────────── */
    inline float readSrc(float pos, bool live, int sampleCount) const;
    void  spawnGrain(float playhead, int sampleCount, float posJtr, float sprd,
                     float detune, float level, bool reverseProb,
                     float psprd, int grainsCap, bool held,
                     float semiShift, int grainDuration, float hopSamples,
                     int sampleOffset, bool liveMode, float compress, float binaural);
    int   findSlot(int grainsCap);

#endif // SWIGLUA
};

} // namespace dirac
