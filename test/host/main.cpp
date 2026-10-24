// Host verification harness for Dirac.
// Modes:
//   ident  <out.raw>   deterministic renders (sample + live-feedback + texture nodes) → raw floats
//   asan               long-grain (2 s) + hold + extreme pitch, run to completion (OOB probe)
//   seam   <label>     detuned long grains parked at the sample loop boundary → R-channel max delta
//   nan                every input poisoned (NaN / ±inf), live + sample: finite, recovers
//   pitch              grain pitch exact anywhere in a 60 s sample, and live
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
                         &d.mVoctIn,
                         &d.mHeadsIn, &d.mHeadSpreadIn, &d.mHeadTuneIn,
                         &d.mSnapIn,
                         &d.mTiltIn, &d.mTiltSprdIn, &d.mWindowIn,
#ifdef HAS_SPEED
                         &d.mSpeedIn,
#endif
                       };
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
        if (argc < 3) { fprintf(stderr, "ident: output path?\n"); return 2; }
        FILE *f = fopen(argv[2], "wb");
        if (!f) { fprintf(stderr, "ident: cannot open %s\n", argv[2]); return 2; }
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
                             {"Binaural",1.0f},{"Detune",2.0f},{"Spread",1.0f},
                             {"Speed",4.0f}};
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
        // v0.2.2 torture: Diffuse + sample-mode feedback + Snap +
        // Hold all at once, tiny sample, freeze toggled mid-flight,
        // detach with the diffuser running.
        {
            Dirac d; rng = 0x222;
            od::Sample smp = makeSample(st, 20000, 1);
            d.setSample(&smp);
            d.mDiffuseOpt.set(2);
            const Ctl c[] = {{"Playhead",0.9f},{"Rate",16.0f},{"Level",0.9f},{"Grains",16.0f},
                             {"GrainLen",2.0f},{"Feedback",0.95f},{"Window",2.0f},{"Tilt",-0.9f},{"TiltSpread",1.0f},
                             {"SemiShift",24.0f},{"Detune",2.0f},{"Binaural",1.0f},{"Spread",1.0f},
                             {"Hold",1.0f},{"Snap",1.0f},{"PosJtr",1.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            run(d, 1500, nullptr, nullptr, &nf);
            d.mFreezeOpt.set(2);                       // freeze mid-flight
            run(d, 750, nullptr, nullptr, &nf);
            d.mFreezeOpt.set(1);
            d.setSample(nullptr);                      // detach with diffuser running
            run(d, 750, nullptr, nullptr, &nf);
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

    if (!strcmp(argv[1], "fieldpos")) {
        const int N = 48000, kPeriod = 4800;
        std::vector<float> st(N, 0.0f);
        for (int o = 0; o < N; o += kPeriod)
            for (int i = 0; i < 200 && o + i < N; ++i)
                st[o + i] = 0.9f * expf(-float(i)/40.0f) * ((i & 1) ? -1.0f : 1.0f);
        od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
        int bad = 0;
        for (int pass = 0; pass < 2; ++pass) {
            Dirac d; d.setSample(&smp);
            Ctl c[] = {{"Playhead",0.25f},{"Rate",8.0f},{"GrainLen",0.02f},{"Grains",16.0f},
                       {"Level",0.7f},{"PosJtr",1.0f},{"Texture",0.5f},
                       {"Snap",pass?1.0f:0.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            // Histogram the position axis into the 12 columns a 12px-wide panel has.
            int hist[42] = {0}; int n = 0;
            for (int b = 0; b < 400; ++b) {
                d.process();
                for (int i = 0; i < dirac::Dirac::kMaxGrains; ++i) {
                    if (!d.isGrainActive(i)) continue;
                    float p = d.getGrainReadPos01(i);
                    if (!(p == p) || p < 0.0f || p > 1.0f) { bad++; continue; }
                    hist[int(p * 41.999f)]++; n++;
                }
            }
            // Occupancy concentration: fraction of hits in the busiest 3 columns.
            int best3 = 0;
            for (int k = 0; k + 2 < 42; ++k) {
                const int sum3 = hist[k] + hist[k+1] + hist[k+2];
                if (sum3 > best3) best3 = sum3;
            }
            printf("snap=%d n=%5d top3=%.2f [", pass, n,
                   n ? float(best3) / float(n) : 0.0f);
            for (int k = 0; k < 42; ++k) printf("%c", hist[k] == 0 ? '.' :
                   (hist[k] > n/8 ? '#' : (hist[k] > n/24 ? '+' : '-')));
            printf("]\n");
        }
        // The showcase: heads spread across the sample should read as COLUMNS.
        for (float hs : {0.0f, 0.3f, 0.9f}) {
            Dirac d; d.setSample(&smp); d.mMultiHeadOpt.set(2);
            Ctl c[] = {{"Playhead",0.05f},{"Rate",8.0f},{"GrainLen",0.02f},{"Grains",16.0f},
                       {"Level",0.7f},{"PosJtr",0.0f},{"Texture",0.5f},
                       {"Heads",4.0f},{"HeadSpread",hs},{"HeadTune",1.0f},{"Snap",0.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            int hist[42] = {0}; int n = 0;
            for (int b = 0; b < 400; ++b) {
                d.process();
                for (int i = 0; i < dirac::Dirac::kMaxGrains; ++i) {
                    if (!d.isGrainActive(i)) continue;
                    float p = d.getGrainReadPos01(i);
                    if (!(p == p) || p < 0.0f || p > 1.0f) { bad++; continue; }
                    hist[int(p * 41.999f)]++; n++;
                }
            }
            printf("hsprd=%.1f    [", hs);
            for (int k = 0; k < 42; ++k) printf("%c", hist[k] == 0 ? '.' :
                   (hist[k] > n/16 ? '#' : (hist[k] > n/48 ? '+' : '-')));
            printf("]\n");
        }
        printf("fieldpos: %s (out-of-range=%d)\n", bad ? "FAIL" : "PASS", bad);
        return bad ? 1 : 0;
    }

    if (!strcmp(argv[1], "headsnap")) {
        const int N = 48000, kPeriod = 4800;
        std::vector<float> st(N, 0.0f);
        for (int o = 0; o < N; o += kPeriod)
            for (int i = 0; i < 200 && o + i < N; ++i)
                st[o + i] = 0.9f * expf(-float(i) / 40.0f) * ((i & 1) ? -1.0f : 1.0f);
        od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();

        const float spreads[] = {0.02f, 0.10f, 0.50f};
        for (float hs : spreads) {
            for (int pass = 0; pass < 2; ++pass) {
                Dirac d; d.setSample(&smp);
                d.mMultiHeadOpt.set(2);
                Ctl c[] = {{"Playhead",0.1f},{"Rate",8.0f},{"GrainLen",0.02f},{"Grains",16.0f},
                           {"Level",0.7f},{"PosJtr",0.0f},{"Texture",0.5f},
                           {"Heads",4.0f},{"HeadSpread",hs},{"HeadTune",1.0f},
                           {"Snap",pass?1.0f:0.0f}};
                setInputs(d, c, sizeof(c)/sizeof(c[0]));
                double sum[4]={0,0,0,0}; int cnt[4]={0,0,0,0};
                int onsetHit[4]={-1,-1,-1,-1};
                bool seen[dirac::Dirac::kMaxGrains]={false};
                for (int b = 0; b < 300; ++b) {
                    d.process();
                    for (int i = 0; i < dirac::Dirac::kMaxGrains; ++i) {
                        if (d.isGrainActive(i) && !seen[i]) {
                            seen[i]=true;
                            int h = int(d.getGrainPitch(i) + 0.5f);   // headTune=1
                            if (h < 0 || h > 3) continue;
                            float p = d.getGrainSpawnPos(i);
                            p -= float(N)*floorf(p/float(N));
                            sum[h]+=p; cnt[h]++;
                            onsetHit[h] = int(p/float(kPeriod)+0.5f) % (N/kPeriod);
                        } else if (!d.isGrainActive(i)) seen[i]=false;
                    }
                }
                printf("hsprd=%.2f snap=%d  ", hs, pass);
                int distinct=0;
                for (int h=0;h<4;++h) {
                    printf("h%d=%7.0f ", h, cnt[h]?float(sum[h]/cnt[h]):-1.0f);
                    bool dup=false;
                    for (int k=0;k<h;++k) if (onsetHit[k]==onsetHit[h]) dup=true;
                    if (!dup && onsetHit[h]>=0) distinct++;
                }
                printf(" distinct onsets=%d\n", distinct);
            }
        }
        return 0;
    }

    if (!strcmp(argv[1], "snapt")) {
        const int N = 48000, kPeriod = 4800;
        std::vector<float> st(N, 0.0f);
        for (int o = 0; o < N; o += kPeriod)            // 200-sample decaying click
            for (int i = 0; i < 200 && o + i < N; ++i)
                st[o + i] = 0.9f * expf(-float(i) / 40.0f) * ((i & 1) ? -1.0f : 1.0f);
        od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();

        float means[2];
        for (int pass = 0; pass < 2; ++pass) {
            const float snap = pass ? 1.0f : 0.0f;
            Dirac d; d.setSample(&smp);
            Ctl c[] = {{"Playhead",0.25f},{"Rate",8.0f},{"GrainLen",0.02f},{"Grains",16.0f},
                       {"Level",0.7f},{"PosJtr",1.0f},{"Texture",0.5f},{"Snap",snap}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            double acc = 0.0; int n = 0;
            bool seen[dirac::Dirac::kMaxGrains] = {false};
            for (int b = 0; b < 400; ++b) {
                d.process();
                for (int i = 0; i < dirac::Dirac::kMaxGrains; ++i) {
                    if (d.isGrainActive(i) && !seen[i]) {
                        seen[i] = true;
                        float p = d.getGrainSpawnPos(i);
                        p -= float(N) * floorf(p / float(N));
                        float r = p - float(kPeriod) * floorf(p / float(kPeriod));
                        if (r > kPeriod * 0.5f) r = float(kPeriod) - r;
                        acc += r; n++;
                    } else if (!d.isGrainActive(i)) seen[i] = false;
                }
            }
            means[pass] = n ? float(acc / n) : -1.0f;
            printf("snap=%.0f  grains=%d  mean|start-onset| = %.1f samples\n", snap, n, means[pass]);
        }
        const bool pass = (means[1] < means[0] * 0.6f);
        printf("snapt: %s (snap pulls starts %.1fx closer to onsets)\n",
               pass ? "PASS" : "FAIL", means[1] > 0 ? means[0] / means[1] : 0.0f);
        return pass ? 0 : 1;
    }



    // ── difft: Diffuse spreads a click-grain in time, conserving energy ─────
    if (!strcmp(argv[1], "difft")) {
        const int N = 48000;
        std::vector<float> st(N, 0.0f);
        for (int i = 0; i < 400; ++i)
            st[i] = 0.9f * expf(-float(i) / 60.0f) * ((i & 1) ? -1.0f : 1.0f);
        od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
        double spread[2], energy[2]; int nf = 0;
        for (int pass = 0; pass < 2; ++pass) {
            Dirac d; rng = 0xd1ff; d.setSample(&smp);
            if (pass) d.mDiffuseOpt.set(2);
            Ctl c[] = {{"Playhead",0.0f},{"Rate",0.0f},{"GrainLen",0.02f},{"Grains",1.0f},
                       {"Level",0.9f},{"Texture",0.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            // Fire on block 0, off after — and MEASURE the fire block too: the
            // click grain dumps most of its energy there, and the diffuser
            // delays that energy into later blocks (first difft version missed
            // it in the dry pass and misread the smear as gain).
            double e = 0.0, et = 0.0, ett = 0.0; long t = 0;
            for (int b = 0; b < 400; ++b) {
                Ctl fc[] = {{"Fire", (b == 0) ? 1.0f : 0.0f}};
                setInputs(d, fc, 1);
                d.process();
                float *L = d.mOutL.buffer();
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx, ++t) {
                    const float x = L[sIdx];
                    if (!std::isfinite(x)) nf++;
                    const double w = double(x) * x;
                    e += w; et += w * t; ett += w * double(t) * t; } }
            const double mean = (e > 0.0) ? et / e : 0.0;
            spread[pass] = (e > 0.0) ? sqrt(std::max(0.0, ett / e - mean * mean)) : 0.0;
            energy[pass] = e;
            printf("diffuse=%d  temporal spread = %.0f samples  energy = %.4f\n",
                   pass, spread[pass], energy[pass]);
        }
        const double eRatio = (energy[0] > 0.0) ? energy[1] / energy[0] : 0.0;
        const bool pass = spread[1] > spread[0] * 2.0
                       && eRatio > 0.7 && eRatio < 1.4 && nf == 0;
        printf("difft: %s (spread x%.1f, energy x%.2f)\n", pass ? "PASS" : "FAIL",
               spread[0] > 0 ? spread[1] / spread[0] : 0.0, eRatio);
        return pass ? 0 : 1;
    }

    // ── freezet: Freeze locks the ring — the cloud survives silent input ────
    if (!strcmp(argv[1], "freezet")) {
        double late[2]; int nf = 0;
        for (int pass = 0; pass < 2; ++pass) {          // 0 = running, 1 = frozen
            Dirac d; rng = 0xf00d;
            // Playhead 0 = tightest delay (one grain-length back), so grains sit
            // inside the tone-filled span of the ring; 100 blocks of fill cover
            // delay + grain length with margin.
            Ctl c[] = {{"Playhead",0.0f},{"Rate",6.0f},{"GrainLen",0.1f},{"Grains",12.0f},
                       {"Level",0.8f},{"Texture",0.5f},{"Mix",1.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            for (int b = 0; b < 100; ++b) {             // fill the ring with tone
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx)
                    d.mIn.buffer()[sIdx] = 0.5f * sinf(2.0f * 3.14159265f * 110.0f
                                            * (b * FRAMELENGTH + sIdx) / 48000.0f);
                d.process();
            }
            if (pass) d.mFreezeOpt.set(2);
            double acc = 0.0; long n = 0;
            for (int b = 0; b < 800; ++b) {             // input goes silent
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) d.mIn.buffer()[sIdx] = 0.0f;
                d.process();
                float *L = d.mOutL.buffer();
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                    if (!std::isfinite(L[sIdx])) nf++;
                    if (b >= 400) { acc += double(L[sIdx]) * L[sIdx]; n++; } } }
            late[pass] = n ? sqrt(acc / n) : 0.0;
            printf("frozen=%d  late RMS = %.4f\n", pass, late[pass]);
        }
        const bool pass = late[1] > 10.0 * std::max(late[0], 1e-5) && nf == 0;
        printf("freezet: %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }

    // ── sfbt: sample-mode feedback — regen without lane bleed (v0.2.6) ──────
    // A: BLEED regression — silent sample + tone on the lane input + fdbk 0.9:
    //    output must stay silent (v0.2.2–0.2.5 mixed lane input into the regen
    //    ring, so upstream units bled into the cloud whenever fdbk was up).
    // B: regen still works, self-bootstrapped from the SAMPLE — content in the
    //    first 10%, granulate it (wet fills the ring via feedback), then move
    //    the playhead to silence: fdbk 0 dies, fdbk 0.9 sustains the tail.
    if (!strcmp(argv[1], "sfbt")) {
        const int N = 48000;
        int nf = 0;
        double bleed = 0.0;
        {   // A: bleed check
            std::vector<float> st(N, 0.0f);
            od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
            Dirac d; rng = 0x5fb7; d.setSample(&smp);
            Ctl c[] = {{"Playhead",0.5f},{"Rate",8.0f},{"GrainLen",0.15f},{"Grains",16.0f},
                       {"Level",0.9f},{"Texture",0.5f},{"Feedback",0.9f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            double acc = 0.0; long n = 0;
            for (int b = 0; b < 600; ++b) {
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx)
                    d.mIn.buffer()[sIdx] = 0.5f * sinf(2.0f * 3.14159265f * 330.0f
                                            * (b * FRAMELENGTH + sIdx) / 48000.0f);
                d.process();
                float *L = d.mOutL.buffer();
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                    if (!std::isfinite(L[sIdx])) nf++;
                    if (b >= 100) { acc += double(L[sIdx]) * L[sIdx]; n++; } } }
            bleed = n ? sqrt(acc / n) : 0.0;
            printf("bleed check: silent sample + lane tone, fdbk 0.9 -> RMS %.5f\n", bleed);
        }
        double late[2];
        for (int pass = 0; pass < 2; ++pass) {   // B: self-bootstrapped regen
            std::vector<float> st(N, 0.0f);
            for (int i = 0; i < N / 10; ++i)
                st[i] = 0.7f * sinf(2.0f * 3.14159265f * 220.0f * i / 48000.0f);
            od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
            Dirac d; rng = 0x5fb8; d.setSample(&smp);
            Ctl c[] = {{"Playhead",0.05f},{"Rate",8.0f},{"GrainLen",0.15f},{"Grains",16.0f},
                       {"Level",0.9f},{"Texture",0.5f},{"Feedback",pass?0.9f:0.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            for (int b = 0; b < 100; ++b) d.process();       // granulate the content
            Ctl mv[] = {{"Playhead",0.6f}}; setInputs(d, mv, 1);  // park in silence
            double acc = 0.0; long n = 0;
            for (int b = 0; b < 600; ++b) {
                d.process();
                float *L = d.mOutL.buffer();
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                    if (!std::isfinite(L[sIdx])) nf++;
                    if (b >= 100) { acc += double(L[sIdx]) * L[sIdx]; n++; } } }
            late[pass] = n ? sqrt(acc / n) : 0.0;
            printf("regen: playhead in silence, fdbk=%.1f -> late RMS %.5f\n",
                   pass ? 0.9f : 0.0f, late[pass]);
        }
        const bool pass = bleed < 0.005 && late[0] < 0.005
                       && late[1] > 20.0 * std::max(late[0], 1e-6)
                       && late[1] < 10.0 && nf == 0;
        printf("sfbt: %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }

    // ── headscale: diatonic head ladder (v0.2.7) ────────────────────────────
    // 4 heads, htune 4, psprd 0, semi 0. Scale MAJOR must voice {0,4,7,12}
    // (triad + octave); Scale OFF must keep the chromatic {0,4,8,12}.
    if (!strcmp(argv[1], "headscale")) {
        std::vector<float> st; od::Sample smp = makeSample(st, 48000, 1);
        int bad = 0;
        for (int pass = 0; pass < 2; ++pass) {
            const float expct[2][4] = {{0,4,8,12},{0,4,7,12}};
            Dirac d; rng = 0x0dda; d.setSample(&smp);
            d.mMultiHeadOpt.set(2);
            Ctl c[] = {{"Playhead",0.3f},{"Rate",8.0f},{"GrainLen",0.05f},{"Grains",16.0f},
                       {"Level",0.7f},{"Texture",0.5f},{"Psprd",0.0f},{"SemiShift",0.0f},
                       {"Heads",4.0f},{"HeadSpread",0.2f},{"HeadTune",4.0f},
                       {"Scale",pass?2.0f:0.0f}};   // 2 = major
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            bool seenPitch[4] = {false,false,false,false};
            bool seen[dirac::Dirac::kMaxGrains] = {false};
            for (int b = 0; b < 200; ++b) {
                d.process();
                for (int i = 0; i < dirac::Dirac::kMaxGrains; ++i) {
                    if (d.isGrainActive(i) && !seen[i]) {
                        seen[i] = true;
                        const float ps = d.getGrainPitch(i);
                        bool ok = false;
                        for (int k = 0; k < 4; ++k)
                            if (fabsf(ps - expct[pass][k]) < 0.01f) { seenPitch[k] = true; ok = true; }
                        if (!ok) { bad++; printf("  unexpected pitch %.2f (scale=%d)\n", ps, pass?2:0); }
                    } else if (!d.isGrainActive(i)) seen[i] = false;
                }
            }
            int distinct = 0; for (int k = 0; k < 4; ++k) if (seenPitch[k]) distinct++;
            printf("scale=%s  htune=4  voicing {%g,%g,%g,%g}  rungs seen=%d/4\n",
                   pass?"major":"off", expct[pass][0],expct[pass][1],expct[pass][2],expct[pass][3], distinct);
            if (distinct != 4) bad++;
        }
        printf("headscale: %s\n", bad ? "FAIL" : "PASS");
        return bad ? 1 : 0;
    }

    // ── fbdc: multi-head feedback must not drown in LF (v0.2.8 regression) ──
    // Live mode, 4 heads, fdbk 0.95, tone+noise held for ~40 s. Without the
    // 120 Hz loop high-pass the output degenerates to an LF pedestal: audio
    // fraction (first-difference ratio) decayed to ~0.18 and |block mean| hit
    // ~0.24 — the user-reported "fades out like an overload".
    if (!strcmp(argv[1], "fbdc")) {
        Dirac d; rng = 0x1234;
        d.mMultiHeadOpt.set(2);
        Ctl c[] = {{"Playhead",0.1f},{"Rate",4.0f},{"Grains",12.0f},{"GrainLen",0.2f},
                   {"Level",0.7f},{"Texture",0.5f},{"Feedback",0.95f},{"Spread",0.5f},
                   {"Mix",1.0f},{"Heads",4.0f},{"HeadSpread",0.5f}};
        setInputs(d, c, sizeof(c)/sizeof(c[0]));
        double rms = 0.0, dc = 0.0, hf = 0.0; long n = 0; float prev = 0.0f; long dcN = 0;
        const int B = 15000;                       // ~40 s
        int nf = 0;
        for (int b = 0; b < B; ++b) {
            for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                const int nn = b * FRAMELENGTH + sIdx;
                d.mIn.buffer()[sIdx] = 0.3f * sinf(2.0f * 3.14159265f * 220.0f * nn / 48000.0f)
                                     + 0.15f * frand();
            }
            d.process();
            if (b >= B - 3000) {                   // last ~8 s
                float *L = d.mOutL.buffer();
                double m = 0.0;
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                    if (!std::isfinite(L[sIdx])) nf++;
                    const float df = L[sIdx] - prev;
                    rms += double(L[sIdx]) * L[sIdx]; hf += double(df) * df;
                    m += L[sIdx]; prev = L[sIdx]; n++;
                }
                dc += fabs(m / FRAMELENGTH); dcN++;
            }
        }
        const double hfFrac = (rms > 0.0) ? sqrt(hf / rms) : 0.0;
        const double dcAvg  = dcN ? dc / dcN : 0.0;
        printf("fbdc: 4 heads, fdbk 0.95, 40 s -> RMS %.3f  |blockDC| %.3f  audioFrac %.3f\n",
               sqrt(rms / n), dcAvg, hfFrac);
        const bool pass = hfFrac > 0.35 && dcAvg < 0.06 && sqrt(rms / n) > 0.1 && nf == 0;
        printf("fbdc: %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }

    // ── windowt: fixed grain windows (v0.2.9) ───────────────────────────────
    // CONSTANT (DC 0.5) sample + one triggered grain at Spread 0, Level 1:
    // the output IS the window (x 0.5). Verify each shape's signature, plus
    // "normal" staying on the Texture path.
    if (!strcmp(argv[1], "windowt")) {
        const int N = 48000;
        std::vector<float> st(N, 0.5f);
        od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
        int fails = 0, nf = 0;
        const char *names[8] = {"","bell","sinc","tri","decay","ramp","tukey","square"};
        for (int w = 1; w <= 7; ++w) {
            Dirac d; rng = 0x111; d.setSample(&smp);
            Ctl c[] = {{"Playhead",0.3f},{"Rate",0.0f},{"GrainLen",0.2f},{"Grains",1.0f},
                       {"Level",1.0f},{"Spread",0.0f},{"Texture",0.5f},{"PosJtr",0.0f},
                       {"Window",float(w)}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            Ctl f1[] = {{"Fire",1.0f}}; setInputs(d, f1, 1);
            d.process();
            Ctl f0[] = {{"Fire",0.0f}}; setInputs(d, f0, 1);
            std::vector<float> env;
            for (int b = 0; b < 80; ++b) {           // 0.2 s grain = 75 blocks
                d.process();
                const float *L = d.mOutL.buffer();
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                    if (!std::isfinite(L[sIdx])) nf++;
                    env.push_back(L[sIdx] * 2.0f);   // undo the 0.5 source
                }
            }
            // trim to the active span (first/last |e| > 1e-4)
            int a = 0, b2 = int(env.size()) - 1;
            while (a < b2 && fabsf(env[a])  < 1e-4f) a++;
            while (b2 > a && fabsf(env[b2]) < 1e-4f) b2--;
            const int n = b2 - a + 1;
            if (n < 1000) { printf("  [FAIL] %s: no grain rendered\n", names[w]); fails++; continue; }
            auto at = [&](float frac){ return env[a + int(frac * (n - 1))]; };
            float mn = 1e9f, mx = -1e9f; int mxi = 0;
            for (int i = 0; i < n; ++i) {
                if (env[a+i] > mx) { mx = env[a+i]; mxi = i; }
                if (env[a+i] < mn) mn = env[a+i];
            }
            const float peakPos = float(mxi) / float(n - 1);
            bool ok = true; char why[64] = "";
            switch (w) {
              case 1:  ok = fabsf(peakPos-0.5f) < 0.06f && mn > -0.01f
                          && fabsf(at(0.5f) - 1.0f) < 0.05f;                  strcpy(why,"peak centre, unsigned"); break;
              case 2:  ok = mn < -0.15f && fabsf(peakPos-0.5f) < 0.06f;        strcpy(why,"signed lobes, peak centre"); break;
              case 3:  ok = fabsf(peakPos-0.5f) < 0.06f
                          && fabsf(at(0.25f) - 0.5f) < 0.06f;                  strcpy(why,"linear to centre"); break;
              case 4:  ok = peakPos < 0.06f && at(0.5f) < 0.10f && mn > -0.01f; strcpy(why,"instant attack, expo fall"); break;
              case 5:  ok = peakPos > 0.94f && at(0.5f) < 0.10f && mn > -0.01f; strcpy(why,"expo rise, hard stop"); break;
              case 6:  ok = fabsf(at(0.5f)-1.0f) < 0.02f
                          && fabsf(at(0.25f)-1.0f) < 0.02f && at(0.02f) < 0.9f; strcpy(why,"plateau, 12.5% tapers"); break;
              case 7:  ok = fabsf(at(0.5f)-1.0f) < 0.02f
                          && fabsf(at(0.03f)-1.0f) < 0.02f
                          && fabsf(at(0.97f)-1.0f) < 0.02f;                    strcpy(why,"flat to the edges"); break;
            }
            printf("  [%s] %-6s peak@%.2f min %+.2f  (%s)\n",
                   ok ? "PASS" : "FAIL", names[w], peakPos, mn, why);
            if (!ok) fails++;
        }
        printf("windowt: %s (nonFinite=%d)\n", (fails == 0 && nf == 0) ? "PASS" : "FAIL", nf);
        return (fails == 0 && nf == 0) ? 0 : 1;
    }

    // ── tiltt: envelope peak position (v0.2.10) ─────────────────────────────
    // DC sample + bell window: measured peak position must track the Tilt
    // input (bipolar: −0.8 → 0.1, 0 → 0.5, +0.8 → 0.9).
    if (!strcmp(argv[1], "tiltt")) {
        const int N = 48000;
        std::vector<float> st(N, 0.5f);
        od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
        int fails = 0, nf = 0;
        const float tin[3]  = {-0.8f, 0.0f, 0.8f};
        const float want[3] = { 0.10f, 0.50f, 0.90f};
        for (int k = 0; k < 3; ++k) {
            Dirac d; rng = 0x717; d.setSample(&smp);   // bell via the Window inlet
            Ctl c[] = {{"Playhead",0.3f},{"Rate",0.0f},{"GrainLen",0.2f},{"Grains",1.0f},
                       {"Level",1.0f},{"Spread",0.0f},{"Texture",0.5f},{"PosJtr",0.0f},
                       {"Tilt",tin[k]},{"Window",1.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            Ctl f1[] = {{"Fire",1.0f}}; setInputs(d, f1, 1);
            d.process();
            Ctl f0[] = {{"Fire",0.0f}}; setInputs(d, f0, 1);
            std::vector<float> env;
            for (int b = 0; b < 80; ++b) {
                d.process();
                const float *L = d.mOutL.buffer();
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                    if (!std::isfinite(L[sIdx])) nf++;
                    env.push_back(L[sIdx]);
                }
            }
            int a = 0, b2 = int(env.size()) - 1;
            while (a < b2 && fabsf(env[a])  < 1e-4f) a++;
            while (b2 > a && fabsf(env[b2]) < 1e-4f) b2--;
            const int n = b2 - a + 1;
            int mxi = 0; float mx = -1e9f;
            for (int i = 0; i < n; ++i) if (env[a+i] > mx) { mx = env[a+i]; mxi = i; }
            const float peakPos = float(mxi) / float(n - 1);
            const bool ok = (n > 1000) && fabsf(peakPos - want[k]) < 0.05f;
            printf("  [%s] tilt %+.1f -> peak@%.2f (want %.2f)\n",
                   ok ? "PASS" : "FAIL", tin[k], peakPos, want[k]);
            if (!ok) fails++;
        }
        // TiltSpread: 12 sequential single grains at tilt 0 — tsprd 0 must
        // put every peak at ~0.5; tsprd 1 must SCATTER them (stddev check).
        for (int pass = 0; pass < 2; ++pass) {
            Dirac d; rng = 0x5ca7; d.setSample(&smp);
            Ctl c[] = {{"Playhead",0.3f},{"Rate",0.0f},{"GrainLen",0.05f},{"Grains",1.0f},
                       {"Level",1.0f},{"Spread",0.0f},{"Texture",0.5f},{"PosJtr",0.0f},
                       {"Tilt",0.0f},{"TiltSpread",pass?1.0f:0.0f},{"Window",1.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            float peaks[12]; int np = 0;
            for (int g = 0; g < 12; ++g) {
                Ctl f1[] = {{"Fire",1.0f}}; setInputs(d, f1, 1);
                d.process();
                Ctl f0[] = {{"Fire",0.0f}}; setInputs(d, f0, 1);
                std::vector<float> env;
                for (int b = 0; b < 22; ++b) {          // 0.05 s grain + margin
                    d.process();
                    const float *L = d.mOutL.buffer();
                    for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                        if (!std::isfinite(L[sIdx])) nf++;
                        env.push_back(L[sIdx]);
                    }
                }
                int a = 0, b2 = int(env.size()) - 1;
                while (a < b2 && fabsf(env[a])  < 1e-4f) a++;
                while (b2 > a && fabsf(env[b2]) < 1e-4f) b2--;
                const int n = b2 - a + 1;
                if (n < 500) continue;
                int mxi = 0; float mx = -1e9f;
                for (int i = 0; i < n; ++i) if (env[a+i] > mx) { mx = env[a+i]; mxi = i; }
                peaks[np++] = float(mxi) / float(n - 1);
            }
            float mean = 0, sd = 0;
            for (int i = 0; i < np; ++i) mean += peaks[i];
            mean /= np;
            for (int i = 0; i < np; ++i) sd += (peaks[i]-mean)*(peaks[i]-mean);
            sd = sqrtf(sd / np);
            const bool ok = pass ? (sd > 0.15f) : (sd < 0.02f && fabsf(mean-0.5f) < 0.03f);
            printf("  [%s] tsprd=%d  %d grains  peak mean %.2f sd %.3f\n",
                   ok ? "PASS" : "FAIL", pass, np, mean, sd);
            if (!ok) fails++;
        }
        printf("tiltt: %s (nonFinite=%d)\n", (fails == 0 && nf == 0) ? "PASS" : "FAIL", nf);
        return (fails == 0 && nf == 0) ? 0 : 1;
    }

    // ── winbankt: Texture as each shape's macro (v0.2.10 banks) ─────────────
    // DC sample, single grain, Texture 0 vs 1 in three shape modes:
    //   sinc  — ring density: zero-crossings rise sharply
    //   decay — fall time: value at 30% of the grain slow vs fast
    //   tukey — taper: value at 5% square-ish vs hann-ish
    if (!strcmp(argv[1], "winbankt")) {
        const int N = 48000;
        std::vector<float> st(N, 0.5f);
        od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
        int fails = 0, nf = 0;
        struct Probe { int w; float tex; };
        auto render = [&](int w, float tex, std::vector<float> &env) {
            Dirac d; rng = 0xba7c; d.setSample(&smp);
            Ctl c[] = {{"Playhead",0.3f},{"Rate",0.0f},{"GrainLen",0.2f},{"Grains",1.0f},
                       {"Level",1.0f},{"Spread",0.0f},{"Texture",tex},{"PosJtr",0.0f},
                       {"Window",float(w)}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            Ctl f1[] = {{"Fire",1.0f}}; setInputs(d, f1, 1);
            d.process();
            Ctl f0[] = {{"Fire",0.0f}}; setInputs(d, f0, 1);
            env.clear();
            for (int b = 0; b < 80; ++b) {
                d.process();
                const float *L = d.mOutL.buffer();
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                    if (!std::isfinite(L[sIdx])) nf++;
                    env.push_back(L[sIdx] * 2.0f);
                }
            }
            int a = 0, b2 = int(env.size()) - 1;
            while (a < b2 && fabsf(env[a])  < 1e-4f) a++;
            while (b2 > a && fabsf(env[b2]) < 1e-4f) b2--;
            env = std::vector<float>(env.begin() + a, env.begin() + b2 + 1);
        };
        auto zc = [](const std::vector<float> &e) {
            int z = 0;
            for (size_t i = 1; i < e.size(); ++i)
                if ((e[i] >= 0.0f) != (e[i-1] >= 0.0f)) z++;
            return z;
        };
        auto at = [](const std::vector<float> &e, float f) {
            return e[size_t(f * (e.size() - 1))];
        };
        std::vector<float> e0, e1;
        render(2, 0.0f, e0); render(2, 1.0f, e1);           // sinc  (shape 2)
        {   const bool ok = zc(e1) >= zc(e0) + 6;
            printf("  [%s] sinc: ring density  zc %d -> %d\n", ok?"PASS":"FAIL", zc(e0), zc(e1));
            if (!ok) fails++; }
        render(4, 0.0f, e0); render(4, 1.0f, e1);           // decay (shape 4)
        {   const bool ok = at(e0, 0.3f) < 0.08f && at(e1, 0.3f) > 0.30f;
            printf("  [%s] decay: fall time  env@30%% %.3f -> %.3f\n", ok?"PASS":"FAIL",
                   at(e0,0.3f), at(e1,0.3f));
            if (!ok) fails++; }
        render(6, 0.0f, e0); render(6, 1.0f, e1);           // tukey (shape 6)
        {   const bool ok = at(e0, 0.05f) > 0.9f && at(e1, 0.05f) < 0.15f;
            printf("  [%s] tukey: taper  env@5%%  %.3f -> %.3f\n", ok?"PASS":"FAIL",
                   at(e0,0.05f), at(e1,0.05f));
            if (!ok) fails++; }
        printf("winbankt: %s (nonFinite=%d)\n", (fails == 0 && nf == 0) ? "PASS" : "FAIL", nf);
        return (fails == 0 && nf == 0) ? 0 : 1;
    }

    // ── nan (v0.2.11): poison every input with NaN / +inf / -inf in turn,
    // live and sample mode; the output must stay finite and the unit must
    // recover once the input is good again (standard §5). Until 0.2.10 the
    // control reads were not sanitized and this passed only because of the
    // order std::max/std::min compare in.
    if (!strcmp(argv[1], "nan")) {
        const int N = 48000 * 4;
        std::vector<float> st(N);
        for (int i = 0; i < N; ++i) st[i] = 0.5f * sinf(2.0f * 3.14159265f * 220.0f * float(i) / 48000.0f);
        int problems = 0, cases = 0;
        for (int mode = 0; mode < 2; ++mode) {
            for (int k = 0; ; ++k) {
                Dirac d;
                od::Inlet *mine[] = { &d.mIn, &d.mPlayheadIn, &d.mRateIn, &d.mPosJtrIn, &d.mSprdIn, &d.mDetuneIn, &d.mLevelIn,
                    &d.mRevProbIn, &d.mPsprdIn, &d.mTextureIn, &d.mGrainsIn, &d.mFireIn, &d.mHoldIn, &d.mSemiShiftIn,
                    &d.mGrainLenIn, &d.mMixIn, &d.mFeedbackIn, &d.mCompressIn, &d.mBinauralIn, &d.mScaleIn, &d.mVoctIn,
                    &d.mSpeedIn, &d.mHeadsIn, &d.mHeadSpreadIn, &d.mHeadTuneIn, &d.mSnapIn, &d.mTiltIn,
                    &d.mTiltSprdIn, &d.mWindowIn };
                const int nIn = int(sizeof(mine) / sizeof(mine[0]));
                if (k >= nIn) break;
                for (float poison : {NAN, INFINITY, -INFINITY}) {
                    Dirac e; rng = 0x7a7a;
                    od::Inlet *ein[] = { &e.mIn, &e.mPlayheadIn, &e.mRateIn, &e.mPosJtrIn, &e.mSprdIn, &e.mDetuneIn, &e.mLevelIn,
                        &e.mRevProbIn, &e.mPsprdIn, &e.mTextureIn, &e.mGrainsIn, &e.mFireIn, &e.mHoldIn, &e.mSemiShiftIn,
                        &e.mGrainLenIn, &e.mMixIn, &e.mFeedbackIn, &e.mCompressIn, &e.mBinauralIn, &e.mScaleIn, &e.mVoctIn,
                        &e.mSpeedIn, &e.mHeadsIn, &e.mHeadSpreadIn, &e.mHeadTuneIn, &e.mSnapIn, &e.mTiltIn,
                        &e.mTiltSprdIn, &e.mWindowIn };
                    od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
                    if (mode == 1) e.setSample(&smp);
                    const Ctl c[] = {{"Playhead",0.3f},{"Rate",4.0f},{"Level",1.0f},{"Texture",0.5f},{"Grains",12.0f},
                                     {"GrainLen",0.05f},{"Mix",1.0f},{"Spread",0.5f},{"Feedback",0.5f}};
                    setInputs(e, c, sizeof(c)/sizeof(c[0]));
                    for (int q = 0; q < FRAMELENGTH; ++q) e.mSpeedIn.buffer()[q] = 1.0f;
                    auto runb = [&](int nb, double &rms, int &nf) {
                        rms = 0; nf = 0; long cnt = 0;
                        for (int b = 0; b < nb; ++b) {
                            if (ein[k] != &e.mIn)
                                for (int q = 0; q < FRAMELENGTH; ++q) e.mIn.buffer()[q] = 0.5f * sinf(0.03f * float(b * FRAMELENGTH + q));
                            e.process();
                            for (int q = 0; q < FRAMELENGTH; ++q) {
                                const float v = e.mOutL.buffer()[q], w = e.mOutR.buffer()[q];
                                if (!std::isfinite(v) || !std::isfinite(w)) ++nf;
                                rms += double(v) * v; ++cnt;
                            }
                        }
                        rms = sqrt(rms / double(cnt));
                    };
                    double r0, r1, r2; int n0, n1, n2;
                    runb(200, r0, n0);
                    const float save = ein[k]->buffer()[0];
                    for (int q = 0; q < FRAMELENGTH; ++q) ein[k]->buffer()[q] = poison;
                    runb(20, r1, n1);
                    for (int q = 0; q < FRAMELENGTH; ++q) ein[k]->buffer()[q] = save;
                    runb(400, r2, n2);
                    ++cases;
                    const bool bad = n0 || n1 || n2 || (r0 > 0.01 && r2 < 0.2 * r0);
                    if (bad) {
                        ++problems;
                        printf("  FAIL %s %-10s %s: rms before %.3f after %.3f, nonfinite %d/%d\n", mode ? "sample" : "live",
                               ein[k]->mName, std::isnan(poison) ? "nan" : (poison > 0 ? "+inf" : "-inf"), r0, r2, n1, n2);
                    }
                    e.setSample(nullptr);
                }
                (void)d;
            }
        }
        printf("nan: %s (%d problems over %d cases)\n", problems ? "FAIL" : "PASS", problems, cases);
        return problems ? 1 : 0;
    }

    // ── pitch (v0.2.11): a grain plays at its transposition anywhere in a
    // long sample, and in live mode. Until 0.2.10 the read position was one
    // float: +1 st was 5 cents sharp from ~1 s and played at UNISON past
    // 21.8 s (2^20 samples). Single 1 s grain on a 60 s 220 Hz sine, pitch by
    // zero crossings; limit 0.5 cents.
    if (!strcmp(argv[1], "pitch")) {
        int fails = 0;
        auto measure = [](const std::vector<float> &out, size_t from, size_t to) {
            double first = -1, last = -1; int n = 0;
            for (size_t i = from; i < to; ++i) if (out[i-1] < 0 && out[i] >= 0) {
                const double t = double(i - 1) + out[i-1] / (out[i-1] - out[i]);
                if (first < 0) first = t; last = t; ++n; }
            return (n > 1) ? (n - 1) * 48000.0 / (last - first) : 0.0;
        };
        const int N = 48000 * 60;
        std::vector<float> st(N);
        for (int i = 0; i < N; ++i) st[i] = float(sin(2.0 * 3.14159265358979 * 220.0 * double(i) / 48000.0));
        for (float semi : {1.0f, -5.0f, 7.0f}) for (float ph : {0.02f, 0.6f, 0.95f}) {
            Dirac d;
            od::Sample smp; smp.mSampleCount = N; smp.mChannelCount = 1; smp.mpData = st.data();
            d.setSample(&smp);
            const Ctl c[] = {{"Playhead",ph},{"Rate",0.0f},{"Level",1.0f},{"Texture",1.0f},{"Grains",1.0f},
                             {"GrainLen",1.0f},{"SemiShift",semi},{"Mix",1.0f},{"Spread",0.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            std::vector<float> out;
            for (int b = 0; b < 400; ++b) {
                for (int q = 0; q < FRAMELENGTH; ++q) d.mFireIn.buffer()[q] = (b == 0 && q < 4) ? 1.0f : 0.0f;
                d.process();
                for (int q = 0; q < FRAMELENGTH; ++q) out.push_back(d.mOutL.buffer()[q]);
            }
            const double f = measure(out, 9600, 38400), want = 220.0 * pow(2.0, semi / 12.0);
            const double cents = 1200.0 * log2(f / want);
            const bool ok = fabs(cents) < 0.5;
            if (!ok) ++fails;
            printf("  sample %+3.0f st at %4.1f s: %+7.2f cents  %s\n", semi, ph * 60.0f, cents, ok ? "ok" : "FAIL");
            d.setSample(nullptr);
        }
        for (float semi : {1.0f, -5.0f}) {
            Dirac d;
            const Ctl c[] = {{"Playhead",0.5f},{"Rate",0.0f},{"Level",1.0f},{"Texture",1.0f},{"Grains",1.0f},
                             {"GrainLen",0.5f},{"SemiShift",semi},{"Mix",1.0f},{"Spread",0.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            std::vector<float> out; long t = 0;
            for (int b = 0; b < 1600; ++b) {
                for (int q = 0; q < FRAMELENGTH; ++q, ++t) d.mIn.buffer()[q] = float(sin(2.0 * 3.14159265358979 * 220.0 * double(t) / 48000.0));
                for (int q = 0; q < FRAMELENGTH; ++q) d.mFireIn.buffer()[q] = (b == 1200 && q < 4) ? 1.0f : 0.0f;
                d.process();
                if (b >= 1200) for (int q = 0; q < FRAMELENGTH; ++q) out.push_back(d.mOutL.buffer()[q]);
            }
            const double f = measure(out, 4800, 19200), want = 220.0 * pow(2.0, semi / 12.0);
            const double cents = 1200.0 * log2(f / want);
            const bool ok = fabs(cents) < 0.5;
            if (!ok) ++fails;
            printf("  live   %+3.0f st:            %+7.2f cents  %s\n", semi, cents, ok ? "ok" : "FAIL");
        }
        printf("pitch: %s\n", fails ? "FAIL" : "PASS");
        return fails ? 1 : 0;
    }

#ifdef HAS_SPEED
    if (!strcmp(argv[1], "speed")) {
        if (argc < 3) { fprintf(stderr, "speed: rate argument?\n"); return 2; }
        // Ramp sample (sample[i] = i/count): windowed mean |out| tracks the scan
        // position directly. 2 s sample; report 0.25 s windows over 5 s.
        const float spd = float(atof(argv[2]));
        const float knobAt2s = (argc > 3) ? float(atof(argv[3])) : -1.0f;   // optional mid-run knob touch
        std::vector<float> st(96000);
        for (int i = 0; i < 96000; ++i) st[i] = float(i) / 96000.0f;
        od::Sample smp; smp.mSampleCount = 96000; smp.mChannelCount = 1; smp.mpData = st.data();
        Dirac d; rng = 0x5eed;
        d.setSample(&smp);
        const Ctl c[] = {{"Playhead",0.0f},{"Rate",8.0f},{"Level",1.0f},{"Texture",0.5f},
                         {"Grains",16.0f},{"GrainLen",0.02f},{"Speed",spd}};
        setInputs(d, c, sizeof(c)/sizeof(c[0]));
        const int blocksPerWin = int(0.25 * 48000 / FRAMELENGTH);
        printf("speed=%.2f:", spd);
        for (int w = 0; w < 20; ++w) {
            if (knobAt2s >= 0.0f && w == 8) {   // knob touch at t=2 s
                const Ctl k[] = {{"Playhead", knobAt2s}};
                setInputs(d, k, 1);
            }
            double sum = 0.0; long cnt = 0;
            for (int b = 0; b < blocksPerWin; ++b) {
                for (int s = 0; s < FRAMELENGTH; ++s) d.mIn.buffer()[s] = 0.0f;
                d.process();
                for (int s = 0; s < FRAMELENGTH; ++s) { sum += fabsf(d.mOutL.buffer()[s]); cnt++; }
            }
            printf(" %.3f", sum / cnt);
        }
        printf("\n");
        d.setSample(nullptr);
        return 0;
    }
#endif

    fprintf(stderr, "unknown mode\n");
    return 2;
}
