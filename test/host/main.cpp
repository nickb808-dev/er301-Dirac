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
                         &d.mVoctIn,
                         &d.mHeadsIn, &d.mHeadSpreadIn, &d.mHeadTuneIn,
                         &d.mSnapIn,
                         &d.mColorIn,
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
        // v0.2.2 torture: Color + Diffuse + sample-mode feedback + Snap +
        // Hold all at once, tiny sample, freeze toggled mid-flight,
        // detach with the diffuser running.
        {
            Dirac d; rng = 0x222;
            od::Sample smp = makeSample(st, 20000, 1);
            d.setSample(&smp);
            d.mDiffuseOpt.set(2);
            const Ctl c[] = {{"Playhead",0.9f},{"Rate",16.0f},{"Level",0.9f},{"Grains",16.0f},
                             {"GrainLen",2.0f},{"Feedback",0.95f},{"Color",1.0f},
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


    // ── colort: Color darkens the cloud (per-grain cutoff scatter) ──────────
    // Sine+noise sample; metric = first-difference RMS over RMS (HF ratio).
    if (!strcmp(argv[1], "colort")) {
        std::vector<float> st; od::Sample smp = makeSample(st, 48000, 1);
        float hf[2]; int nf = 0;
        for (int pass = 0; pass < 2; ++pass) {
            Dirac d; rng = 0x5eed; d.setSample(&smp);
            Ctl c[] = {{"Playhead",0.3f},{"Rate",8.0f},{"GrainLen",0.05f},{"Grains",16.0f},
                       {"Level",0.7f},{"Texture",0.5f},{"Color",pass?1.0f:0.0f}};
            setInputs(d, c, sizeof(c)/sizeof(c[0]));
            double num = 0.0, den = 0.0; float prev = 0.0f;
            for (int b = 0; b < 800; ++b) { d.process();
                float *L = d.mOutL.buffer();
                for (int sIdx = 0; sIdx < FRAMELENGTH; ++sIdx) {
                    if (!std::isfinite(L[sIdx])) nf++;
                    const float df = L[sIdx] - prev;
                    num += df * df; den += L[sIdx] * L[sIdx]; prev = L[sIdx]; } }
            hf[pass] = (den > 0.0) ? float(sqrt(num / den)) : 0.0f;
            printf("color=%d  HF ratio = %.4f\n", pass, hf[pass]);
        }
        const bool pass = (hf[1] < hf[0] * 0.85f) && nf == 0;
        printf("colort: %s (%.2fx darker)\n", pass ? "PASS" : "FAIL",
               hf[1] > 0 ? hf[0] / hf[1] : 0.0f);
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
