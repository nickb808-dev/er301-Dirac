// Host verification harness for Dirac.
// Modes:
//   ident  <out.raw>   deterministic renders (sample + live-feedback + texture nodes) → raw floats
//   asan               long-grain (2 s) + hold + extreme pitch, run to completion (OOB probe)
//   seam   <label>     detuned long grains parked at the sample loop boundary → R-channel max delta
#include "Dirac.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

using dirac::Dirac;

static uint32_t rng = 0xdeadbeef;
static float frand()
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return float(rng & 0x7FFFFFFFu) / float(0x7FFFFFFFu) * 2.0f - 1.0f;
}

struct Ctl { const char *port; float v; };

static void setInputs(Dirac &d, const Ctl *ctls, int n)
{
    od::Inlet *ins[] = { &d.mIn, &d.mPlayheadIn, &d.mRateIn, &d.mPosJtrIn, &d.mSprdIn,
                         &d.mDetuneIn, &d.mLevelIn, &d.mRevProbIn, &d.mPsprdIn, &d.mTextureIn,
                         &d.mGrainsIn, &d.mFireIn, &d.mHoldIn, &d.mSemiShiftIn, &d.mGrainLenIn,
                         &d.mMixIn, &d.mFeedbackIn, &d.mCompressIn, &d.mBinauralIn, &d.mScaleIn,
                         &d.mVoctIn };
    for (int c = 0; c < n; ++c)
        for (auto *p : ins)
            if (!strcmp(p->mName, ctls[c].port))
                for (int s = 0; s < FRAMELENGTH; ++s) p->buffer()[s] = ctls[c].v;
}

static od::Sample makeSample(std::vector<float> &store, int count, int nc)
{
    store.resize(size_t(count) * nc);
    for (int i = 0; i < count; ++i)
        for (int c = 0; c < nc; ++c)
            store[size_t(i) * nc + c] = 0.7f * sinf(2.0f * 3.14159265f * 220.0f * i / 48000.0f)
                                      + 0.1f * frand();
    od::Sample s; s.mSampleCount = size_t(count); s.mChannelCount = nc; s.mpData = store.data();
    return s;
}

static void run(Dirac &d, int blocks, FILE *fout, float *maxRDelta = nullptr, int *nonFinite = nullptr)
{
    float prevR = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        // deterministic live input
        for (int s = 0; s < FRAMELENGTH; ++s)
            d.mIn.buffer()[s] = 0.5f * sinf(2.0f * 3.14159265f * 110.0f * (b * FRAMELENGTH + s) / 48000.0f);
        d.process();
        float *L = d.mOutL.buffer(), *R = d.mOutR.buffer();
        if (fout) { fwrite(L, 4, FRAMELENGTH, fout); fwrite(R, 4, FRAMELENGTH, fout); }
        for (int s = 0; s < FRAMELENGTH; ++s) {
            if (nonFinite && (!std::isfinite(L[s]) || !std::isfinite(R[s]))) (*nonFinite)++;
            if (maxRDelta) {
                float delta = fabsf(R[s] - prevR);
                if (delta > *maxRDelta) *maxRDelta = delta;
                prevR = R[s];
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "mode?\n"); return 2; }

    if (!strcmp(argv[1], "ident")) {
        FILE *f = fopen(argv[2], "wb");
        std::vector<float> st;
        // A: sample mode, busy controls, feedback knob up (inert in sample mode pre/post — but
        //    pre-change it ran the tanh path; outputs must still match since ring is unread)
        {
            Dirac d; rng = 0xdeadbeef;
            od::Sample smp = makeSample(st, 48000, 2);
            d.setSample(&smp);
            const Ctl c[] = {{"Playhead",0.4f},{"Rate",4.0f},{"PosJtr",0.3f},{"Spread",0.6f},
                             {"Detune",0.0f},{"Level",0.8f},{"RevProb",0.3f},{"Psprd",5.0f},
                             {"Texture",0.37f},{"Grains",12.0f},{"SemiShift",3.0f},
                             {"GrainLen",0.08f},{"Mix",1.0f},{"Feedback",0.0f},
                             {"Compress",0.5f},{"Binaural",0.0f},{"Scale",2.0f},{"V/Oct",0.05f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            run(d, 800, f);
            d.setSample(nullptr);
        }
        // B: live mode, feedback active
        {
            Dirac d; rng = 0x12345678;
            const Ctl c[] = {{"Playhead",0.1f},{"Rate",3.0f},{"Spread",0.5f},{"Level",0.7f},
                             {"Texture",0.8f},{"Grains",10.0f},{"GrainLen",0.05f},
                             {"Mix",0.7f},{"Feedback",0.8f},{"Binaural",0.4f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            run(d, 800, f);
        }
        // C: sample mode, texture exactly at nodes 0.0 / 0.5 / 1.0
        for (float tex : {0.0f, 0.5f, 1.0f}) {
            Dirac d; rng = 0xcafef00d;
            od::Sample smp = makeSample(st, 30000, 1);
            d.setSample(&smp);
            const Ctl c[] = {{"Playhead",0.5f},{"Rate",6.0f},{"Level",0.8f},{"Texture",tex},
                             {"Grains",16.0f},{"GrainLen",0.03f},{"Feedback",0.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            run(d, 400, f);
            d.setSample(nullptr);
        }
        fclose(f);
        printf("ident done\n");
        return 0;
    }

    if (!strcmp(argv[1], "asan")) {
        std::vector<float> st;
        int nf = 0;
        // Long grains at ceiling (2 s), hold-looped, extreme transpose, tiny + big samples.
        for (int count : {5, 300, 480000}) {
            Dirac d; rng = 0xabc123;
            od::Sample smp = makeSample(st, count, 1);
            d.setSample(&smp);
            const Ctl c[] = {{"Playhead",0.99f},{"Rate",8.0f},{"PosJtr",0.8f},{"Level",0.9f},
                             {"Psprd",12.0f},{"Texture",0.2f},{"Grains",16.0f},
                             {"SemiShift",24.0f},{"V/Oct",0.2f},   // +24 st clamp probe
                             {"GrainLen",2.0f},{"Hold",1.0f},{"Compress",1.0f},
                             {"Binaural",1.0f},{"Detune",2.0f},{"Spread",1.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            run(d, 3000, nullptr, nullptr, &nf);   // 3000 blocks = 8 s → grains complete + re-loop
            d.setSample(nullptr);
            run(d, 200, nullptr, nullptr, &nf);    // live-mode tail after detach
        }
        // Live mode long-grain + feedback
        {
            Dirac d; rng = 0x777;
            const Ctl c[] = {{"Rate",8.0f},{"Level",0.9f},{"Grains",16.0f},{"GrainLen",2.0f},
                             {"Feedback",0.95f},{"SemiShift",-24.0f},{"Detune",2.0f},
                             {"Binaural",1.0f},{"Spread",1.0f},{"Hold",1.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            run(d, 3000, nullptr, nullptr, &nf);
        }
        printf("asan done, nonFinite=%d\n", nf);
        return nf ? 1 : 0;
    }

    if (!strcmp(argv[1], "seam")) {
        // Detuned long grains reading across the sample loop boundary.
        std::vector<float> st;
        Dirac d; rng = 0xbeef;
        // Non-seamless boundary: DC-offset sine so the wrap is a hard step.
        od::Sample smp = makeSample(st, 20000, 1);
        for (auto &v : st) v = v * 0.5f + 0.4f;
        d.setSample(&smp);
        const float det = (argc > 3) ? float(atof(argv[3])) : 2.0f;
        const Ctl c[] = {{"Playhead",0.999f},{"Rate",2.0f},{"Level",0.9f},{"Grains",8.0f},
                         {"GrainLen",1.0f},{"Detune",det},{"Spread",0.0f},{"Texture",1.0f}};
        setInputs(d, c, sizeof(c)/sizeof(c[0]));
        float maxRD = 0.0f; int nf = 0;
        run(d, 2000, nullptr, &maxRD, &nf);
        printf("%s: R-channel max sample delta = %.5f (nonFinite=%d)\n", argv[2], maxRD, nf);
        return 0;
    }

    fprintf(stderr, "unknown mode\n");
    return 2;
}
