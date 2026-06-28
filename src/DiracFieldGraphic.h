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

  private:
    Dirac *mpGrain;

    static const int kMaxW = 128;
    static const int kMaxH = 64;
    uint8_t mPixels[kMaxW * kMaxH];
    bool    mCleared = false;

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
    inline void triSplat(int w, int h, int x, int y, int amt, int size)
    {
      if (size < 1) size = 1;
      for (int r = 0; r <= size; ++r)
      {
        const int yy   = y + r - (size >> 1);   // bottom (r=0) → apex on top (r=size)
        const int half = size - r;              // base is widest, apex is a point
        int a = amt - (r >> 1);                 // slight fade toward the base
        if (a < 2) a = 2;
        for (int dx = -half; dx <= half; ++dx)
          addPix(w, h, x + dx, yy, a);
      }
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

      // Phosphor decay — every pixel fades one level per frame (~0.4 s @ 40 fps).
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

          int xc = (int)(cx + pan * halfW + 0.5f);
          int py = (int)(ny * (h - 1) + 0.5f);
          int amt = 5 + (int)(env * 10.0f);                     // bright mid-grain
          if (amt > 15) amt = 15;

          if (wide)
          {
            triSplat(w, h, (int)(xc - splitX + 0.5f), py, amt, size);  // L image
            triSplat(w, h, (int)(xc + splitX + 0.5f), py, amt, size);  // R image
          }
          else
          {
            triSplat(w, h, xc, py, amt, size);                  // mono
          }
        }
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
