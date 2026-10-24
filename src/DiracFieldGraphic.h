// DiracFieldGraphic — phosphor "grain field" scope for Dirac.
//
// Each ACTIVE grain is plotted as a glowing point in a 2D field:
//   X = stereo field   (pan, scattered by Spread; split into an L/R pair by Detune)
//   Y = pitch          (the grain's transpose in semitones: Chord/Semitone/Psprd/HoldDetune)
//   brightness = grain envelope (phosphor accumulation + per-frame decay)
//
// So the number of dots reads the polyphony (Grains), the cloud's vertical
// smear is the pitch scatter, its horizontal spread is the stereo width, and
// Hold freezes the whole constellation. Monochrome — brightness only.
//
// Modeled directly on Habitat's MirrorPhosphorGraphic: header-only (inline
// virtuals), uint8 brightness buffer, fade −1 per frame, additive hits.

#pragma once

#include "Dirac.h"
#include <od/graphics/Graphic.h>
#include <od/graphics/constants.h>
#include <string.h>
#include <math.h>

namespace dirac
{

  class DiracFieldGraphic : public od::Graphic
  {
  public:
    DiracFieldGraphic(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpGrain(0) {}

    virtual ~DiracFieldGraphic()
    {
      if (mpGrain)
        mpGrain->release();
    }

    void follow(Dirac *p)
    {
      if (mpGrain)
        mpGrain->release();
      mpGrain = p;
      if (mpGrain)
        mpGrain->attach();
    }

    // X-axis mode (v0.2.0), toggled by a SHIFT tap on the field control.
    //   0 = STEREO   — X is the pan position (the original field)
    //   1 = POSITION — X is where the grain reads in the buffer/sample
    // Position mode is how you SEE Snap and multi-playhead working: the cloud
    // visibly collects onto transients as Snap comes up, and splits into columns
    // as HeadSpread opens. Clears the phosphor on change so the two mappings
    // don't smear into each other.
    void setAxisMode(int m)
    {
      const int v = (m != 0) ? 1 : 0;
      if (v != mAxisMode) { mAxisMode = v; mCleared = false; }
    }
    int getAxisMode() const { return mAxisMode; }

  private:
    Dirac *mpGrain;
    int    mAxisMode = 0;

    static const int kMaxW = 128;
    static const int kMaxH = 64;
    uint8_t mPixels[kMaxW * kMaxH];
    bool    mCleared = false;
    uint32_t mFrame  = 0;

    // Pitch axis half-span in semitones (±). Covers chord + moderate transpose.
    static constexpr float kPitchHalfSpan = 18.0f;

    // Grain-length → mark-length mapping (log-scaled over ~200..48000 samples).
    // Precomputed (logf isn't constexpr): logf(200)=5.2983, 1/(logf(48000)-logf(200))=0.18246.
    static constexpr float kLogLenMin = 5.2983f;
    static constexpr float kLogLenInv = 0.18246f;
    static const int       kMaxTri = 5;   // max triangle half-height (px)

    // Additive "impulse" splat — an upward-pointing filled triangle (a Dirac-comb
    // arrowhead) centred at (x,y). Wide bright base, narrowing to a bright apex on
    // top; `size` scales with the grain's envelope (and a little with grain length).
    // `tex` morphs the GLYPH into the grain's envelope shape, so the field reads
    // the Texture knob at a glance:
    //   0.0  Percussive — the wedge LEANS (fast attack, long decay)
    //   0.5  Hann       — a symmetric triangle (the classic Dirac arrowhead)
    //   1.0  Tukey      — a FLAT-TOPPED trapezoid (plateau window)
    // v0.2.5: `rev` flips the arrowhead to point DOWN (reverse grains — an
    // upside-down Dirac comb), `hollow` draws only the outline + solid base
    // (sample-mode REGEN grains, i.e. Feedback's share of the cloud).
    inline void triSplat(int w, int h, int x, int y, int amt, int size, float tex,
                         bool rev, bool hollow)
    {
      if (size < 1) size = 1;
      // Above 0.5 → grow a plateau (Tukey).  Below 0.5 → lean the wedge (percussive).
      const int   flat = (tex > 0.5f) ? int((tex - 0.5f) * 1.2f * float(size) + 0.5f) : 0;
      const float skew = (tex < 0.5f) ? (0.5f - tex) * 2.0f : 0.0f;   // 0 … 1
      for (int r = 0; r <= size; ++r)
      {
        // bottom (r=0) → apex on top (r=size); reverse grains point down.
        const int yy = rev ? (y - r + (size >> 1)) : (y + r - (size >> 1));
        int half = size - r;                    // base widest, apex a point …
        if (half < flat) half = flat;           // … unless a Tukey plateau caps it
        const int lean = int(skew * float(r) * 0.9f + 0.5f);   // percussive tilt
        int a = amt - (r >> 1);                 // slight fade toward the base
        if (a < 2) a = 2;
        if (hollow && r > 0 && half > 0)
        {
          addPix(w, h, x - half - lean, yy, a);  // outline only …
          addPix(w, h, x + half - lean, yy, a);
        }
        else
        {
          for (int dx = -half; dx <= half; ++dx) // … solid base row / filled glyph
            addPix(w, h, x + dx - lean, yy, a);
        }
      }
    }
    // Comb stem (v0.2.5): the thin vertical line under the arrowhead — ↑ — the
    // way a Dirac comb is actually drawn. Length = log grain length, so short
    // grains are bare points (clicks) and long grains stand on tall stems.
    inline void stemSplat(int w, int h, int x, int yTop, int len, int amt, bool rev)
    {
      for (int k = 1; k <= len; ++k)
        addPix(w, h, x, rev ? (yTop + k) : (yTop - k), amt);
    }
    inline void addPix(int w, int h, int x, int y, int amt)
    {
      if (x < 0 || x >= w || y < 0 || y >= h) return;
      int idx = y * w + x;
      int b = mPixels[idx] + amt;
      if (b > 15) b = 15;
      mPixels[idx] = (uint8_t)b;
    }

  public:
    virtual void draw(od::FrameBuffer &fb)
    {
      int w = mWidth  < kMaxW ? mWidth  : kMaxW;
      int h = mHeight < kMaxH ? mHeight : kMaxH;

      if (!mCleared) { memset(mPixels, 0, sizeof(mPixels)); mCleared = true; }

      // Phosphor decay — one level per frame (~0.4 s @ 40 fps). With Diffuse ON
      // it fades every OTHER frame: trails linger ~2× — the halo you hear is
      // the smear you see (v0.2.5).
      mFrame++;
      const bool slowFade = mpGrain && mpGrain->getDiffuseOn();
      if (!slowFade || (mFrame & 1u))
        for (int i = 0; i < w * h; i++)
          if (mPixels[i] > 0) mPixels[i]--;

      if (mpGrain)
      {
        const float cx     = (w - 1) * 0.5f;
        const float halfW  = (w - 1) * 0.46f;
        const float spread = mpGrain->getVizSpread();
        const float detune = mpGrain->getVizDetune();
        const float splitX = detune * (w * 0.45f); // detune → L/R split, scaled to panel width
        const bool  wide   = (spread > 0.01f) || (detune > 0.001f);
        const float tex    = mpGrain->getVizTexture();  // morphs the glyph shape
        const bool  sampleMode = mpGrain->isSampleMode();

        const int N = mpGrain->getGrainCount();
        for (int i = 0; i < N; i++)
        {
          if (!mpGrain->isGrainActive(i)) continue;

          float pan   = mpGrain->getGrainPan(i);               // [-1,1] (already × Spread)
          float pitch = mpGrain->getGrainPitch(i);
          float env   = mpGrain->getGrainEnvelope(i);
          if (!(pan == pan) || !(pitch == pitch)) continue;     // NaN guard

          // Y: higher pitch → higher on screen. (worldBottom + py, so larger py
          // is UP — hence + here, not −.)
          float ny = 0.5f + pitch / (2.0f * kPitchHalfSpan);
          if (ny < 0.0f) ny = 0.0f; else if (ny > 1.0f) ny = 1.0f;

          // Triangle size pulses with the grain envelope; longer grains read a
          // touch bigger. duration ∈ [~64 .. 96000] samples (log-scaled).
          float lenSamp = (float)mpGrain->getGrainDuration(i);
          if (lenSamp < 1.0f) lenSamp = 1.0f;
          float lt = (logf(lenSamp) - kLogLenMin) * kLogLenInv;  // 0 short … 1 long
          if (lt < 0.0f) lt = 0.0f; else if (lt > 1.0f) lt = 1.0f;
          int size = 1 + (int)(env * 3.0f + lt * 1.5f);          // 1..~5 px tall
          if (size > kMaxTri) size = kMaxTri;
          // Stem length: 0 px for the shortest grains → ~7 px for 1 s+ grains.
          const int stem = (int)(lt * 7.0f + 0.5f);

          int py = (int)(ny * (h - 1) + 0.5f);

          // Honest brightness (v0.2.5): envelope × the grain's REAL gain — so
          // Level dims the whole field, Compress visibly evens it out, and loud
          // vs quiet source bits read at a glance.
          float loud = env * mpGrain->getGrainGain(i) * 2.0f;
          if (loud > 1.0f) loud = 1.0f;
          // Filtered grains draw DIM (v0.2.5). Since Color's removal (v0.2.12)
          // that is binaural's shadowed far ear only (alphaMin down to ~0.5 at
          // full side): farther reads a touch darker, which is right.
          const float aMin = mpGrain->getGrainAlphaMin(i);
          float dim = 0.35f + 0.76f * (aMin - 0.075f);
          if (dim > 1.0f) dim = 1.0f; else if (dim < 0.35f) dim = 0.35f;
          int amt = 4 + (int)(loud * dim * 12.0f);              // bright mid-grain
          if (amt > 15) amt = 15;

          // Reverse grains point DOWN; sample-mode regen grains draw HOLLOW.
          const bool revG   = mpGrain->getGrainReverse(i);
          const bool hollow = sampleMode && mpGrain->getGrainLive(i);

          if (mAxisMode)
          {
            // POSITION mode: X = read position in the source, full panel width.
            // No L/R split — detune/spread are stereo concepts and mean nothing
            // on this axis; a single mark per grain keeps the columns readable.
            float pos = mpGrain->getGrainReadPos01(i);
            if (!(pos == pos)) continue;                        // NaN guard
            if (pos < 0.0f) pos = 0.0f; else if (pos > 1.0f) pos = 1.0f;
            const int xg = (int)(pos * (w - 1) + 0.5f);
            // Read-span trail (v0.2.5): a dim underline from WHERE THE GRAIN
            // SPAWNED to where it's reading now — grain length as the material
            // it actually covers. Grows as the grain plays; reverse grains grow
            // leftward. Skipped when the span wraps the buffer edge (a line
            // across the whole panel would lie).
            float sp = mpGrain->getGrainSpawnPos01(i);
            if (sp == sp && fabsf(pos - sp) <= 0.5f)
            {
              int xa = (int)(sp * (w - 1) + 0.5f), xb = xg;
              if (xa > xb) { const int tswap = xa; xa = xb; xb = tswap; }
              const int yTrail = (py > 1) ? (py - 1) : 0;
              for (int xt = xa; xt <= xb; ++xt)
                addPix(w, h, xt, yTrail, 3);
            }
            triSplat(w, h, xg, py, amt, size, tex, revG, hollow);
            continue;
          }

          int xc = (int)(cx + pan * halfW + 0.5f);
          if (wide)
          {
            triSplat(w, h, (int)(xc - splitX + 0.5f), py, amt, size, tex, revG, hollow);  // L
            triSplat(w, h, (int)(xc + splitX + 0.5f), py, amt, size, tex, revG, hollow);  // R
            const int yBaseW = revG ? (py + (size >> 1)) : (py - (size >> 1));
            stemSplat(w, h, (int)(xc - splitX + 0.5f), yBaseW, stem, 3 + (amt >> 2), revG);
            stemSplat(w, h, (int)(xc + splitX + 0.5f), yBaseW, stem, 3 + (amt >> 2), revG);
          }
          else
          {
            triSplat(w, h, xc, py, amt, size, tex, revG, hollow);   // mono
            const int yBase = revG ? (py + (size >> 1)) : (py - (size >> 1));
            stemSplat(w, h, xc, yBase, stem, 3 + (amt >> 2), revG);
          }
        }
      }

      // POSITION mode gets a dim baseline so the axis reads as an axis (and so
      // an empty field still shows you which mode you're in). v0.2.5: the dashes
      // CRAWL with the capture write head (~12 px/s) and park when Capture is
      // frozen — the axis itself shows recording state.
      if (mAxisMode)
      {
        const int phase = mpGrain ? ((mpGrain->getCapWritePos() >> 12) & 3) : 0;
        for (int x = 0; x < w; ++x)
          if (((x - phase) & 3) < 2)
            addPix(w, h, x, 0, 3);
      }

      // Freeze indicator (v0.2.5): a 5×5 snowflake in the top-right corner while
      // Capture is frozen — the one menu state that's invisible AND surprising
      // when forgotten (the ring silently ignores input). Stamped additively
      // into the phosphor every frame so it holds steady while frozen and
      // MELTS away through the decay when you unfreeze. Other options stay
      // icon-free on purpose: Diffuse shows itself in the trails, multi-head
      // in the stacked pitch rows — one glyph keeps the scope a scope.
      if (mpGrain && mpGrain->getFreezeOn() && w >= 8 && h >= 8)
      {
        static const uint8_t kFlake[5] = {  // ✳ plus + diagonal tips, 5×5
          0b10101,
          0b00100,
          0b11111,
          0b00100,
          0b10101,
        };
        const int x0 = w - 7, y0 = h - 7;   // top-right, 2 px in from the edge
        for (int r = 0; r < 5; ++r)
          for (int c = 0; c < 5; ++c)
            if (kFlake[r] & (1u << (4 - c)))
              addPix(w, h, x0 + c, y0 + 4 - r, 12);
      }

      // Background + phosphor render (monochrome; v is the 4-bit intensity).
      fb.fill(BLACK, mWorldLeft, mWorldBottom,
              mWorldLeft + mWidth - 1, mWorldBottom + mHeight - 1);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
          uint8_t v = mPixels[y * w + x];
          if (v > 0)
            fb.pixel(v, mWorldLeft + x, mWorldBottom + y);
        }
    }
  };

} // namespace dirac
